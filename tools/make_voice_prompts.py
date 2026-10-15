#!/usr/bin/env python3
"""
八音琴语音提示素材生成器（甲方案：静态 14 句清单）。

流程：
  1. 读取 tools/voice_prompts/_raw/ 下由即梦 TTS 生成的分组 mp3（每组含多句，句间有停顿）；
  2. 重采样到 **16kHz 单声道**（比原来的 32kHz 减半）；切句、淡入淡出、峰值归一化；
  3. 从切出的句子（原 17 句 + 重录的"已保存"1 句）里挑出定稿的 **14 句静态清单**
     （放弃动态报数"共N首/第N首"）；
  4. 打包成**带索引表**的单一资源文件 voice_prompts.vpmt：
       头 16B : magic 'VPMT' + version(u16) + count(u16) + 保留(8)
       索引 ×N: slot(u16) + offset(u32) + length(u32) + 保留(u16)
       数据   : 每句 PCM（16kHz int16 小端），offset 相对资源区起始
     资源写入 rec 分区 slot5 起（slot0~4 是曲库），单句可跨槽故索引记 slot+offset。

产物：
  tools/voice_prompts/voice_prompts.vpmt     —— 固件读取的资源（写进 rec 分区 slot5+）
  tools/voice_prompts/16k/<name>.pcm / .wav  —— 每句 16k 素材（便于试听/核对）

用法：
  python make_voice_prompts.py            # 转换并打包
  python make_voice_prompts.py --analyze  # 只打印切分情况，不写文件

依赖：numpy、soundfile（已在 tools/.venv 中安装）
"""
import argparse
import os
import struct

import numpy as np
import soundfile as sf

SAMPLE_RATE = 16000          # 16kHz 单声道（播放时固件做 2× 上采样到 32kHz）
PEAK = 26000                 # 峰值归一化目标
HERE = os.path.dirname(os.path.abspath(__file__))
RAW_DIR = os.path.join(HERE, "voice_prompts", "_raw")   # 即梦原始 mp3
OUT_DIR = os.path.join(HERE, "voice_prompts")           # 交付目录
CLIP_DIR = os.path.join(OUT_DIR, "16k")                 # 每句 16k 素材

# 固件侧槽位分配（必须与 firmware/main/rec_layout.h 一致）
REC_SLOT_SIZE = 32768
REC_SLOT_PROMPT_BASE = 5

# 即梦原始 mp3 → 按顺序切成若干句（与固件预期一致，见 docs/八音琴-需求与决策记录.md）
GROUPS = [
    {
        "tag": "A",
        "src": "dreamina-eb2ecfc0-5787-4158-a230-0ce64de8d862.mp3",
        "names": [
            ("hint_select_mode", "请选择模式"),
            ("mode_trial", "试听模式"),
            ("mode_self", "自弹模式"),
            ("mode_game", "游戏模式"),
            ("mode_playback", "回放模式"),
        ],
    },
    {
        "tag": "B",
        "src": "dreamina-a4ad5fb9-4022-488f-af73-5d54340bbbe3.mp3",
        "names": [
            ("hint_trial_count", "试听模式，共五首"),
            ("song_twinkle", "小星星"),
            ("song_two_tigers", "两只老虎"),
            ("song_ode", "欢乐颂"),
            ("hint_play_end", "播放结束"),
            ("hint_pause", "暂停"),
            ("hint_resume", "继续"),
        ],
    },
    {
        "tag": "C",
        "src": "dreamina-9de47d2c-40d9-4304-a047-764b982133f7.mp3",
        "names": [
            ("hint_start_record", "开始记谱"),
            ("hint_saved_count", "已保存第五首"),
            ("hint_no_record", "暂无记录"),
            ("hint_no_function", "本键无功能"),
            ("hint_back_home", "回到主目录"),
        ],
    },
    # 2026-10-15 重录：id9"已保存"单独再生成一次，去掉会误导用户的动态数字"第五首"。
    # 其余 13 句沿用原有素材，不重生成（避免音色/时长漂移）。
    {
        "tag": "D",
        "src": "dreamina-ff2b73e9-c65b-4aa6-9074-1cc42707efc1.mp3",
        "names": [
            ("hint_saved", "已保存"),
        ],
    },
]

