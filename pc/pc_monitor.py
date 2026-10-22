"""电脑性能数据采集：每秒经 USB 串口发一行给 ESP32，由 ESP32 画到串口屏上。

用法：
    python pc_monitor.py            自动找 ESP32（乐鑫 USB VID 303A）
    python pc_monitor.py COM3       指定串口
    python pc_monitor.py -v         同时打印 ESP32 回传的日志
依赖：pip install psutil pyserial
"""
import subprocess
import sys
import time

import psutil
import serial
from serial.tools import list_ports

ESPRESSIF_VID = 0x303A
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)  # 用 pythonw 运行时不弹黑框


def find_port():
    for p in list_ports.comports():
        if p.vid == ESPRESSIF_VID:
            return p.device
    return None


def read_gpu():
    """返回 (占用%, 温度, 显存已用GB, 显存总GB)，没有 NVIDIA 显卡返回 None"""
    try:
        out = subprocess.run(
            ["nvidia-smi", "--query-gpu=utilization.gpu,temperature.gpu,memory.used,memory.total",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=3, creationflags=NO_WINDOW,
        ).stdout.strip().splitlines()[0]
        util, temp, used, total = (float(x) for x in out.split(","))
        return util, temp, used / 1024, total / 1024
    except Exception:
        return None


def open_port(port):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0
    # DTR/RTS 都拉低再打开，避免把 ESP32 复位
    s.dtr = False
    s.rts = False
    s.open()
    return s


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    verbose = "-v" in sys.argv
    has_gpu = read_gpu() is not None
    psutil.cpu_percent(None)  # 第一次调用返回 0，先丢掉
    last_net = psutil.net_io_counters()
    last_t = time.time()
    ser = None

    while True:
        if ser is None:
            port = args[0] if args else find_port()
            if not port:
                print("没找到 ESP32，3 秒后重试")
                time.sleep(3)
                continue
            try:
                ser = open_port(port)
                print(f"已连接 {port}，开始发送（Ctrl+C 退出）")
            except serial.SerialException as e:
                print(f"打开 {port} 失败：{e}，3 秒后重试")
                time.sleep(3)
                continue

        time.sleep(max(0.0, 1 - (time.time() - last_t)))
        now = time.time()
        net = psutil.net_io_counters()
        dt = now - last_t
        up = (net.bytes_sent - last_net.bytes_sent) / dt
        down = (net.bytes_recv - last_net.bytes_recv) / dt
        last_net, last_t = net, now

        mem = psutil.virtual_memory()
        fields = {
            "cpu": f"{psutil.cpu_percent(None):.1f}",
            "ram": f"{mem.percent:.1f}",
            "ramu": f"{mem.used / 2**30:.1f}",
            "ramt": f"{mem.total / 2**30:.1f}",
            "up": f"{up:.0f}",
            "down": f"{down:.0f}",
            "time": time.strftime("%H:%M:%S"),
        }
        gpu = read_gpu() if has_gpu else None
        if gpu:
            fields.update(gpu=f"{gpu[0]:.0f}", gput=f"{gpu[1]:.0f}",
                          vramu=f"{gpu[2]:.1f}", vramt=f"{gpu[3]:.0f}")
        line = "M " + " ".join(f"{k}={v}" for k, v in fields.items()) + "\n"

        try:
            ser.write(line.encode())
            back = ser.read(4096)
            if verbose:
                print(line.strip())
                if back:
                    print(back.decode("utf-8", "replace").rstrip())
        except serial.SerialException as e:
            print(f"串口断开：{e}，重新连接")
            ser.close()
            ser = None


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
