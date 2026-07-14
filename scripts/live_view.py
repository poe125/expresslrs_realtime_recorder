#!/usr/bin/env python3
"""ExpressLRS Realtime Recorder - リアルタイム値ビューア (方式C)

Pico がデバッグUART(GP4/5 @921600)に出力する連続ストリーム
("D," プレフィックスのCSV) を解析し、各CRSFチャンネルをバーゲージで
ライブ表示する。スナップショット等の非ストリーム行はログ領域に表示。

使い方:
    python3 scripts/live_view.py [--port /dev/tty.usbmodemXXXX] [--baud 921600]

キー操作:
    s : ストリーム ON/OFF（Pico側へ 's' 送信）
    d : スナップショット要求（Pico側へ 'd' 送信、ログ領域に全段ダンプ表示）
    q : 終了

依存: pyserial  (pip install pyserial)
"""
import argparse
import curses
import glob
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial が必要です:  pip install pyserial")

CRSF_MIN, CRSF_MID, CRSF_MAX = 172, 992, 1811

# main.c の map_gamepad_to_channels と対応
CH_LABELS = [
    "CH1  Roll", "CH2  Pitch", "CH3  Thr", "CH4  Yaw",
    "CH5  L2", "CH6  R2", "CH7  A/Arm", "CH8  B",
    "CH9  X", "CH10 Y", "CH11 LB", "CH12 RB",
    "CH13 -", "CH14 -", "CH15 -", "CH16 -",
]
AXIS_LABELS = ["LX", "LY", "RX", "RY", "L2", "R2"]


def find_port():
    # PCへの出力は GP4/5 → USB-シリアル変換経由。USB-UART変換の一般的な名前を優先。
    for pat in ("/dev/cu.usbserial-*", "/dev/cu.SLAB_USBtoUART*", "/dev/cu.wchusbserial*",
                "/dev/cu.usbmodem*", "/dev/ttyUSB*", "/dev/ttyACM*"):
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    return None


def bar(value, width=30):
    frac = (value - CRSF_MIN) / (CRSF_MAX - CRSF_MIN)
    frac = max(0.0, min(1.0, frac))
    fill = int(round(frac * width))
    mid = int(round((CRSF_MID - CRSF_MIN) / (CRSF_MAX - CRSF_MIN) * width))
    cells = []
    for i in range(width):
        if i == mid:
            cells.append("|" if i >= fill else "#")
        else:
            cells.append("#" if i < fill else "-")
    return "".join(cells)


def draw(stdscr, state, log, port, stream_on, rate_hz):
    stdscr.erase()
    h, w = stdscr.getmaxyx()
    conn = "CONNECTED" if state.get("conn") else "----"
    title = f" ExpressLRS Live View  [{port}]  {conn}  stream:{'ON' if stream_on else 'OFF'}  {rate_hz:5.1f}Hz "
    stdscr.addnstr(0, 0, title.ljust(w - 1), w - 1, curses.A_REVERSE)

    row = 2
    chans = state.get("ch", [CRSF_MID] * 16)
    for i in range(16):
        if row >= h - 1:
            break
        line = f"{CH_LABELS[i]:<11} {chans[i]:4d} [{bar(chans[i])}]"
        stdscr.addnstr(row, 1, line, w - 2)
        row += 1

    row += 1
    if row < h - 1:
        axes = state.get("axes", [0] * 6)
        axline = "AXES  " + "  ".join(f"{AXIS_LABELS[i]}={axes[i]:6d}" for i in range(6))
        stdscr.addnstr(row, 1, axline, w - 2)
        row += 1
    if row < h - 1:
        stdscr.addnstr(row, 1, f"BTN   0x{state.get('btn', 0):04X}", w - 2)
        row += 2

    # ログ領域（スナップショット等）
    if row < h - 1:
        stdscr.addnstr(row, 0, " LOG ".center(w - 1, "-"), w - 1, curses.A_DIM)
        row += 1
        for ln in log[-(h - row - 1):]:
            if row >= h - 1:
                break
            stdscr.addnstr(row, 1, ln, w - 2)
            row += 1

    stdscr.addnstr(h - 1, 0, " s:stream  d:snapshot  q:quit ".ljust(w - 1), w - 1, curses.A_REVERSE)
    stdscr.refresh()


def run(stdscr, ser, port):
    curses.curs_set(0)
    stdscr.nodelay(True)
    state = {"conn": 0, "axes": [0] * 6, "btn": 0, "ch": [CRSF_MID] * 16}
    log = []
    buf = b""
    stream_on = False
    frames, last_calc, rate_hz = 0, time.time(), 0.0

    # 起動時にストリームを有効化
    ser.write(b"s")
    stream_on = True

    while True:
        # シリアル受信（非ブロッキング）
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("ascii", "replace").strip()
                if not line:
                    continue
                if line.startswith("D,"):
                    parts = line.split(",")
                    # D,conn,a0..a5,btn,ch0..ch15  => 1+1+6+1+16 = 25
                    if len(parts) >= 25:
                        try:
                            vals = [int(x) for x in parts[1:25]]
                            state["conn"] = vals[0]
                            state["axes"] = vals[1:7]
                            state["btn"] = vals[7]
                            state["ch"] = vals[8:24]
                            frames += 1
                        except ValueError:
                            pass
                else:
                    log.append(line)
                    log[:] = log[-200:]
                    if line.startswith("# stream"):
                        stream_on = "ON" in line

        # レート計算
        now = time.time()
        if now - last_calc >= 0.5:
            rate_hz = frames / (now - last_calc)
            frames, last_calc = 0, now

        # キー入力
        try:
            c = stdscr.getch()
        except curses.error:
            c = -1
        if c != -1:
            ch = chr(c) if 0 <= c < 256 else ""
            if ch == "q":
                break
            elif ch == "s":
                ser.write(b"s")
            elif ch == "d":
                ser.write(b"d")

        draw(stdscr, state, log, port, stream_on, rate_hz)
        time.sleep(0.02)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--baud", type=int, default=921600)
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        sys.exit("シリアルポートが見つかりません。--port で指定してください。")

    ser = serial.Serial(port, args.baud, timeout=0)
    try:
        curses.wrapper(run, ser, port)
    finally:
        ser.write(b"s")  # 終了時にストリームを止める（OFFトグル）
        ser.close()


if __name__ == "__main__":
    main()
