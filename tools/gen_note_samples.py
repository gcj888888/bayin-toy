#!/usr/bin/env python3
"""
电子琴音色采样生成器（离线合成，纯 Python，无需 numpy）。

思路：向 beatbox 的鼓声学习——把"现场实时合成"改成"离线算好完整采样"，
按键时像鼓声一样直接播放。这样做有三个好处：

1. 音频在结尾处已完整衰减到 0，播放完自然停止，不存在"掐断"→ 彻底消除爆音。
2. 离线计算不受芯片实时性能限制，可以用更精细的加法合成。
3. 每个泛音有独立衰减率（高次泛音衰减更快），音色更接近真实乐器。

本次调音色（更像钢琴）：把原来的"八音盒音簧"（2/4/6/8 次泛音突出的金属齿感）
改成"钢琴钢弦"——
  · 谐波堆栈：幅度按约 1/n^1.3 递减，且含偶次谐波（不是纯三角波的奇次 1/n²）；
  · 轻微失谐（inharmonicity）：上分音频率随阶数轻微拉伸（模拟钢琴弦刚度）；
  · 短促明亮起音瞬态：10~16 次高次泛音约 10ms 快速衰减（模拟槌击瞬间的"脆亮"）；
  · 双段衰减：起音后先快衰减、再进入慢尾（不是单一指数）。

守住原有防爆音设计：起音仍是 5ms 余弦窗（两端导数为 0）、释放仍是 120ms 余弦窗，
波形结尾完整衰减到 0。

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

# ---- 钢琴化音色参数 ----
# 泛音堆栈：(谐波次数, 相对幅度)。幅度大致按 1/n^1.3 递减 —— 真实钢琴"锤击钢弦"
# 的谐波堆栈特征；含 2/4/6/8 次偶次谐波（纯三角波只有奇次，这里是关键区别）。
PIANO_PARTIALS = [
    (1, 1.000),
    (2, 0.410),
    (3, 0.240),
    (4, 0.160),
    (5, 0.122),
    (6, 0.096),
    (7, 0.078),
    (8, 0.065),
]
# 起音瞬态：10~16 次高次泛音，约 10ms 快速衰减 —— 模拟槌击瞬间的"脆亮"。
TRANSIENT_PARTIALS = [
    (10, 0.050),
    (12, 0.040),
    (14, 0.032),
    (16, 0.026),
]

INHARMONICITY_B = 0.0005    # 上分音"拉伸"系数（钢琴弦刚度）：f_n = f0·n·sqrt(1+B·n²)
BASE_DECAY = 2.6            # 慢尾衰减率（余音）
PARTIAL_DECAY_SLOPE = 0.22  # 每高一阶泛音，衰减率额外增加比例（高次衰减更快）
FAST_DECAY_EXTRA = 7.0      # 双段衰减中"快段"的额外衰减率
FAST_WEIGHT = 0.45          # 双段衰减中"快段"占比（其余为慢尾）
TRANSIENT_TAU = 0.010       # 起音瞬态时间常数 ~10ms（约 10~20ms 内衰减完）

ATTACK_S = 0.005   # 起音 5 毫秒（余弦窗，保持"脆"且无 click）
RELEASE_S = 0.12   # 释放 120 毫秒（余弦窗，两端导数为 0，平滑收尾）


def partial_freq(f0, n):
    """第 n 次泛音的（轻微拉伸的）频率，模拟钢琴弦的 inharmonicity。"""
    return f0 * n * math.sqrt(1.0 + INHARMONICITY_B * n * n)


def two_stage(t, decay):
    """双段衰减：先快（起音后快速回落）、后慢（余音长尾）。t=0 时为 1。"""
    fast = math.exp(-t * (decay + FAST_DECAY_EXTRA))
    slow = math.exp(-t * decay)
    return FAST_WEIGHT * fast + (1.0 - FAST_WEIGHT) * slow


def synth(freq):
    samples = [0.0] * N
    attack_n = max(1, int(ATTACK_S * SR))
    release_n = max(1, int(RELEASE_S * SR))
    release_start = N - release_n

    # 预计算各泛音的频率与衰减率（避免逐样本重复 sqrt）
    body = [(partial_freq(freq, n), amp, BASE_DECAY * (1.0 + (n - 1) * PARTIAL_DECAY_SLOPE))
            for n, amp in PIANO_PARTIALS]
    transient = [(partial_freq(freq, n), amp) for n, amp in TRANSIENT_PARTIALS]

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

        # 谐波堆栈：多个泛音叠加、各自独立（双段）衰减
        v = 0.0
        for f, amp, decay in body:
            if f >= SR / 2:  # 超过奈奎斯特频率则跳过，避免混叠
                continue
            v += amp * math.sin(2.0 * math.pi * f * t) * two_stage(t, decay)

        # 起音瞬态：高次泛音，约 10ms 快速衰减
        trans_env = math.exp(-t / TRANSIENT_TAU)
        for f, amp in transient:
            if f >= SR / 2:
                continue
            v += amp * math.sin(2.0 * math.pi * f * t) * trans_env

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
