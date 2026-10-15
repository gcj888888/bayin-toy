#!/usr/bin/env python3
"""
电子琴音色采样生成器（离线合成，纯 Python，无需 numpy）。

思路：向 beatbox 的鼓声学习——把"现场实时合成"改成"离线算好完整采样"，
按键时像鼓声一样直接播放。这样做有三个好处：

1. 音频在结尾处已完整衰减到 0，播放完自然停止，不存在"掐断"→ 彻底消除爆音。
2. 离线计算不受芯片实时性能限制，可以用更精细的加法合成（6 个泛音）。
3. 每个泛音有独立衰减率（高次泛音衰减更快），音色更接近真实乐器。

输出：int16 小端 32kHz 单声道 .raw（与 beatbox 鼓声采样格式一致）。
"""

import math
import os
import struct

SR = 32000          # 采样率，与固件 I2S 一致
DURATION = 0.85     # 音长（秒）
N = int(SR * DURATION)

# C 大调七个音
FREQS = [261.63, 293.66, 329.63, 349.23, 392.00, 440.00, 493.88]
NAMES = ["do", "re", "mi", "fa", "sol", "la", "si"]

# 泛音结构：(倍数, 幅度, 衰减率倍数)
# 八音盒"音簧"音色（alt_musicbox）：基础音干净，重点加强 2/4/6/8 次泛音，
# 且高次泛音衰减更快（衰减率倍数逐次增大），做出"清脆敲击 → 余音纯净"的盒子感；
# 3 次泛音压低以减少"木琴/方波"味。
HARMONICS = [
    (1.0, 1.00, 1.0),
    (2.0, 0.45, 1.6),
    (3.0, 0.08, 2.4),
    (4.0, 0.32, 2.6),
    (6.0, 0.14, 3.2),
    (8.0, 0.06, 3.8),
]

ATTACK_S = 0.005   # 起音 5 毫秒（余弦窗，保持"脆"且无 click）
RELEASE_S = 0.12   # 释放 120 毫秒（余弦窗，两端导数为 0，平滑收尾）


def synth(freq):
    samples = [0.0] * N
    # 基础衰减率：alt_musicbox 余音较慢（3.2），配合 0.85s 采样长度与 120ms 释放窗
    # 在"字节预算"内截短，既保留金属音簧余音又不超固件采样数组预算。
    base_decay = 3.2
    attack_n = max(1, int(ATTACK_S * SR))
    release_n = max(1, int(RELEASE_S * SR))
    release_start = N - release_n

    for i in range(N):
        t = i / SR

        # 起音：余弦窗（两端导数为 0，不产生 click）
        attack = 0.5 * (1.0 - math.cos(math.pi * i / attack_n)) if i < attack_n else 1.0

        # 释放：余弦窗，从 1 平滑降到 0
        if i >= release_start:
            x = (N - i) / release_n
            release = 0.5 * (1.0 - math.cos(math.pi * x))
        else:
            release = 1.0

        # 加法合成：多个泛音叠加，各自独立衰减
        v = 0.0
        for mult, amp, decay_mult in HARMONICS:
            f = freq * mult
            if f >= SR / 2:  # 超过奈奎斯特频率则跳过，避免混叠
                continue
            v += amp * math.sin(2.0 * math.pi * f * t) * math.exp(-t * base_decay * decay_mult)

        samples[i] = v * attack * release

    # 归一化到 85% 满幅：七个音音量一致，且给多音叠加留余量
    peak = max(abs(s) for s in samples) or 1.0
    scale = 0.85 * 32767.0 / peak

    out = bytearray()
    for s in samples:
        val = int(max(-32768, min(32767, s * scale)))
        out += struct.pack("<h", val)
    return bytes(out)


def main():
    outdir = r"E:\easyIntput\easyinput-piano\firmware\main\audio\samples"
    os.makedirs(outdir, exist_ok=True)
    total = 0
    for name, freq in zip(NAMES, FREQS):
        data = synth(freq)
        path = os.path.join(outdir, "note_%s.raw" % name)
        with open(path, "wb") as f:
            f.write(data)
        total += len(data)
        print("  %-4s %6.2f Hz -> %s (%d 字节)" % (name, freq, os.path.basename(path), len(data)))
    print("合计 %d 字节 (%.1f KB)" % (total, total / 1024.0))


if __name__ == "__main__":
    main()
