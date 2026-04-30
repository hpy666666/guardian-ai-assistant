"""
声纹注册脚本
===========
用法：
  python register.py <音频文件路径> [说话人ID]

例：
  python register.py my_voice.wav owner
  python register.py recording.wav owner

说明：
  - 音频建议 10-30 秒干净普通话，无背景音乐
  - 说话人ID默认为 "owner"（主人）
  - 注册后声纹向量保存到 speakers/embeddings.json
"""

import sys
import requests
from pathlib import Path

API_KEY    = "guardian_vp_key"
BASE_URL   = "http://127.0.0.1:8002"
SPEAKER_ID = "owner"


def register(audio_path: str, speaker_id: str = SPEAKER_ID):
    path = Path(audio_path)
    if not path.exists():
        print(f"[错误] 文件不存在: {audio_path}")
        sys.exit(1)

    print(f"正在注册声纹: {path.name} → speaker_id='{speaker_id}'")

    with open(path, "rb") as f:
        resp = requests.post(
            f"{BASE_URL}/voiceprint/register",
            params={"key": API_KEY},
            data={"speaker_id": speaker_id},
            files={"file": (path.name, f, "audio/wav")},
            timeout=30,
        )

    if resp.status_code == 200:
        result = resp.json()
        print(f"[成功] 声纹注册完成！")
        print(f"  说话人ID : {result['speaker_id']}")
        print(f"  向量维度  : {result['embedding_dim']}")
    else:
        print(f"[失败] HTTP {resp.status_code}: {resp.text}")
        sys.exit(1)


def list_speakers():
    resp = requests.get(
        f"{BASE_URL}/voiceprint/list",
        params={"key": API_KEY},
        timeout=5,
    )
    if resp.status_code == 200:
        speakers = resp.json().get("speakers", [])
        if speakers:
            print(f"已注册说话人: {', '.join(speakers)}")
        else:
            print("暂无已注册说话人")
    else:
        print(f"查询失败: {resp.status_code}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        print("当前已注册说话人：")
        list_speakers()
        sys.exit(0)

    audio_file = sys.argv[1]
    speaker_id = sys.argv[2] if len(sys.argv) > 2 else SPEAKER_ID
    register(audio_file, speaker_id)

    print("\n当前已注册说话人：")
    list_speakers()
