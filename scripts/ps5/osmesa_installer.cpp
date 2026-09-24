#include <curl/curl.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr char kAppDirectory[] = "/data/homebrew/wiliwili";
constexpr char kLibraryDirectory[] = "/data/homebrew/wiliwili/lib";
constexpr char kIconDirectory[] = "/data/homebrew/wiliwili/sce_sys";
constexpr char kLibraryPath[] = "/data/homebrew/wiliwili/lib/libOSMesa.so.8";
constexpr char kLibraryPartialPath[] = "/data/homebrew/wiliwili/lib/libOSMesa.so.8.part";
constexpr char kPayloadPath[] = "/data/homebrew/wiliwili/wiliwili.elf";
constexpr char kPayloadPartialPath[] = "/data/homebrew/wiliwili/wiliwili.elf.part";
constexpr char kCaPath[] = "/data/homebrew/wiliwili/ca-bundle.crt";
constexpr char kCaPartialPath[] = "/data/homebrew/wiliwili/ca-bundle.crt.part";
constexpr char kIconPath[] = "/data/homebrew/wiliwili/sce_sys/icon0.png";
constexpr char kIconPartialPath[] = "/data/homebrew/wiliwili/sce_sys/icon0.png.part";
constexpr char kExtensionPath[] = "/data/homebrew/wiliwili/homebrew.js";
constexpr char kLegacyPayloadPath[] = "/data/homebrew/wiliwili/eboot.elf";
constexpr char kExtensionPartialPath[] = "/data/homebrew/wiliwili/homebrew.js.part";

size_t write_file(char* data, size_t size, size_t count, void* userdata) {
    return std::fwrite(data, size, count, static_cast<FILE*>(userdata));
}

bool ensure_directory(const char* path) {
    if (mkdir(path, 0777) == 0 || errno == EEXIST) {
        return true;
    }
    std::fprintf(stderr, "mkdir %s: %s\n", path, std::strerror(errno));
    return false;
}

bool download_file(CURL* curl, const char* url, const char* partial_path,
                   const char* output_path, mode_t mode) {
    FILE* output = std::fopen(partial_path, "wb");
    if (!output) {
        std::fprintf(stderr, "open %s: %s\n", partial_path, std::strerror(errno));
        return false;
    }

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, output);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "wiliwili-ps5-installer/1");
    const CURLcode result = curl_easy_perform(curl);

    const int flush_result = std::fflush(output);
    const int sync_result = flush_result == 0 ? fsync(fileno(output)) : -1;
    const int close_result = std::fclose(output);
    if (result != CURLE_OK || flush_result != 0 || sync_result != 0 || close_result != 0) {
        std::fprintf(stderr, "download %s failed: %s\n", url, curl_easy_strerror(result));
        unlink(partial_path);
        return false;
    }

    struct stat st {};
    if (stat(partial_path, &st) != 0 || st.st_size == 0) {
        std::fprintf(stderr, "invalid download %s: %s\n", partial_path, std::strerror(errno));
        unlink(partial_path);
        return false;
    }
    if (rename(partial_path, output_path) != 0) {
        std::fprintf(stderr, "rename %s: %s\n", output_path, std::strerror(errno));
        unlink(partial_path);
        return false;
    }
    if (chmod(output_path, mode) != 0) {
        std::fprintf(stderr, "chmod %s: %s\n", output_path, std::strerror(errno));
        return false;
    }

    std::printf("installed %s (%lld bytes)\n", output_path, static_cast<long long>(st.st_size));
    return true;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: %s <osmesa-url> <wiliwili-url> <ca-bundle-url> <icon-url> <homebrew-js-url>\n",
                     argc > 0 ? argv[0] : "osmesa-installer");
        return 2;
    }
    if (!ensure_directory(kAppDirectory) || !ensure_directory(kLibraryDirectory) || !ensure_directory(kIconDirectory)) {
        return 1;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        std::fputs("curl_global_init failed\n", stderr);
        return 1;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        curl_global_cleanup();
        std::fputs("curl_easy_init failed\n", stderr);
        return 1;
    }
    unlink(kLegacyPayloadPath);

    bool success = download_file(curl, argv[1], kLibraryPartialPath, kLibraryPath, 0644);
    if (success) {
        success = download_file(curl, argv[3], kCaPartialPath, kCaPath, 0644);
    }
    if (success) {
        success = download_file(curl, argv[2], kPayloadPartialPath, kPayloadPath, 0755);
    }
    if (success) {
        success = download_file(curl, argv[4], kIconPartialPath, kIconPath, 0644);
    }
    if (success) {
        success = download_file(curl, argv[5], kExtensionPartialPath, kExtensionPath, 0644);
    }

    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return success ? 0 : 1;
}