# 定稿 14 句静态清单：id 顺序即固件 prompt_id_t 顺序（prompt_format.h）。
# 每项 = (资源名, 文案, 来源组 tag)。未列入的 3 句（试听共五首/暂停/继续）丢弃。
FINAL = [
    ("hint_select_mode", "请选择模式", "A"),   # id0
    ("mode_trial", "试听模式", "A"),           # id1
    ("mode_self", "自弹模式", "A"),            # id2
    ("mode_game", "游戏模式", "A"),            # id3
    ("mode_playback", "回放模式", "A"),        # id4
    ("song_twinkle", "小星星", "B"),           # id5
    ("song_two_tigers", "两只老虎", "B"),      # id6
    ("song_ode", "欢乐颂", "B"),               # id7
    ("hint_start_record", "开始记谱", "C"),    # id8
    ("hint_saved", "已保存", "D"),             # id9（2026-10-15 重录；旧素材原话"已保存第五首"带错误数字）
    ("hint_play_end", "播放结束", "B"),        # id10
    ("hint_no_record", "暂无记录", "C"),       # id11
    ("hint_no_function", "本键无功能", "C"),   # id12
    ("hint_back_home", "回到主目录", "C"),     # id13
]


def resample_linear(x, sr_in, sr_out):
    """线性插值重采样；降采样时先做一次简单抗混叠平滑。"""
    if sr_in == sr_out:
        return x
    if sr_out < sr_in:
        k = max(1, int(round(sr_in / sr_out)))
        if k > 1:
            kernel = np.ones(k) / k
            x = np.convolve(x, kernel, mode="same")
    n_out = int(round(len(x) * sr_out / sr_in))
    idx = np.linspace(0, len(x) - 1, n_out)
    return np.interp(idx, np.arange(len(x)), x).astype(np.float32)


