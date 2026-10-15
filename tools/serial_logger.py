"""板子串口日志录制工具。

用途：随时抓 ESP32-S3 的串口输出（Web Serial 也会占同一个口，所以同一时刻只能一个占用者）。
- 带毫秒时间戳落盘到 logs/board-<时间戳>.log
- 默认抓 N 秒；--follow 则一直抓（Ctrl+C 停）

用法（本机 bash 是 WSL，但串口在 Windows 侧，需用 Windows 的 python）：
    python tools/serial_logger.py --port COM5 --seconds 30
    python tools/serial_logger.py --port COM5 --follow
"""

import argparse
import datetime
import os
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("需要 pyserial：请用 IDF 的 python 运行本脚本（自带 pyserial）")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--follow", action="store_true")
    ap.add_argument("--out-dir", default=None)
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    out_dir = args.out_dir or os.path.join(os.path.dirname(here), "logs")
    os.makedirs(out_dir, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = os.path.join(out_dir, "board-%s.log" % stamp)

    s = serial.Serial(args.port, args.baud, timeout=0.2)
    print("录制中 -> %s" % path)
    print("提示：串口是流式的，打开之前的输出已丢失，只能抓到打开之后的。")

    end = None if args.follow else time.time() + args.seconds
    pending = b""
    with open(path, "wb") as f:
        try:
            while end is None or time.time() < end:
                try:
                    d = s.read(4096)
                except Exception as e:
                    # 端口断开（常见于板子关机/开机导致 USB 重新枚举）：自动重连后继续录，
                    # 这样"关机再开机"验证掉电记忆、复位、烧录都不会再中断抓取。
                    print("读口中断：%s | 1 秒后自动重连..." % e)
                    try:
                        s.close()
                    except Exception:
                        pass
                    time.sleep(1.0)
                    if end is not None and time.time() >= end:
                        break
                    for _ in range(15):
                        try:
                            s = serial.Serial(args.port, args.baud, timeout=0.2)
                            print("已重连 %s，继续录制" % args.port)
                            break
                        except Exception as e2:
                            time.sleep(1.0)
                    else:
                        print("重连失败，停止录制")
                        break
                    continue
                if not d:
                    continue
                f.write(d)
                f.flush()
                pending += d
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
                    print("[%s] %s" % (ts, line.decode("utf-8", "replace")))
        except KeyboardInterrupt:
            pass
        finally:
            try:
                s.close()
            except Exception:
                pass

    size = os.path.getsize(path)
    print("已保存 %s（%d 字节）" % (path, size))


if __name__ == "__main__":
    main()
