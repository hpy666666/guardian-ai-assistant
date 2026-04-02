import os
import time
import queue
import aiohttp
import asyncio
import requests
import traceback
import numpy as np
from config.logger import setup_logging
from core.utils.tts import MarkdownCleaner
from core.providers.tts.base import TTSProviderBase
from core.utils import opus_encoder_utils, textUtils
from core.providers.tts.dto.dto import SentenceType, ContentType, InterfaceType

TAG = __name__
logger = setup_logging()

# IndexTTS 输出 24kHz，ESP32 硬件只支持 16kHz，在服务端降采样
_TTS_NATIVE_RATE = 24000
_TARGET_RATE = 16000


def _resample_pcm(pcm_bytes: bytes, src_rate: int, dst_rate: int) -> bytes:
    """将 int16 PCM 从 src_rate 重采样到 dst_rate（简单线性插值）"""
    if src_rate == dst_rate:
        return pcm_bytes
    samples = np.frombuffer(pcm_bytes, dtype=np.int16).astype(np.float32)
    n_src = len(samples)
    n_dst = int(n_src * dst_rate / src_rate)
    if n_dst == 0:
        return b""
    indices = np.linspace(0, n_src - 1, n_dst)
    lo = indices.astype(np.int32)
    hi = np.clip(lo + 1, 0, n_src - 1)
    frac = (indices - lo).astype(np.float32)
    resampled = samples[lo] + frac * (samples[hi] - samples[lo])
    return resampled.astype(np.int16).tobytes()


