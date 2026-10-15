#!/usr/bin/env python3
"""
生成电子琴开机默认语音：人声“欢迎来到AI音乐世界”。
输出：audio/samples/voice_default.raw（32kHz / 16bit / 单声道 PCM）。

流程：
  1. 优先用 edge-tts（微软云端中文神经语音，免费）合成 mp3；
  2. 用 miniaudio 解码并重采样到 32kHz 单声道 16bit；
  3. 任意一步失败（无网络/缺库），自动降级为一段柔和的 do-mi-sol-do 琶音，
     保证固件一定能编译、开机一定有声音。

用法：python gen_voice_default.py
"""
import math
import os
import struct
import subprocess
import sys

SAMPLE_RATE = 32000
# 输出到固件源码的采样目录（与 note_*.raw 同目录，CMakeLists 在那里 EMBED_FILES）
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_PATH = os.path.join(REPO_ROOT, "firmware", "main", "audio", "samples", "voice_default.raw")
TEXT = "欢迎来到AI音乐世界"


def write_pcm(path, samples):
    """samples: list[int16]"""
    with open(path, "wb") as f:
        f.write(b"".join(struct.pack("<h", s) for s in samples))
    print(f"[gen] wrote {len(samples)} frames ({len(samples)*2} bytes) -> {path}")


def fallback_arpeggio():
    """降级：do-mi-sol-do 柔和琶音（C大调），约 1.6 秒。"""
    print("[gen] TTS unavailable, falling back to soft arpeggio")
    freqs = [261.6, 329.6, 392.0, 523.3]  # do mi sol do(+8)
    dur = 0.4
    out = []
    for f in freqs:
        n = int(SAMPLE_RATE * dur)
        for i in range(n):
            t = i / SAMPLE_RATE
            # 三角波+少量泛音，余弦起音/收尾，柔和不刺耳
            phase = (f * t) % 1.0
            tri = 4 * phase - 1 if phase < 0.5 else 3 - 4 * phase
            env = 0.5 * (1 - math.cos(math.pi * min(t, 0.02) / 0.02))
            tail = 0.5 * (1 - math.cos(math.pi * max(0, (dur - t)) / 0.02)) if t > dur - 0.02 else 1.0
            val = tri * 0.6 * env * min(1.0, tail)
            out.append(int(max(-1, min(1, val)) * 24000))
    write_pcm(OUT_PATH, out)


def main():
    try:
        import edge_tts
        import miniaudio
    except ImportError:
        print("[gen] edge_tts/miniaudio not installed; using fallback")
        fallback_arpeggio()
        return

    mp3 = OUT_PATH + ".mp3"
    try:
        print(f"[gen] synthesizing via edge-tts: {TEXT!r}")
        import asyncio

        async def synth():
            voice = "zh-CN-XiaoxiaoNeural"  # 中文女声神经语音
            comm = edge_tts.Communicate(TEXT, voice)
            await comm.save(mp3)

        asyncio.run(synth())
        with open(mp3, "rb") as f:
            data = f.read()
        dec = miniaudio.decode(
            data=data,
            output_format=miniaudio.SampleFormat.SIGNED16,
            sample_rate=SAMPLE_RATE,
            nchannels=1,
            dither=miniaudio.DitherMode.NONE,
        )
        # miniaudio 对 16bit 既可能返回 bytes 也可能返回样本序列，统一成 int 列表
        raw = dec.samples
        if isinstance(raw, (bytes, bytearray)):
            samples = list(struct.unpack("<%dh" % (len(raw) // 2), raw))
        else:
            samples = [int(x) for x in raw]
        # 归一化到合理音量（避免云端音量过大）
        peak = max((abs(s) for s in samples), default=0)
        if peak > 26000:
            scale = 26000 / peak
            samples = [int(s * scale) for s in samples]
        write_pcm(OUT_PATH, samples)
        try:
            os.remove(mp3)
        except OSError:
            pass
        return
    except Exception as e:  # noqa: BLE001
        print(f"[gen] TTS/decode failed ({e!r}); using fallback")
        fallback_arpeggio()
        return


if __name__ == "__main__":
    main()
