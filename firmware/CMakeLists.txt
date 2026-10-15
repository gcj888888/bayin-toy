cmake_minimum_required(VERSION 3.16)

# ★ 八音琴：本工程内禁用「四固件一键切换」。
#   原先通过 EXTRA_COMPONENT_DIRS 引入固件切换项目的 fw_switch / fw_switch_glue
#   组件（"长按旋钮 3 秒=切固件"会抢走八音琴的 5 秒手势）。这里不再引入该目录，
#   也不 REQUIRES fw_switch_glue（见 main/CMakeLists.txt），状态机里无任何调用。
#   不改动其它工程。

# 显式固定固件版本号。
#   本目录外层 E:\ 根目录恰好是一个无关的第三方 git 仓库（带 tag 2.4.0），
#   IDF 默认用 `git describe` 猜版本，会误取到 "2.4.0-dirty"。
#   这里显式写死，避免固件自报一个不相干的版本号。
set(PROJECT_VER "1.0.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(easyinput_piano)