class TTSProvider(TTSProviderBase):
    def __init__(self, config, delete_audio_file):
        super().__init__(config, delete_audio_file)
        self.interface_type = InterfaceType.SINGLE_STREAM
        self.voice = config.get("voice", "xiao_he")
        if config.get("private_voice"):
            self.voice = config.get("private_voice")
        else:
            self.voice = config.get("voice", "xiao_he")
        self.api_url = config.get("api_url", "http://8.138.114.124:11996/tts")
        self.audio_format = "pcm"
        self.before_stop_play_files = []

        # 创建Opus编码器，使用 16kHz（与 ESP32 硬件输出采样率匹配）
        # IndexTTS 返回 24kHz PCM，在编码前先 resample 到 16kHz
        self.opus_encoder = opus_encoder_utils.OpusEncoderUtils(
            sample_rate=_TARGET_RATE, channels=1, frame_size_ms=60
        )

        # PCM缓冲区（存放 24kHz 原始数据，累积够一个 16kHz 帧再处理）
        self.pcm_buffer = bytearray()

    def tts_text_priority_thread(self):
        """流式文本处理线程"""
        while not self.conn.stop_event.is_set():
            try:
                message = self.tts_text_queue.get(timeout=1)
                if message.sentence_type == SentenceType.FIRST:
                    # 初始化参数
                    self.tts_stop_request = False
                    self.processed_chars = 0
                    self.tts_text_buff = []
                    self.before_stop_play_files.clear()
                elif ContentType.TEXT == message.content_type:
                    self.tts_text_buff.append(message.content_detail)
                    segment_text = self._get_segment_text()
                    if segment_text:
                        self.to_tts_single_stream(segment_text)

                elif ContentType.FILE == message.content_type:
                    logger.bind(tag=TAG).info(
                        f"添加音频文件到待播放列表: {message.content_file}"
                    )
                    if message.content_file and os.path.exists(message.content_file):
                        # 先处理文件音频数据
                        self._process_audio_file_stream(message.content_file, callback=lambda audio_data: self.handle_audio_file(audio_data, message.content_detail))

                if message.sentence_type == SentenceType.LAST:
                    # 处理剩余的文本
                    self._process_remaining_text_stream(True)

            except queue.Empty:
                continue
            except Exception as e:
                logger.bind(tag=TAG).error(
                    f"处理TTS文本失败: {str(e)}, 类型: {type(e).__name__}, 堆栈: {traceback.format_exc()}"
                )

    def _process_remaining_text_stream(self, is_last=False):
        """处理剩余的文本并生成语音
        Returns:
            bool: 是否成功处理了文本
        """
        full_text = "".join(self.tts_text_buff)
        remaining_text = full_text[self.processed_chars :]
        if remaining_text:
            segment_text = textUtils.get_string_no_punctuation_or_emoji(remaining_text)
            if segment_text:
                self.to_tts_single_stream(segment_text, is_last)
                self.processed_chars += len(full_text)
            else:
                self._process_before_stop_play_files()
        else:
            self._process_before_stop_play_files()

    def to_tts_single_stream(self, text, is_last=False):
        try:
            max_repeat_time = 5
            text = MarkdownCleaner.clean_markdown(text)
            try:
                asyncio.run(self.text_to_speak(text, is_last))
            except Exception as e:
                logger.bind(tag=TAG).warning(
                    f"语音生成失败{5 - max_repeat_time + 1}次: {text}，错误: {e}"
                )
                max_repeat_time -= 1

            if max_repeat_time > 0:
                logger.bind(tag=TAG).info(
                    f"语音生成成功: {text}，重试{5 - max_repeat_time}次"
                )
            else:
                logger.bind(tag=TAG).error(
                    f"语音生成失败: {text}，请检查网络或服务是否正常"
                )
        except Exception as e:
            logger.bind(tag=TAG).error(f"Failed to generate TTS file: {e}")
        finally:
            return None

    async def text_to_speak(self, text, is_last):
        """流式处理TTS音频，每句只推送一次音频列表"""
        payload = {"text": text, "character": self.voice}

        # 每句重置编码器状态，防止跨句状态污染
        self.opus_encoder.reset_state()
        self.pcm_buffer.clear()

        # 目标帧大小（16kHz, 60ms）对应的 24kHz 输入字节数
        # 16kHz帧: 16000*60/1000*2 = 1920 bytes → 对应 24kHz: 24000*60/1000*2 = 2880 bytes
        src_frame_bytes = int(_TTS_NATIVE_RATE * self.opus_encoder.channels * self.opus_encoder.frame_size_ms / 1000 * 2)

        try:
            # connect_timeout: 等待服务器响应头的时间（IndexTTS首次推理可能需要20-30s）
            # read_timeout: None 表示流式读取不超时（音频流可能持续数秒）
            tts_timeout = aiohttp.ClientTimeout(connect=30, sock_read=None, total=None)
            async with aiohttp.ClientSession() as session:
                async with session.post(self.api_url, json=payload, timeout=tts_timeout) as resp:

                    if resp.status != 200:
                        logger.bind(tag=TAG).error(
                            f"TTS请求失败: {resp.status}, {await resp.text()}"
                        )
                        self.tts_audio_queue.put((SentenceType.LAST, [], None))
                        return

                    self.tts_audio_queue.put((SentenceType.FIRST, [], text))

                    # 处理音频流数据：按 24kHz 帧大小累积，resample 后再 Opus 编码
                    async for chunk in resp.content.iter_any():
                        data = chunk[0] if isinstance(chunk, (list, tuple)) else chunk
                        if not data:
                            continue

                        self.pcm_buffer.extend(data)

                        while len(self.pcm_buffer) >= src_frame_bytes:
                            src_frame = bytes(self.pcm_buffer[:src_frame_bytes])
                            del self.pcm_buffer[:src_frame_bytes]

                            # 24kHz → 16kHz resample
                            dst_frame = _resample_pcm(src_frame, _TTS_NATIVE_RATE, _TARGET_RATE)

                            self.opus_encoder.encode_pcm_to_opus_stream(
                                dst_frame,
                                end_of_stream=False,
                                callback=self.handle_opus
                            )

                    # flush 剩余数据并结束本句编码
                    # 无论 pcm_buffer 是否有剩余，都必须调用 end_of_stream=True
                    # 以 flush 编码器内部的 lookahead buffer，确保最后几帧不丢失
                    flush_pcm = bytes(self.pcm_buffer) if self.pcm_buffer else b""
                    if flush_pcm:
                        flush_pcm = _resample_pcm(flush_pcm, _TTS_NATIVE_RATE, _TARGET_RATE)
                    self.opus_encoder.encode_pcm_to_opus_stream(
                        flush_pcm,
                        end_of_stream=True,
                        callback=self.handle_opus
                    )
                    self.pcm_buffer.clear()

                    # 如果是最后一段，输出音频获取完毕
                    if is_last:
                        self._process_before_stop_play_files()

        except Exception as e:
            logger.bind(tag=TAG).error(f"TTS请求异常: {e}")
            self.tts_audio_queue.put((SentenceType.LAST, [], None))

    def audio_to_pcm_data_stream(
        self, audio_file_path, callback=None
    ):
        """音频文件转换为PCM编码，使用16kHz采样率（与Opus编码器匹配）"""
        from core.utils.util import audio_to_data_stream
        return audio_to_data_stream(audio_file_path, is_opus=False, callback=callback, sample_rate=_TARGET_RATE, opus_encoder=None)

    def audio_to_opus_data_stream(
        self, audio_file_path, callback=None
    ):
        """音频文件转换为Opus编码，使用16kHz采样率和自己的编码器"""
        from core.utils.util import audio_to_data_stream
        return audio_to_data_stream(audio_file_path, is_opus=True, callback=callback, sample_rate=_TARGET_RATE, opus_encoder=self.opus_encoder)

    async def close(self):
        """资源清理"""
        await super().close()
        if hasattr(self, "opus_encoder"):
            self.opus_encoder.close()

    def to_tts(self, text: str) -> list:
        """非流式TTS处理，用于测试及保存音频文件的场景
        Args:
            text: 要转换的文本
        Returns:
            list: 返回opus编码后的音频数据列表
        """
        start_time = time.time()
        text = MarkdownCleaner.clean_markdown(text)

        payload = {"text": text, "character": self.voice}

        try:
            with requests.post(self.api_url, json=payload, timeout=30) as response:
                if response.status_code != 200:
                    logger.bind(tag=TAG).error(
                        f"TTS请求失败: {response.status_code}, {response.text}"
                    )
                    return []

                logger.info(f"TTS请求成功: {text}, 耗时: {time.time() - start_time}秒")

                # 使用opus编码器处理PCM数据
                opus_datas = []
                pcm_data = response.content

                # 计算每帧的字节数
                frame_bytes = int(
                    self.opus_encoder.sample_rate
                    * self.opus_encoder.channels
                    * self.opus_encoder.frame_size_ms
                    / 1000
                    * 2
                )

                # 分帧处理PCM数据
                for i in range(0, len(pcm_data), frame_bytes):
                    frame = pcm_data[i : i + frame_bytes]
                    if len(frame) < frame_bytes:
                        # 最后一帧可能不足，用0填充
                        frame = frame + b"\x00" * (frame_bytes - len(frame))

                    self.opus_encoder.encode_pcm_to_opus_stream(
                        frame,
                        end_of_stream=(i + frame_bytes >= len(pcm_data)),
                        callback=lambda opus: opus_datas.append(opus)
                    )

                return opus_datas

        except Exception as e:
            logger.bind(tag=TAG).error(f"TTS请求异常: {e}")
            return []