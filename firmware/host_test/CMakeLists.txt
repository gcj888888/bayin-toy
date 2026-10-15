cmake_minimum_required(VERSION 3.16)
project(beatbox_host_tests C)

enable_testing()

# ---- 已有的纯逻辑测试 ----
add_executable(clock_tests clock_tests.c)
target_include_directories(clock_tests PRIVATE ../main/seq)
add_test(NAME clock_tests COMMAND clock_tests)

# 电子琴：七个音的频率与颜色（纯逻辑，可直接在电脑上验证，无需烧录）
add_executable(music_notes_tests music_notes_tests.c ../main/music_notes.c)
target_include_directories(music_notes_tests PRIVATE ../main)
add_test(NAME music_notes_tests COMMAND music_notes_tests)

# 电子琴：旋钮短按/长按手势状态机（纯逻辑，可电脑验证）
add_executable(gesture_tests gesture_tests.c ../main/gesture.c)
target_include_directories(gesture_tests PRIVATE ../main)
add_test(NAME gesture_tests COMMAND gesture_tests)

# ---- 八音琴：五态状态机（主目录/试听/自弹/游戏/回放）纯逻辑 ----
add_executable(piano_fsm_tests
    piano_fsm_tests.c
    ../main/piano_fsm.c)
target_include_directories(piano_fsm_tests PRIVATE ../main)
add_test(NAME piano_fsm_tests COMMAND piano_fsm_tests)

# ---- 八音琴：按键场景级测试（合成按键序列驱动 piano_fsm + 假 audio 计数）----
add_executable(piano_scene_tests
    piano_scene_tests.c
    ../main/piano_fsm.c)
target_include_directories(piano_scene_tests PRIVATE ../main)
add_test(NAME piano_scene_tests COMMAND piano_scene_tests)

# ---- 八音琴：旋钮语义分流 + 亮度钳位 + NVS 脏标记落盘时机（纯逻辑） ----
add_executable(enc_dial_tests
    enc_dial_tests.c
    ../main/enc_dial.c)
target_include_directories(enc_dial_tests PRIVATE ../main)
add_test(NAME enc_dial_tests COMMAND enc_dial_tests)

# ---- 八音琴：混音声道"源选择"不变量（守护"语音反复播报"根因）----
add_executable(voice_src_tests voice_src_tests.c)
target_include_directories(voice_src_tests PRIVATE ../main/audio)
add_test(NAME voice_src_tests COMMAND voice_src_tests)

# ---- 八音琴：音量增益单极点平滑（消除 zipper 阶跃杂音）----
add_executable(audio_gain_tests audio_gain_tests.c)
target_include_directories(audio_gain_tests PRIVATE ../main/audio)
add_test(NAME audio_gain_tests COMMAND audio_gain_tests)

# ---- 八音琴：5 槽曲库 / 回放池（song_add/del/list/push FIFO + flash 持久化） ----
# song_pool.c 的 flash 持久化复用 recorder 的 "rec" 分区，经 voice_store_stubs 的内存
# 分区替身 + stubs/include 的 esp_partition/esp_log/esp_err 替身，可在 PC 上覆盖。
add_executable(song_pool_tests
    song_pool_tests.c
    ../main/song_pool.c
    voice_store_stubs.c)
target_include_directories(song_pool_tests PRIVATE ../main ../main/audio stubs/include)
add_test(NAME song_pool_tests COMMAND song_pool_tests)

# ---- 八音琴：【试听池(flash曲库) vs 回放池(RAM FIFO)】双池划分场景测试 ----
# 录音→保存→回放可见；内置 3 首不出现在回放；试听不受影响；游戏记谱进回放池。
add_executable(piano_pool_tests
    piano_pool_tests.c
    ../main/song_pool.c
    ../main/play_pool.c
    voice_store_stubs.c)
target_include_directories(piano_pool_tests PRIVATE ../main ../main/audio stubs/include)
add_test(NAME piano_pool_tests COMMAND piano_pool_tests)

# ---- 八音琴：语音提示清单 / prompt id 映射 / 资源索引解析 / rec 槽位布局（纯逻辑） ----
add_executable(prompt_tests
    prompt_tests.c
    ../main/prompt_format.c)
target_include_directories(prompt_tests PRIVATE ../main)
add_test(NAME prompt_tests COMMAND prompt_tests)

# ---- 阶段③：开机语音存储（base64 重组/落分区/has_import/clear/截断/非法参数） ----
# voice_store.c 依赖 ESP-IDF 的 esp_partition/heap_caps/esp_log（均为 stubs 替身）
# 与 audio_click（voice_store_stubs 替身）。
add_executable(voice_store_tests
    voice_store_tests.c
    ../main/voice_store.c
    voice_store_stubs.c)
target_include_directories(voice_store_tests PRIVATE ../main ../main/audio stubs/include)
add_test(NAME voice_store_tests COMMAND voice_store_tests)

# ---- 阶段③：host_link 命令解析+分发（song/demo/voice_*/旧命令回归） ----
# host_link.c 依赖 json_field.c（解析）与 pattern（pattern_stub 替身）、
# USB-Serial-JTAG 驱动（usb_serial_jtag_stub 替身，仅链接用，测试不调用 init）。
add_executable(host_link_tests
    host_link_tests.c
    ../main/host/host_link.c
    ../main/host/json_field.c
    pattern_stub.c
    usb_serial_jtag_stub.c)
target_include_directories(host_link_tests PRIVATE ../main/host ../main/seq ../main stubs/include)
# MinGW 默认不暴露 fcntl/F_GETFL/O_NONBLOCK（host_link_init 用，但测试不调用），
# 强制包含 posix_compat.h 垫片让 host_link.c 能编译链接。
target_compile_options(host_link_tests PRIVATE "-include" "posix_compat.h")
add_test(NAME host_link_tests COMMAND host_link_tests)

# ---- 阶段②：录音 / 回放 + flash 乐谱存储（录→存→读往返/槽位独立/回放/发电脑） ----
# recorder.c 依赖 esp_partition（voice_store_stubs 替身）、host_link_send_line（host_link_stub 替身）。
add_executable(recorder_tests
    recorder_tests.c
    ../main/recorder.c
    host_link_stub.c
    voice_store_stubs.c)
target_include_directories(recorder_tests PRIVATE ../main ../main/host ../main/audio ../main/ui stubs/include)
add_test(NAME recorder_tests COMMAND recorder_tests)
