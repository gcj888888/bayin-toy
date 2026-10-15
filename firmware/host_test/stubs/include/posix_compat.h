/*
 * 宿主测试用的 POSIX 兼容垫片。
 *
 * host_link.c 在 host_link_init 里用 fcntl / F_GETFL / F_SETFL / O_NONBLOCK
 * 把 stdin 设成非阻塞（这是 ESP-IDF 的真实写法，烧录固件里没问题）。
 * 但 MinGW-w64 默认不暴露这些符号，而宿主测试根本不调用 host_link_init，
 * 只需“能编译、能链接”即可。这里补上最小定义。
 */
#pragma once

#include <fcntl.h>
#include <unistd.h>

#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x0004
#endif

#if !defined(fcntl)
static inline int host_test_fcntl_shim(int fd, int cmd, int arg)
{
    (void)fd;
    (void)cmd;
    (void)arg;
    return 0;
}
#define fcntl host_test_fcntl_shim
#endif
