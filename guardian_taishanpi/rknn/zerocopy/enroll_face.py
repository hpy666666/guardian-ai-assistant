"""
enroll_face.py — Guardian 人脸录入命令行工具
=============================================

在泰山派上通过 SSH 运行，支持两种录入方式：
  1. 摄像头实时拍摄（--capture）：打开摄像头，倒计时 3 秒拍一张
  2. 图片文件录入（--image 路径）：从现有图片提取人脸

用法示例：
    # 摄像头拍摄录入（3 秒倒计时）
    python3 enroll_face.py --name "张三" --capture

    # 从图片文件录入
    python3 enroll_face.py --name "张三" --image /tmp/photo.jpg

    # 追加多张图片（同一个人录入多张可提升识别准确率）
    python3 enroll_face.py --name "张三" --image /tmp/photo1.jpg
    python3 enroll_face.py --name "张三" --image /tmp/photo2.jpg

    # 查看当前人脸库
    python3 enroll_face.py --list

    # 删除某人的所有记录
    python3 enroll_face.py --delete "张三"

依赖：
    source ~/rknn/venv/bin/activate
    已有 SCRFD + ArcFace RKNN 模型文件
"""

import os
import sys
import argparse
import time
import logging

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s %(levelname)s %(message)s',
    datefmt='%H:%M:%S'
)
log = logging.getLogger(__name__)

# 把 zerocopy 父目录加入路径，使 lib 包可导入
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
import cv2
from zerocopy.lib.face_recognizer import FaceRecognizer, FACE_DB_PATH


# ── 摄像头拍摄 ───────────────────────────────────────────────────────

def capture_from_camera(device='/dev/video-camera0', countdown=3):
    """
    打开摄像头，倒计时 countdown 秒后拍一张，返回 BGR 图像。
    在没有显示器的环境（SSH）下，直接拍摄不显示预览。
    如果有 DISPLAY 环境变量，显示倒计时预览窗口。
    """
    cap = cv2.VideoCapture(device)
    if not cap.isOpened():
        # 尝试备用设备节点
        for fallback in ['/dev/video0', '/dev/video1', 0]:
            cap = cv2.VideoCapture(fallback)
            if cap.isOpened():
                log.info(f'使用备用摄像头: {fallback}')
                break
        else:
            log.error('无法打开摄像头，请检查设备节点')
            return None

    cap.set(cv2.CAP_PROP_FRAME_WIDTH,  1280)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)

    has_display = bool(os.environ.get('DISPLAY'))
    win_name    = 'Guardian - 人脸录入（倒计时）'

    if has_display:
        cv2.namedWindow(win_name, cv2.WINDOW_NORMAL)
        cv2.resizeWindow(win_name, 640, 360)

    log.info(f'摄像头已打开，{countdown} 秒后拍摄，请正面对准摄像头...')
    t_start = time.time()
    frame   = None

    while True:
        ret, frame = cap.read()
        if not ret or frame is None:
            log.warning('读取帧失败，重试...')
            time.sleep(0.1)
            continue

        elapsed   = time.time() - t_start
        remaining = countdown - int(elapsed)

        if has_display:
            disp = frame.copy()
            if remaining > 0:
                cv2.putText(disp, f'倒计时: {remaining}s  请正面对准摄像头',
                            (20, 50), cv2.FONT_HERSHEY_SIMPLEX,
                            1.2, (0, 255, 0), 2)
            cv2.imshow(win_name, disp)
            key = cv2.waitKey(30) & 0xFF
            if key == ord('q'):
                log.info('用户取消')
                cap.release()
                cv2.destroyAllWindows()
                return None
        else:
            # 无显示器：只打印倒计时
            if remaining > 0:
                sys.stdout.write(f'\r倒计时: {remaining}s  ')
                sys.stdout.flush()

        if elapsed >= countdown:
            break

    cap.release()
    if has_display:
        cv2.destroyAllWindows()

    print()  # 换行
    log.info('拍摄完成')
    return frame


