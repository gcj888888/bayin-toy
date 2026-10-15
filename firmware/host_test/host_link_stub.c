/* 宿主测试用的 host_link 发送替身：
 * 记录最后发出的一行 JSON（recorder 发乐谱清单时复用 host_link_send_line）。 */
#include "host_link.h"

#include <string.h>

char g_last_line[512];

void host_link_send_line(const char *line)
{
    if (line == NULL) {
        g_last_line[0] = '\0';
        return;
    }
    strncpy(g_last_line, line, sizeof(g_last_line) - 1);
    g_last_line[sizeof(g_last_line) - 1] = '\0';
}
