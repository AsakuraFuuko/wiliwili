/*
 * 「资源只保留一份」用的空 romfs 表。
 *
 * 资源已全部以松散文件随包发布到镜像内 /app0/assets/（见 notes/06 §10.29 与
 * run-continuation/ps5-native-handoff.md），内嵌副本成了纯冗余：libromfs 的字节表
 * 由生成的 libromfs_resources.cpp 提供（约 12.8 MB），和 romfs.cpp 一起打进
 * libromfs-wiliwili.a。静态库按需抽取成员——本文件先把 RomFs_wiliwili_* 三个符号
 * 定义好，链接器就不会再去抽取那个生成对象，字节表因此不再进入 eboot（实测 eboot
 * 63 MB → 50 MB），而 romfs::get/list 的代码路径与语义完全不变：
 * overlay 未命中时仍抛 std::invalid_argument("Invalid romfs resource path ...")。
 *
 * 想恢复内嵌回退：构建时设 PS5_NATIVE_ROMFS_EMBED=1（native_build.py 会跳过本文件）。
 */
#include <romfs/romfs.hpp>

#include <map>
#include <string>
#include <vector>

const std::map<fs::path, romfs::Resource> &RomFs_wiliwili_get_resources() {
    static const std::map<fs::path, romfs::Resource> empty;
    return empty;
}

const std::vector<fs::path> &RomFs_wiliwili_get_paths() {
    static const std::vector<fs::path> empty;
    return empty;
}

const std::string &RomFs_wiliwili_get_name() {
    static const std::string name = "wiliwili";
    return name;
}

/* 供 native_shims.c 的启动日志区分「空表/内嵌」两种镜像；内嵌构建里没有这个符号，
 * C 侧按 weak 引用处理（见 native_fs_probe 的 res: 行）。 */
extern "C" const char *wiliwili_romfs_state = "empty";
