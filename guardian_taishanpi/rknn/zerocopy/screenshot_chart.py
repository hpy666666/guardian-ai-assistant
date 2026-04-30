"""
显示色卡并截屏，用于分析 MIPI 面板色彩偏差
运行方式：DISPLAY=:0 python3 screenshot_chart.py
截图保存到 /tmp/mipi_screenshot.png，scp 传回电脑分析
"""
import cv2
import time
import subprocess

CHART_PATH = '/tmp/color_chart.png'
SCREENSHOT_PATH = '/tmp/mipi_screenshot.png'

img = cv2.imread(CHART_PATH)
if img is None:
    print(f'错误：找不到色卡文件 {CHART_PATH}')
    print('请先从电脑 scp 传入：')
    print('  scp color_chart.png lckfb@192.168.1.26:/tmp/color_chart.png')
    exit(1)

cv2.namedWindow('chart', cv2.WINDOW_NORMAL)
cv2.setWindowProperty('chart', cv2.WND_PROP_FULLSCREEN, cv2.WINDOW_FULLSCREEN)
cv2.imshow('chart', img)
cv2.waitKey(500)  # 等画面渲染完成

print('色卡已显示，3秒后截屏...')
time.sleep(3)

ret = subprocess.run(['scrot', SCREENSHOT_PATH], capture_output=True)
if ret.returncode == 0:
    print(f'截图成功：{SCREENSHOT_PATH}')
    print('传回电脑命令（在电脑 PowerShell 执行）：')
    print(f'  scp lckfb@192.168.1.26:{SCREENSHOT_PATH} C:\\Users\\34376\\Desktop\\mipi_screenshot.png')
else:
    print(f'scrot 失败: {ret.stderr.decode()}')
    print('尝试安装：sudo apt install scrot -y')

cv2.destroyAllWindows()
