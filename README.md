# 口袋里的玩具 —— 八音琴（EasyInput V2.0 / ESP32-S3）

一台能塞进口袋的小琴：**可以玩、可以学、可以创作**。

- 课程：第七期 AI 硬件基础训练营 · 第8课毕业大作业
- 方向：**方向二 · 从零开始实现自己想做的项目**（板子是课程提供的载体，项目是自己构思的）
- 作业文档（含演示视频）：https://ucn299x5d3ob.feishu.cn/wiki/WMOAwUg5Uil7RRkwQ5RcRkOQnVf

## 它长什么样

一块 EasyInput V2.0（ESP32-S3）开发板 —— 8 个按键（8 个音）、1 个 EC11 旋钮、5 颗 WS2812 灯、1 个 I2S 喇叭 —— 长成一个「文件夹式」的顶层结构：

```
主目录（开机默认；语音「请选择模式」＋ D1~D4 亮）
 ├─ S1 试听 ── 内置曲库，最多 5 首
 ├─ S2 自弹 ── 弹奏 ＋ 记谱（记「按了哪个键 ＋ 隔了多久」，即简谱）
 ├─ S3 游戏 ── 网页跟弹闯关
 └─ S4 回放 ── 放自己弹的（持久化在 flash）
任何场景：长按旋钮 ≥5 秒 → 回主目录
主目录转旋钮 ＝ 调灯亮度（掉电记忆）｜ 其它模式转旋钮 ＝ 调音量
```

## 目录结构

| 目录 | 内容 |
|-|-|
| `firmware/` | 固件（ESP-IDF 工程）：`main/` 源码，`host_test/` 宿主测试（ctest） |
| `web/` | 网页端：单文件 `index.html`（曲库管理 ＋ 8 音跟弹闯关，走 Web Serial） |
| `tools/` | 生成语音语料 / 音色的脚本与素材、串口日志工具 |
| `docs/` | 需求与决策记录、变更与会话记录（「资料先行」的证据） |

## 编译

```bash
. $IDF_PATH/export.sh          # ESP-IDF 5.5
cd firmware && idf.py build
```

## 烧录（⚠️ 有个坑）

```bash
esptool --chip esp32s3 -p <PORT> -b 460800 write_flash \
  0x0      bootloader/bootloader.bin \
  0x8000   partition-table.bin \
  0x10000  ota_data_initial.bin \
  0x20000  easyinput_piano.bin \
  0x9D8000 voice_prompts.vpmt
```

⚠️ **语音是单独一份 `voice_prompts.vpmt`，必须写到 `rec` 分区 offset `0x9D8000`。**
只烧主程序会出现「**静默无语音**」——听不到任何提示，但也不报错，很容易误判成硬件坏了。

## 宿主测试

```bash
cd firmware/host_test
cmake -B build-host && cmake --build build-host && ctest --test-dir build-host --output-on-failure
```

## 网页端

```bash
cd web && python -m http.server 8765
# 浏览器打开 http://127.0.0.1:8765/index.html
```

需要支持 Web Serial 的浏览器（Chrome / Edge）；页面里点「连接键盘」选 USB 串行设备。

## 硬件红线（别踩）

| ❌ 错误 | ✅ 正确 |
|-|-|
| S1 ＝ GPIO1 | S1 ＝ GPIO2 |
| S5 ＝ GPIO0 | S5 ＝ GPIO1 |
| S6 / S7 ＝ GPIO5 / 6 | S6 / S7 ＝ GPIO6 / 7 |
| GPIO8 只是灯的开关 | GPIO8 是灯＋麦＋喇叭的共享总电源 |
| 发黑帧 ＝ 关电源 | 黑帧只熄灯，不动 GPIO8 |
| BOOT 要按住再上电 | 开机状态下短按一下就能进下载模式 |
| GPIO26~37 可用 | 那是内存总线，绝对禁用 |

## 已知限制与下一步

- 板子按键连按 → 网页的跟手性仍在排查（这条链路的时序）
- 游戏端（网页 ＋ 板子）联动终验待补
- 8 个按键的音色可再细调，让它更像钢琴音
- 网页游戏界面继续美化、操作继续调顺
- 公网托管未做（Web Serial 只能在 https 或本机环境用），先用静态演示 ＋ 录屏