def voiced_runs(x, win_ms=15.0, hop_ms=5.0, thr_rel=0.05):
    """返回静音判定下的连续有声区间 [(start,end) 采样点]。"""
    win, hop = int(SAMPLE_RATE * win_ms / 1000) or 1, int(SAMPLE_RATE * hop_ms / 1000) or 1
    n = 1 + max(0, (len(x) - win) // hop)
    env = np.array([np.sqrt(np.mean(x[i * hop:i * hop + win] ** 2) + 1e-12) for i in range(n)])
    voiced = env > max(env.max() * thr_rel, 1e-4)
    runs, i = [], 0
    while i < n:
        if voiced[i]:
            j = i
            while j < n and voiced[j]:
                j += 1
            runs.append((i * hop, min(len(x), j * hop + win)))
            i = j
        else:
            i += 1
    return runs


def split_runs(runs, expected):
    """按最大的 expected-1 个间隙切分，得到恰好 expected 段（合并小间隙/逗号停顿）。"""
    if len(runs) < expected:
        raise RuntimeError(f"只检测到 {len(runs)} 段有声区间，少于预期 {expected} 句")
    gaps = [(runs[i + 1][0] - runs[i][1], i) for i in range(len(runs) - 1)]
    boundaries = sorted(i for _, i in sorted(gaps, reverse=True)[:expected - 1])
    segments, start_run = [], 0
    for b in boundaries:
        segments.append((runs[start_run][0], runs[b][1]))
        start_run = b + 1
    segments.append((runs[start_run][0], runs[-1][1]))
    return segments


def polish(seg):
    """淡入淡出 + 峰值归一化到 int16。"""
    fade = int(SAMPLE_RATE * 0.005)  # 5ms
    seg = seg.astype(np.float32).copy()
    if len(seg) > 2 * fade:
        seg[:fade] *= np.linspace(0.0, 1.0, fade)
        seg[-fade:] *= np.linspace(1.0, 0.0, fade)
    peak = np.max(np.abs(seg)) or 1.0
    seg = seg / peak * (PEAK / 32767.0)
    return np.clip(seg * 32767.0, -32768, 32767).astype(np.int16)


def write_wav_pcm(path_base, samples):
    sf.write(path_base + ".wav", samples, SAMPLE_RATE, subtype="PCM_16")
    with open(path_base + ".pcm", "wb") as f:
        f.write(b"".join(struct.pack("<h", int(s)) for s in samples))


def build_clips(analyze):
    """切出各组每句 int16 数组，返回 {name: np.ndarray}。"""
    clips = {}
    for g in GROUPS:
        path = os.path.join(RAW_DIR, g["src"])
        x, sr = sf.read(path, dtype="float32", always_2d=False)
        if x.ndim > 1:
            x = x.mean(axis=1)
        x = resample_linear(x, sr, SAMPLE_RATE)
        runs = voiced_runs(x)
        segs = split_runs(runs, len(g["names"]))
        print(f"[{g['tag']}] {g['src']}  sr={sr}->{SAMPLE_RATE}  runs={len(runs)} -> {len(segs)} 句")
        for (name, text), (a, b) in zip(g["names"], segs):
            seg = polish(x[a:b])
            clips[name] = seg
            ms = len(seg) / SAMPLE_RATE * 1000
            if analyze:
                print(f"    {name:22s} {text:12s} {ms:6.0f} ms  {len(seg)*2:6d} B")
            else:
                write_wav_pcm(os.path.join(CLIP_DIR, name), seg)
    return clips


def pack(clips):
    """打包 VPMT 资源（头 + 索引 + 数据），返回 bytes 与统计。"""
    count = len(FINAL)
    hdr = struct.pack("<4sHH", b"VPMT", 1, count) + b"\x00" * 8  # 16B
    idx_start = len(hdr)
    data_start = idx_start + count * 12

    entries = []
    data = bytearray()
    for name, _text, _tag in FINAL:
        s = clips[name]
        off = data_start + len(data)
        raw = s.tobytes()  # int16 小端
        entries.append((REC_SLOT_PROMPT_BASE + off // REC_SLOT_SIZE, off, len(raw)))
        data += raw
        if len(data) % 2:  # 保持偶数字节（理论上不会发生）
            data += b"\x00"

    idx = bytearray()
    for slot, off, ln in entries:
        idx += struct.pack("<HIIH", slot, off, ln, 0)

    blob = bytearray(hdr) + idx + data
    used_slots = 0
    if blob:
        used_slots = (len(blob) + REC_SLOT_SIZE - 1) // REC_SLOT_SIZE
    return bytes(blob), entries, used_slots


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--analyze", action="store_true", help="只打印切分情况，不写文件")
    args = ap.parse_args()

    if not args.analyze:
        os.makedirs(OUT_DIR, exist_ok=True)
        os.makedirs(CLIP_DIR, exist_ok=True)

    clips = build_clips(args.analyze)

    # 校验清单里的每句都切出来了
    missing = [n for n, _t, _g in FINAL if n not in clips]
    if missing:
        raise RuntimeError(f"清单缺少素材：{missing}")

    blob, entries, used_slots = pack(clips)
    print(f"清单 {len(FINAL)} 句；未使用 3 句：hint_trial_count / hint_pause / hint_resume")
    print(f"资源总字节 {len(blob)} ({len(blob)/1024:.1f} KB)，占用 rec slot "
          f"{REC_SLOT_PROMPT_BASE}~{REC_SLOT_PROMPT_BASE + used_slots - 1}（共 {used_slots} 槽）")
    for i, (name, text, _tag) in enumerate(FINAL):
        slot, off, ln = entries[i]
        print(f"  id{i:2d} {text:6s} slot{slot:2d} off={off:6d} len={ln:6d} ({ln/2:6.0f} 帧)")

    if not args.analyze:
        vpmt = os.path.join(OUT_DIR, "voice_prompts.vpmt")
        with open(vpmt, "wb") as f:
            f.write(blob)
        print(f"已写出：{vpmt}")

        gap = np.zeros(int(SAMPLE_RATE * 0.25), dtype=np.int16)  # 试听版：句间 250ms
        cat = []
        for i, (name, _t, _g) in enumerate(FINAL):
            cat.append(clips[name])
            if i != len(FINAL) - 1:
                cat.append(gap)
        sf.write(os.path.join(OUT_DIR, "_all_prompts_preview.wav"),
                 np.concatenate(cat), SAMPLE_RATE, subtype="PCM_16")
    print(f"合计 {len(FINAL)} 句，{sum(len(clips[n]) for n, _t, _g in FINAL) * 2} B @16kHz")


if __name__ == "__main__":
    main()