# ── 主程序 ───────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description='Guardian 人脸录入工具',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python3 enroll_face.py --name "张三" --capture
  python3 enroll_face.py --name "张三" --image /tmp/photo.jpg
  python3 enroll_face.py --list
  python3 enroll_face.py --delete "张三"
        """
    )

    parser.add_argument('--name',    type=str, help='录入的姓名')
    parser.add_argument('--capture', action='store_true',
                        help='从摄像头拍摄（倒计时3秒）')
    parser.add_argument('--image',   type=str,
                        help='从图片文件录入，指定路径')
    parser.add_argument('--device',  type=str, default='/dev/video-camera0',
                        help='摄像头设备节点（默认 /dev/video-camera0）')
    parser.add_argument('--countdown', type=int, default=3,
                        help='拍摄倒计时秒数（默认 3）')
    parser.add_argument('--list',    action='store_true',
                        help='查看当前人脸库')
    parser.add_argument('--delete',  type=str, metavar='NAME',
                        help='删除指定姓名的所有记录')
    parser.add_argument('--db',      type=str, default=FACE_DB_PATH,
                        help=f'人脸库路径（默认 {FACE_DB_PATH}）')
    parser.add_argument('--scrfd',   type=str,
                        default='/home/lckfb/rknn/scrfd_500m_kps.rknn',
                        help='SCRFD 模型路径')
    parser.add_argument('--arcface', type=str,
                        default='/home/lckfb/rknn/mobilenet_arcface.rknn',
                        help='ArcFace 模型路径')

    args = parser.parse_args()

    # ── 初始化识别器 ──
    fr = FaceRecognizer(
        scrfd_path=args.scrfd,
        arcface_path=args.arcface,
        db_path=args.db
    )

    # ── --list 和 --delete 不需要加载模型 ──
    if args.list:
        fr.load_db(args.db)
        names = fr.db_list()
        if not names:
            print('人脸库为空')
        else:
            print(f'人脸库共 {fr.db_count} 条记录，涉及 {len(names)} 人：')
            from collections import Counter
            import numpy as np
            all_names = []
            if os.path.exists(args.db):
                data = np.load(args.db, allow_pickle=True)
                all_names = list(data['names'])
            counts = Counter(all_names)
            for n in sorted(counts):
                print(f'  {n}: {counts[n]} 条')
        return

    if args.delete:
        fr.load_db(args.db)
        deleted = fr.delete(args.delete)
        if deleted == 0:
            print(f'未找到 "{args.delete}" 的记录')
        else:
            print(f'已删除 "{args.delete}" 共 {deleted} 条记录')
            fr.save_db(args.db)
        return

    # ── 录入操作需要加载模型 ──
    if not args.name:
        parser.error('录入操作需要指定 --name')
    if not args.capture and not args.image:
        parser.error('录入操作需要指定 --capture 或 --image')

    print(f'正在加载 RKNN 模型（SCRFD + ArcFace）...')
    ok = fr.load_models()
    if not ok:
        print('模型加载失败，请检查模型文件路径和 RKNN 环境')
        sys.exit(1)

    fr.load_db(args.db)

    # ── 获取图像 ──
    if args.capture:
        bgr = capture_from_camera(args.device, args.countdown)
        if bgr is None:
            print('拍摄失败，退出')
            sys.exit(1)
        # 保存一份原始拍摄图以供确认
        save_path = f'/tmp/enroll_{args.name}_{int(time.time())}.jpg'
        cv2.imwrite(save_path, bgr)
        log.info(f'原始拍摄图已保存到: {save_path}')

    elif args.image:
        if not os.path.exists(args.image):
            print(f'图片文件不存在: {args.image}')
            sys.exit(1)
        bgr = cv2.imread(args.image)
        if bgr is None:
            print(f'无法读取图片: {args.image}')
            sys.exit(1)
        log.info(f'从图片读取: {args.image}，尺寸 {bgr.shape[1]}×{bgr.shape[0]}')

    # ── 执行录入 ──
    print(f'正在录入 "{args.name}" 的人脸...')
    success = fr.enroll(args.name, bgr)

    if success:
        fr.save_db(args.db)
        print(f'✓ 录入成功！人脸库现有 {fr.db_count} 条记录')
        print(f'  人员列表: {fr.db_list()}')
    else:
        print('✗ 录入失败：未能在图像中检测到清晰的人脸')
        print('  建议：')
        print('  1. 确保人脸正面对准摄像头，光线充足')
        print('  2. 距摄像头 0.5~2 米范围内')
        print('  3. 使用 --image 传入更清晰的照片')
        sys.exit(1)

    fr.release()


if __name__ == '__main__':
    main()
