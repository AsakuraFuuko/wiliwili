/*
 * wiliwili PS5 原生标题：通过 getdents 做一层目录枚举。
 * Copyright (C) 2026 wiliwili contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 标题沙箱会让 libc 的 opendir() 对 /app0、/app0/assets 等路径返回 EPERM；
 * 但直接 open(O_DIRECTORY)+getdents() 可用，所以外置资源必须从这里枚举。
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* SDK 头文件没有在当前声明开关下暴露这个 FreeBSD 入口，故在此补声明。 */
int getdents(int fd, char *buf, int nbytes);

#ifndef O_DIRECTORY
/* 某些诊断用 clean-room 头文件没有 O_DIRECTORY；保留 FreeBSD 的值以便编译。 */
#define O_DIRECTORY 0x00020000
#endif

/* getdents 要求缓冲区不小于文件系统块大小，否则会返回 EINVAL；PS5 上相关
 * UFS/APFS 挂载实测需要 64 KB，因此固定使用该大小而不是较小的临时缓冲区。 */
#define WILIWILI_DIRENT_BUF_SIZE (64 * 1024)

/*
 * 枚举 path 的直接子项。
 * names 是调用者提供的 [maxNames][256] 数组；跳过 . 和 ..，成功返回写入数，
 * open/getdents 或损坏记录失败返回 -1。超过 maxNames 的项只丢弃名字，仍继续
 * 消耗目录记录，避免把“达到上限”误报成系统错误。
 */
int wiliwili_list_dir(const char *path, char names[][256], int maxNames) {
    if (path == NULL || names == NULL || maxNames <= 0) {
        errno = EINVAL;
        return -1;
    }

    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        /* 个别文件系统不接受 O_DIRECTORY；回退到普通只读 open，保持与参考实现
         * 相同的兼容路径，同时仍由 getdents 判断该 fd 是否可枚举。 */
        fd = open(path, O_RDONLY);
    }
    if (fd < 0) return -1;

    char *buffer = malloc(WILIWILI_DIRENT_BUF_SIZE);
    if (buffer == NULL) {
        const int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    int count = 0;
    for (;;) {
        const int got = getdents(fd, buffer, WILIWILI_DIRENT_BUF_SIZE);
        if (got < 0) {
            count = -1;
            break;
        }
        if (got == 0) break;

        const char *cursor = buffer;
        const char *end = buffer + got;
        while (cursor < end) {
            const struct dirent *entry = (const struct dirent *)cursor;
            const size_t record_len = entry->d_reclen;
            if (record_len == 0 || record_len > (size_t)(end - cursor)) {
                /* d_reclen 是步进依据；坏记录若只 break 会把枚举结果伪装成成功。 */
                errno = EIO;
                count = -1;
                break;
            }

            if (entry->d_namlen != 0) {
                const size_t raw_name_len = entry->d_namlen;
                const size_t name_offset = offsetof(struct dirent, d_name);
                if (record_len < name_offset || raw_name_len > record_len - name_offset) {
                    /* d_namlen 不能越过当前记录，否则复制会读到下一条记录。 */
                    errno = EIO;
                    count = -1;
                    break;
                }
                size_t name_len = raw_name_len;
                if (name_len > 255) name_len = 255;
                const char *name = entry->d_name;
                const int is_dot = name_len == 1 && name[0] == '.';
                const int is_dotdot = name_len == 2 && name[0] == '.' && name[1] == '.';
                if (!is_dot && !is_dotdot && count < maxNames) {
                    memcpy(names[count], name, name_len);
                    names[count][name_len] = '\0';
                    ++count;
                }
            }

            /* FreeBSD dirent 记录不是定长结构，必须按 d_reclen 跳到下一条。 */
            cursor += record_len;
        }
        if (count < 0) break;
    }

    const int saved = errno;
    free(buffer);
    close(fd);
    if (count < 0) errno = saved;
    return count;
}
