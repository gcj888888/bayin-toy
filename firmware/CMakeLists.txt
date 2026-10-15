cmake_minimum_required(VERSION 3.16)

# ★ 八音琴：本工程内禁用「四固件一键切换」。
#   原先通过 EXTRA_COMPONENT_DIRS 引入固件切换项目的 fw_switch / fw_switch_glue
#   组件（"长按旋钮 3 秒=切固件"会抢走八音琴的 5 秒手势）。这里不再引入该目录，
#   也不 REQUIRES fw_switch_glue（见 main/CMakeLists.txt），状态机里无任何调用。
#   不改动其它工程。

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(easyinput_piano)
