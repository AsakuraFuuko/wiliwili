//
// Created by fang on 2022/7/16.
//

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(PS5_NATIVE_APP)
#include <sys/select.h>
#endif
#include <borealis/core/application.hpp>
#include <borealis/core/cache_helper.hpp>
#include <borealis/core/thread.hpp>
#include <stb_image.h>

#include "utils/image_helper.hpp"
#include "utils/string_helper.hpp"
#include "api/bilibili/util/http.hpp"

#ifdef USE_WEBP
#include <webp/decode.h>
#endif

#if defined(PS5_NATIVE_APP)
extern "C" void wiliwili_boot_log(const char*);
#endif

#ifdef BOREALIS_USE_GXM
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#define STB_DXT_IMPLEMENTATION
#include <borealis/extern/nanovg/stb_dxt.h>
#include <borealis/extern/nanovg/nanovg_gxm.h>

static inline __attribute__((always_inline)) uint32_t nearest_po2(uint32_t val) {
    val--;
    val |= val >> 1;
    val |= val >> 2;
    val |= val >> 4;
    val |= val >> 8;
    val |= val >> 16;
    val++;

    return val;
}

static inline __attribute__((always_inline)) uint64_t morton_1(uint64_t x) {
    x = x & 0x5555555555555555;
    x = (x | (x >> 1)) & 0x3333333333333333;
    x = (x | (x >> 2)) & 0x0F0F0F0F0F0F0F0F;
    x = (x | (x >> 4)) & 0x00FF00FF00FF00FF;
    x = (x | (x >> 8)) & 0x0000FFFF0000FFFF;
    x = (x | (x >> 16)) & 0xFFFFFFFFFFFFFFFF;
    return x;
}

static inline __attribute__((always_inline)) void d2xy_morton(uint64_t d, uint64_t* x, uint64_t* y) {
    *x = morton_1(d);
    *y = morton_1(d >> 1);
}

static inline __attribute__((always_inline)) void extract_block(const uint8_t* src, uint32_t width, uint8_t* block) {
    for (int j = 0; j < 4; j++) {
        memcpy(&block[j * 4 * 4], src, 16);
        src += width * 4;
    }
}

/**
 * Compress RGBA data to DXT1 or DXT5
 * @param dst DXT data
 * @param src RGBA data
 * @param w source width
 * @param h source height
 * @param stride source stride
 * @param last_size max block size in pixel of last compression round, default is 64
 * @param isdxt5 false for DXT1, true for DXT5
 */
static void dxt_compress_ext(uint8_t* dst, uint8_t* src, uint32_t w, uint32_t h, uint32_t stride, uint32_t last_size,
                             bool isdxt5) {
    uint8_t block[64];
    uint32_t align_w          = MAX(nearest_po2(w), last_size);
    uint32_t align_h          = MAX(nearest_po2(h), last_size);
    uint32_t s                = MIN(align_w, align_h);
    uint32_t num_blocks       = s * s / 16;
    const uint32_t block_size = isdxt5 ? 16 : 8;
    uint64_t d, offs_x, offs_y;

    for (d = 0; d < num_blocks; d++, dst += block_size) {
        d2xy_morton(d, &offs_x, &offs_y);
        if (offs_x * 4 >= h || offs_y * 4 >= w) continue;
        extract_block(src + offs_y * 16 + offs_x * stride * 16, stride, block);
        stb_compress_dxt_block(dst, block, isdxt5, STB_DXT_NORMAL);
    }
    if (align_w > align_h) return dxt_compress_ext(dst, src + s * 4, w - s, h, stride, s, isdxt5);
    if (align_w < align_h) return dxt_compress_ext(dst, src + stride * s * 4, w, h - s, stride, s, isdxt5);
}

static void dxt_compress(uint8_t* dst, uint8_t* src, uint32_t w, uint32_t h, bool isdxt5) {
    dxt_compress_ext(dst, src, w, h, w, 64, isdxt5);
}
#endif

#if defined(PS5_NATIVE_APP)
static constexpr size_t MAX_IMAGE_REQUEST_THREADS = 8;
static constexpr unsigned MAX_IMAGE_RETRIES       = 2;
static constexpr int IMAGE_CONNECTION_TIMEOUT_MS  = 3000;
/* 无进度请求（尚未收到任何响应字节）的硬期限：真机实测新建连接的 TLS 握手会在
 * curl_multi_socket_action 内同步阻塞 1.4–2.1 s，watchdog 在调用内部无法运行；
 * 超过这里就主动释放 lane 并交给重试，避免一个卡死连接把队列拖到超时上限。 */
static constexpr int IMAGE_NO_PROGRESS_DEADLINE_MS = 6000;
#endif

#if defined(PS5_NATIVE_APP)
// 慢请求相关性：给 img-net 行带上 host 与进程内 uptime，便于和 UDP 日志时间戳对齐。
static long long imageUptimeMs() {
    static const auto start = std::chrono::steady_clock::now();
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
        .count();
}
static std::string imageUrlHost(const std::string& url) {
    const auto scheme  = url.find("://");
    const size_t begin = scheme == std::string::npos ? 0 : scheme + 3;
    const auto slash   = url.find('/', begin);
    const auto colon   = url.find(':', begin);
    size_t stop        = slash;
    if (colon != std::string::npos && (stop == std::string::npos || colon < stop)) stop = colon;
    return url.substr(begin, stop == std::string::npos ? std::string::npos : stop - begin);
}
#endif

static size_t effectiveImageRequestThreads(size_t configured) {
#if defined(PS5_NATIVE_APP)
    return std::min(std::max<size_t>(2, configured), MAX_IMAGE_REQUEST_THREADS);
#else
    return configured == 0 ? 1 : configured;
#endif
}

// Separate CURLM lanes isolate stalled image transfers; each worker drives socket_action with a bounded select wait so watchdog and cancellation checks stay schedulable.
#if defined(PS5_NATIVE_APP)

// 真机实测 worker 会在 libcurl 调用内阻塞 57–113 s，watchdog 无法在调用内部运行；先定位阻塞的具体调用。
static std::atomic<unsigned> imageStallLogs{0};
static void logImageStall(const char* operation, std::chrono::steady_clock::time_point started, int running,
                          size_t active) {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    if (elapsed.count() < 1000 || imageStallLogs.fetch_add(1) >= 64) return;
    char message[160];
    std::snprintf(message, sizeof(message), "img-stall: op=%s ms=%lld running=%d active=%zu", operation,
                  static_cast<long long>(elapsed.count()), running, active);
    wiliwili_boot_log(message);
}
/* 慢传输跟踪（诊断）：把 curl 的 VERBOSE 事件按"相对开始时刻"记下来，只在传输
 * 结果很慢（≥1 s）时 dump。curl 的 TEXT 事件覆盖连接/TLS 阶段，SSL_DATA_IN/OUT
 * 给出加密层收发时刻，用它能定位 2 s 级卡顿到底停在握手哪一步。 */
struct TransferTrace {
    std::chrono::steady_clock::time_point start;
    std::string lines;
    int count = 0;
};

static int imageDebugCallback(CURL*, curl_infotype type, char* data, size_t size, void* userptr) {
    auto* trace = static_cast<TransferTrace*>(userptr);
    if (trace == nullptr || trace->count >= 160 || trace->lines.size() > 7000) return 0;
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - trace->start).count();
    if (type == CURLINFO_TEXT) {
        std::string text(data, size);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        trace->lines += "+" + std::to_string(ms) + "ms " + text + "\n";
    } else if (type == CURLINFO_SSL_DATA_IN) {
        trace->lines += "+" + std::to_string(ms) + "ms ssl-in " + std::to_string(size) + "B\n";
    } else if (type == CURLINFO_SSL_DATA_OUT) {
        trace->lines += "+" + std::to_string(ms) + "ms ssl-out " + std::to_string(size) + "B\n";
    } else {
        return 0;
    }
    ++trace->count;
    return 0;
}

class ImageRequestRunner {
    struct Request {
        std::string url;
        std::function<bool()> isCancelled;
        std::function<void(cpr::Response)> complete;
        bool priority;
        std::chrono::steady_clock::time_point queuedAt;
        unsigned attempt;
    };

    struct ActiveRequest {
        Request request;
        std::shared_ptr<cpr::Session> session;
        std::chrono::steady_clock::time_point startedAt;
        std::shared_ptr<TransferTrace> trace;
    };

public:
    static ImageRequestRunner& instance() {
        static ImageRequestRunner runner(ImageHelper::REQUEST_THREADS);
        return runner;
    }

    explicit ImageRequestRunner(size_t maxInFlight) : maxInFlight(effectiveImageRequestThreads(maxInFlight)) {
        for (size_t workerIndex = 0; workerIndex < MAX_IMAGE_REQUEST_THREADS; ++workerIndex) {
            workers.emplace_back([this, workerIndex] { run(workerIndex); });
        }
    }

    ~ImageRequestRunner() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        condition.notify_all();
        for (auto& worker : workers) {
            if (worker.joinable()) worker.join();
        }
        for (;;) {
            Request request;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!hasRequests()) break;
                request = popRequest();
            }
            deliver(request, cpr::Response{});
        }
    }

    void enqueue(std::string url, std::function<bool()> isCancelled, std::function<void(cpr::Response)> complete) {
        const bool priority = url.find("!note-comment-multiple") == std::string::npos;
        bool rejected;
        {
            std::lock_guard<std::mutex> lock(mutex);
            rejected = stopping;
            if (!rejected) {
                pushRequest(Request{std::move(url), std::move(isCancelled), std::move(complete), priority,
                                    std::chrono::steady_clock::now(), 0});
            }
        }
        if (rejected) {
            complete(cpr::Response{});
            return;
        }
        condition.notify_all();
    }

    void setMaxInFlight(size_t value) {
        maxInFlight = effectiveImageRequestThreads(value);
        condition.notify_all();
    }

private:
    void pushRequest(Request request) {
        (request.priority ? priorityRequests : normalRequests).push_back(std::move(request));
    }

    bool hasRequests() const { return !priorityRequests.empty() || !normalRequests.empty(); }

    Request popRequest() {
        auto& queue     = priorityRequests.empty() ? normalRequests : priorityRequests;
        Request request = std::move(queue.front());
        queue.pop_front();
        return request;
    }
    static void deliver(Request& request, cpr::Response response) {
        try {
            request.complete(std::move(response));
        } catch (...) {
            // The completion callback only schedules decode work; keep exceptions from terminating the network owner.
        }
    }

    std::shared_ptr<cpr::Session> createSession(const Request& request) {
        auto session = std::make_shared<cpr::Session>();
        CURL* curl   = session->GetCurlHolder()->handle;
        curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, bilibili::HTTP::DNS_CACHE_TIMEOUT);
#ifdef PS5
        curl_easy_setopt(curl, CURLOPT_CAINFO, bilibili::HTTP::CA_BUNDLE);
#if defined(PS5_NATIVE_APP)
        /* CA store 的构建是重活（解析 PEM + 建 X509_STORE，OpenSSL 内部锁很多，
         * 而标题沙箱里争用锁 14-17us/次，见 notes/06 §10.29）。curl 默认
         * ca_cache_timeout=0 等于**每条新连接重建一次**；开了之后解析好的 store 挂在
         * 本 worker 的 multi handle 上（multi->proto_hash + X509_STORE_up_ref），
         * 该 multi 下所有 easy handle 共用。*/
        curl_easy_setopt(curl, CURLOPT_CA_CACHE_TIMEOUT, 3600L);
#endif
#endif
        session->SetTimeout(cpr::Timeout{bilibili::HTTP::TIMEOUT});
#if defined(PS5_NATIVE_APP)
        // Keep both cPR and the easy handle configured; native curl has observed multi-state stalls
        // where the application watchdog cannot run until curl returns.
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)bilibili::HTTP::TIMEOUT);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)IMAGE_CONNECTION_TIMEOUT_MS);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 6L);
        /* 让连接池里的 CDN 连接活得更久，减少重复握手（TLS 是当前最贵的一段）。 */
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 30L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 15L);
#else
        session->SetConnectTimeout(cpr::ConnectTimeout{bilibili::HTTP::CONNECTION_TIMEOUT});
#endif
        session->SetVerifySsl(bilibili::HTTP::VERIFY);
        session->SetProxies(bilibili::HTTP::PROXIES);
        session->SetUrl(cpr::Url{request.url});
        session->SetProgressCallback(
            cpr::ProgressCallback([isCancelled = request.isCancelled](...) { return !isCancelled(); }));
        session->PrepareGet();
        return session;
    }

    void logSlowTransfer(CURL* curl, const Request& request, const cpr::Response& response,
                         std::chrono::steady_clock::time_point startedAt) {
        const auto totalMs       = (long long)(response.elapsed * 1000);
        const bool initialSample = loggedTransfers.fetch_add(1) < 12;
        const long long queueMs =
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(startedAt - request.queuedAt).count();

        double dns = 0, connected = 0, tls = 0, firstByte = 0;
        long newConnections = 0;
        /* 传输实际用的 socket fd：标题的 fd 上限是 13952（远高于 FD_SETSIZE=1024），
         * 如果慢样本的 fd 全在 1024 以上，就能解释"只有 curl 传输慢"（curl 的
         * 多路复用等待走 select 时 fd>=FD_SETSIZE 是未定义行为）。 */
        long lastSocket = -1;
        curl_easy_getinfo(curl, CURLINFO_LASTSOCKET, &lastSocket);
        curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &dns);
        curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connected);
        curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &tls);
        curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &firstByte);
        curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &newConnections);
        if (totalMs < 250 && !initialSample) return;
        char message[320];
        const std::string host = imageUrlHost(request.url);
        std::snprintf(
            message, sizeof(message),
            "img-net: total=%lldms queue=%lldms dns=%lld tcp=%lld tls=%lld first=%lld newconn=%ld code=%ld bytes=%lld "
            "host=%s fd=%ld up=%lldms",
            totalMs, queueMs, (long long)(dns * 1000), (long long)((connected - dns) * 1000),
            (long long)((tls - connected) * 1000), (long long)(firstByte * 1000), newConnections, response.status_code,
            (long long)response.downloaded_bytes, host.c_str(), lastSocket, imageUptimeMs());
        wiliwili_boot_log(message);
    }

    /* 慢传输的 curl 事件时间线（最多 8 次 dump，避免刷日志）。 */
    static void dumpTransferTrace(const ActiveRequest& request, long long elapsedMs) {
        static std::atomic<unsigned> dumps{0};
        if (!request.trace || request.trace->count == 0) return;
        if (dumps.fetch_add(1) >= 1) return;
        char head[160];
        const std::string host = imageUrlHost(request.request.url);
        std::snprintf(head, sizeof(head), "img-trace: begin elapsed=%lldms lines=%d host=%s", elapsedMs,
                      request.trace->count, host.c_str());
        wiliwili_boot_log(head);
        const std::string& lines = request.trace->lines;
        size_t pos               = 0;
        while (pos < lines.size()) {
            size_t end = lines.find('\n', pos);
            if (end == std::string::npos) end = lines.size();
            std::string line = "img-trace:  " + lines.substr(pos, end - pos);
            wiliwili_boot_log(line.c_str());
            pos = end + 1;
        }
        wiliwili_boot_log("img-trace: end");
    }

    void finish(CURLM* multi, std::unordered_map<CURL*, ActiveRequest>& active, CURL* curl, CURLcode result) {
        auto it = active.find(curl);
        if (it == active.end()) return;

        ActiveRequest request = std::move(it->second);
        active.erase(it);
        cpr::Response response = request.session->Complete(result);
        const bool cancelled   = result == CURLE_ABORTED_BY_CALLBACK || request.request.isCancelled();
        const bool failed      = result != CURLE_OK || response.status_code != 200 || response.downloaded_bytes == 0;
        if (result == CURLE_OK && !failed) {
            logSlowTransfer(curl, request.request, response, request.startedAt);
            const auto elapsed = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - request.startedAt)
                                     .count();
            if (elapsed >= 1000) dumpTransferTrace(request, elapsed);
        } else if (!cancelled) {
            char message[192];
            const auto elapsed = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - request.startedAt)
                                     .count();
            const std::string host = imageUrlHost(request.request.url);
            std::snprintf(message, sizeof(message),
                          "img-net: failed curl=%d code=%ld bytes=%lld elapsed=%lldms try=%u host=%s up=%lldms", result,
                          response.status_code, (long long)response.downloaded_bytes, elapsed,
                          request.request.attempt + 1, host.c_str(), imageUptimeMs());
            wiliwili_boot_log(message);
        }
        curl_multi_remove_handle(multi, curl);

        if (failed && !cancelled && request.request.attempt < MAX_IMAGE_RETRIES) {
            bool requeued = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!stopping) {
                    request.request.attempt++;
                    request.request.queuedAt = std::chrono::steady_clock::now();
                    pushRequest(std::move(request.request));
                    requeued = true;
                }
            }
            if (requeued) {
                condition.notify_one();
                return;
            }
        }
        deliver(request.request, std::move(response));
    }
    void run(size_t workerIndex) {
        CURLM* multi = curl_multi_init();
        if (!multi) {
            for (;;) {
                Request request;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    condition.wait(lock, [this, workerIndex] {
                        return stopping || (workerIndex < maxInFlight.load() && hasRequests());
                    });
                    if (stopping) return;
                    request = popRequest();
                }
                deliver(request, cpr::Response{});
            }
        }

        if (workerIndex == 0) {
            const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
            const bool asyncDns                = info && (info->features & CURL_VERSION_ASYNCHDNS);
            char startupMessage[112];
            std::snprintf(startupMessage, sizeof(startupMessage), "img-multi: lanes=%zu max-inflight=%zu async-dns=%d",
                          MAX_IMAGE_REQUEST_THREADS, maxInFlight.load(), asyncDns ? 1 : 0);
            wiliwili_boot_log(startupMessage);
        }
        curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long)MAX_IMAGE_REQUEST_THREADS);
        curl_multi_setopt(multi, CURLMOPT_MAXCONNECTS, (long)(MAX_IMAGE_REQUEST_THREADS * 4));
        std::unordered_map<CURL*, ActiveRequest> active;

        for (;;) {
            Request request;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this, workerIndex] {
                    return stopping || (workerIndex < maxInFlight.load() && hasRequests());
                });
                if (stopping) break;
                request = popRequest();
            }
            if (request.isCancelled()) {
                deliver(request, cpr::Response{});
                continue;
            }

            auto session          = createSession(request);
            CURL* curl            = session->GetCurlHolder()->handle;
            const CURLMcode added = curl_multi_add_handle(multi, curl);
            if (added != CURLM_OK) {
                cpr::Response response = session->Complete(CURLE_FAILED_INIT);
                deliver(request, std::move(response));
                continue;
            }
            /* 逐事件跟踪默认关闭（curl VERBOSE 对每个传输都有开销）；需要时用
             * WILIWILI_IMG_TRACE=1 打开，慢传输（≥1 s）才会 dump。 */
            static const bool traceEnabled = std::getenv("WILIWILI_IMG_TRACE") != nullptr;
            std::shared_ptr<TransferTrace> trace;
            if (traceEnabled) {
                trace        = std::make_shared<TransferTrace>();
                trace->start = std::chrono::steady_clock::now();
                curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
                curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, imageDebugCallback);
                curl_easy_setopt(curl, CURLOPT_DEBUGDATA, trace.get());
            }
            active.emplace(curl, ActiveRequest{std::move(request), std::move(session), std::chrono::steady_clock::now(),
                                               std::move(trace)});

            while (!active.empty()) {
                bool shouldStop;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    shouldStop = stopping;
                }
                if (shouldStop) {
                    finish(multi, active, active.begin()->first, CURLE_ABORTED_BY_CALLBACK);
                    break;
                }

                auto process = [&]() {
                    int messages = 0;
                    while (CURLMsg* message = curl_multi_info_read(multi, &messages)) {
                        if (message->msg == CURLMSG_DONE) {
                            finish(multi, active, message->easy_handle, message->data.result);
                        }
                    }
                    const auto now      = std::chrono::steady_clock::now();
                    const auto watchdog = std::chrono::milliseconds(bilibili::HTTP::TIMEOUT) + std::chrono::seconds(5);
                    const auto noProgressDeadline = std::chrono::milliseconds(IMAGE_NO_PROGRESS_DEADLINE_MS);
                    for (auto it = active.begin(); it != active.end();) {
                        CURL* activeCurl     = it->first;
                        const bool cancelled = it->second.request.isCancelled();
                        /* 有进度（收到过字节）的请求沿用完整 watchdog；完全没有响应的请求
                         * 更早释放 lane，避免排队被单个卡死连接拖住。 */
                        curl_off_t downloaded = 0;
                        curl_easy_getinfo(activeCurl, CURLINFO_SIZE_DOWNLOAD_T, &downloaded);
                        const auto deadline = downloaded > 0 ? watchdog : noProgressDeadline;
                        const bool timedOut = now - it->second.startedAt >= deadline;
                        ++it;
                        if (cancelled) {
                            finish(multi, active, activeCurl, CURLE_ABORTED_BY_CALLBACK);
                        } else if (timedOut) {
                            finish(multi, active, activeCurl, CURLE_OPERATION_TIMEDOUT);
                        }
                    }
                };

                int running              = 0;
                const auto actionStarted = std::chrono::steady_clock::now();
                CURLMcode result         = curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, 0, &running);
                logImageStall("socket_action", actionStarted, running, active.size());
                if (result != CURLM_OK) {
                    finish(multi, active, active.begin()->first, CURLE_FAILED_INIT);
                    continue;
                }
                process();
                if (active.empty()) break;

                fd_set readfds;
                fd_set writefds;
                fd_set exceptfds;
                FD_ZERO(&readfds);
                FD_ZERO(&writefds);
                FD_ZERO(&exceptfds);
                int maxfd = -1;
                if (curl_multi_fdset(multi, &readfds, &writefds, &exceptfds, &maxfd) != CURLM_OK) maxfd = -1;

                long timeoutMs = 20;
                curl_multi_timeout(multi, &timeoutMs);
                if (timeoutMs < 0 || timeoutMs > 20) timeoutMs = 20;
                if (timeoutMs == 0) continue;

                timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
                const auto selectStarted = std::chrono::steady_clock::now();
                const int ready          = maxfd >= 0 ? select(maxfd + 1, &readfds, &writefds, &exceptfds, &timeout)
                                                      : select(0, nullptr, nullptr, nullptr, &timeout);
                logImageStall("select", selectStarted, running, active.size());
                if (ready <= 0) continue;

                for (int fd = 0; fd <= maxfd && !active.empty(); ++fd) {
                    int events = 0;
                    if (FD_ISSET(fd, &readfds)) events |= CURL_CSELECT_IN;
                    if (FD_ISSET(fd, &writefds)) events |= CURL_CSELECT_OUT;
                    if (FD_ISSET(fd, &exceptfds)) events |= CURL_CSELECT_ERR;
                    if (events != 0) {
                        const auto actionStarted = std::chrono::steady_clock::now();
                        result                   = curl_multi_socket_action(multi, fd, events, &running);
                        logImageStall("socket_fd", actionStarted, running, active.size());
                        if (result != CURLM_OK) {
                            finish(multi, active, active.begin()->first, CURLE_FAILED_INIT);
                            break;
                        }
                        process();
                    }
                }
            }
        }

        for (auto it = active.begin(); it != active.end();) {
            CURL* curl = it->first;
            ++it;
            finish(multi, active, curl, CURLE_ABORTED_BY_CALLBACK);
        }
        curl_multi_cleanup(multi);
    }

    std::mutex mutex;
    std::condition_variable condition;
    // Dynamic note grids are bursty; keep avatar/card requests ahead of them.
    std::deque<Request> priorityRequests;
    std::deque<Request> normalRequests;
    std::atomic<size_t> maxInFlight;
    std::atomic<unsigned> loggedTransfers{0};
    bool stopping = false;
    std::vector<std::thread> workers;
};
#endif

class ImageThreadPool : public cpr::ThreadPool, public brls::Singleton<ImageThreadPool> {
public:
    ImageThreadPool()
        : cpr::ThreadPool(1, effectiveImageRequestThreads(ImageHelper::REQUEST_THREADS),
                          std::chrono::milliseconds(5000)) {
        brls::Logger::info("image workers: configured={} effective={}", ImageHelper::REQUEST_THREADS,
                           this->max_thread_num);
        this->Start();
    }

    ~ImageThreadPool() override { this->Stop(); }

#if !defined(PS5_NATIVE_APP)
    CURL* getShare() { return share.getShare(); }

private:
    bilibili::CurlSharedObject share;
#endif
};

ImageHelper::ImageHelper(brls::Image* view) : imageView(view) {}

ImageHelper::~ImageHelper() { brls::Logger::verbose("delete ImageHelper {}", (size_t)this); }

std::shared_ptr<ImageHelper> ImageHelper::with(brls::Image* view) {
    std::lock_guard<std::mutex> lock(requestMutex);
    std::shared_ptr<ImageHelper> item;

    if (!requestPool.empty() && (*requestPool.begin())->getImageView() == nullptr) {
        // 复用请求，挪到队尾
        item = *requestPool.begin();
        item->setImageView(view);
        requestPool.splice(requestPool.end(), requestPool, requestPool.begin());
    } else {
        // 新建 ImageHelper 实例
        item = std::make_shared<ImageHelper>(view);
        requestPool.emplace_back(item);
    }

    auto iter         = --requestPool.end();
    requestMap[view]  = iter;
    item->currentIter = iter;
    // 重置 "取消" 标记位
    item->isCancel = false;
    // 禁止图片组件销毁
    item->imageView->ptrLock();
    // 设置图片组件不处理纹理的销毁，由缓存统一管理纹理销毁
    item->imageView->setFreeTexture(false);

    brls::Logger::verbose("with view: {} {} {}", (size_t)view, (size_t)item.get(), (size_t)(*iter).get());

    return item;
}

void ImageHelper::load(const std::string& url) {
    this->imageUrl       = bilibili::HTTP::VERIFY.verify ? url : pystring::replace(url, "https", "http", 1);
    this->decodeAttempts = 0;

#ifdef BOREALIS_USE_GXM
    std::vector<std::string> urls = pystring::rsplit(this->imageUrl, "@", 1);
    if (pystring::endswith(urls[0], "jpg")) {
        this->imageFlag = NVG_IMAGE_DXT1;
    } else {
        this->imageFlag = NVG_IMAGE_DXT5;
    }
#endif

    brls::Logger::verbose("load view: {} {}", (size_t)this->imageView, (size_t)this);

    //    std::unique_lock<std::mutex> lock(this->loadingMutex);

    // 检查是否存在缓存
    int tex = brls::TextureCache::instance().getCache(this->imageUrl);
    if (tex > 0) {
        brls::Logger::verbose("cache hit: {}", this->imageUrl);
        this->imageView->innerSetImage(tex);
        this->clean();
        return;
    }

    //todo: 可能会发生同时请求多个重复链接的情况，此种情况下最好合并为一个请求

    // 缓存网络图片
    brls::Logger::verbose("request Image 1: {} {}", this->imageUrl, this->isCancel.load());
    ImageThreadPool::instance().Submit([this]() {
        brls::Logger::verbose("Submit view: {} {} {} {}", (size_t)this->imageView, (size_t)this, this->imageUrl,
                              this->isCancel.load());
        if (this->isCancel) {
            this->clean();
            return;
        }
        this->requestImage();
    });
}

static inline void freeImageData(uint8_t* imageData, bool isWebp) {
    if (!imageData) return;
#ifdef USE_WEBP
    if (isWebp)
        WebPFree(imageData);
    else
#endif
        stbi_image_free(imageData);
}

/* Native image requests may finish on four runner lanes at once, while Borealis
 * drains every sync callback in one main-loop pass. Keep decoded pixels bounded
 * and submit at most two render-thread texture uploads per frame so AGC
 * allocation and view binding cannot turn a burst of completed requests into
 * one long stall. */
#if defined(PS5_NATIVE_APP)
struct DecodedImage {
    uint8_t* data   = nullptr;
    int width       = 0;
    int height      = 0;
    bool isWebp     = false;
    bool compressed = false;

    ~DecodedImage() {
        if (!data) return;
        if (compressed)
            free(data);
        else
            freeImageData(data, isWebp);
    }
};

class ImageUploadQueue {
public:
    using Task = std::function<void()>;

    static ImageUploadQueue& instance() {
        static ImageUploadQueue queue;
        return queue;
    }

    bool enqueue(Task task, std::function<bool()> cancelled, bool priority) {
        bool schedule = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            space.wait(lock, [&] { return stopping || urgent.size() + normal.size() < MAX_PENDING || cancelled(); });
            if (stopping || cancelled()) return false;
            (priority ? urgent : normal).emplace_back(std::move(task));
            if (!scheduled) {
                scheduled = true;
                schedule  = true;
            }
        }
        if (schedule) brls::sync([this] { runOne(); });
        return true;
    }

    void wake() { space.notify_all(); }

private:
    static constexpr size_t MAX_PENDING           = 8;
    static constexpr size_t MAX_UPLOADS_PER_FRAME = 2;

    void runOne() {
        for (size_t i = 0; i < MAX_UPLOADS_PER_FRAME; ++i) {
            Task task;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (urgent.empty() && normal.empty()) break;
                if (!urgent.empty()) {
                    task = std::move(urgent.front());
                    urgent.pop_front();
                } else {
                    task = std::move(normal.front());
                    normal.pop_front();
                }
                space.notify_one();
            }
            task();
        }

        bool schedule = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (urgent.empty() && normal.empty())
                scheduled = false;
            else
                schedule = true;
        }
        if (schedule) brls::sync([this] { runOne(); });
    }

    std::mutex mutex;
    std::condition_variable space;
    std::deque<Task> urgent;
    std::deque<Task> normal;
    bool scheduled = false;
    bool stopping  = false;
};
#endif

void ImageHelper::requestImage() {
    brls::Logger::verbose("request Image 2: {} {}", this->imageUrl, this->isCancel.load());
#if defined(PS5_NATIVE_APP)
    ImageRequestRunner::instance().enqueue(
        this->imageUrl, [this] { return this->isCancel.load(); },
        [this](cpr::Response response) {
            ImageThreadPool::instance().Submit(
                [this, response = std::move(response)]() mutable { this->handleImageResponse(std::move(response)); });
        });
#else
    cpr::Session session;
    CURL* curl = session.GetCurlHolder()->handle;
    curl_easy_setopt(curl, CURLOPT_SHARE, ImageThreadPool::instance().getShare());
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, bilibili::HTTP::DNS_CACHE_TIMEOUT);
#ifdef PS5
    curl_easy_setopt(curl, CURLOPT_CAINFO, bilibili::HTTP::CA_BUNDLE);
#endif
    session.SetTimeout(cpr::Timeout{bilibili::HTTP::TIMEOUT});
    session.SetConnectTimeout(cpr::ConnectTimeout{bilibili::HTTP::CONNECTION_TIMEOUT});
    session.SetVerifySsl(bilibili::HTTP::VERIFY);
    session.SetProxies(bilibili::HTTP::PROXIES);
    session.SetUrl(cpr::Url{this->imageUrl});
    session.SetProgressCallback(cpr::ProgressCallback([this](...) -> bool { return !this->isCancel; }));
    this->handleImageResponse(session.Get());
#endif
}

void ImageHelper::handleImageResponse(cpr::Response r) {
    // 图片请求失败或取消请求
    if (r.status_code != 200 || r.downloaded_bytes == 0 || this->isCancel) {
        brls::Logger::verbose("request undone: {} {} {} {}", r.status_code, r.downloaded_bytes, this->isCancel.load(),
                              r.url.str());
        this->clean();
        return;
    }
    const std::string& body     = r.text;
    const long downloaded_bytes = r.downloaded_bytes;

    brls::Logger::verbose("load pic:{} size:{} bytes by{} to {} {}", this->imageUrl, downloaded_bytes, (size_t)this,
                          (size_t)this->imageView, this->imageView->describe());

    /* 解码器要的是**我们真正持有的缓冲长度**，不是 curl 统计的 body 字节数：
     * downloaded_bytes 在压缩/部分传输/重试等情况下与 text 的长度并不一致，
     * 拿它当长度会读越界——真机播放页 2–4 分钟必崩，三次崩溃点都在
     * stbi__load_main / stbi__load_and_postprocess_8bit。取两者较小值，
     * 不一致时记一行（保留原值供排查）。 */
    const size_t imageBytes = std::min<size_t>(body.size(), (size_t)downloaded_bytes);
    if (imageBytes != (size_t)downloaded_bytes)
        brls::Logger::warning("image size mismatch: url={} downloaded={} text={}", this->imageUrl,
                              (long long)downloaded_bytes, (long long)body.size());

    uint8_t* imageData = nullptr;
    int imageW = 0, imageH = 0;
    bool isWebp = false;

#ifdef USE_WEBP
    if (imageUrl.size() > 5 && imageUrl.substr(imageUrl.size() - 5, 5) == ".webp") {
        imageData = WebPDecodeRGBA((const uint8_t*)body.c_str(), imageBytes, &imageW, &imageH);
        isWebp    = true;
    } else {
#endif
        int n;
        imageData = stbi_load_from_memory((unsigned char*)body.c_str(), (int)imageBytes, &imageW, &imageH, &n, 4);
#ifdef USE_WEBP
    }
#endif

#if defined(PS5_NATIVE_APP)
    if (!imageData) {
        const char* reason = isWebp ? "webp decode failed" : stbi_failure_reason();
        char message[192];
        std::snprintf(message, sizeof(message), "img-decode: bytes=%zu size=%dx%d reason=%s try=%u", imageBytes, imageW,
                      imageH, reason ? reason : "unknown", this->decodeAttempts + 1);
        wiliwili_boot_log(message);
        if (!this->isCancel && this->decodeAttempts < MAX_IMAGE_RETRIES) {
            ++this->decodeAttempts;
            this->requestImage();
            return;
        }
    }
#endif

#ifdef BOREALIS_USE_GXM
    if (imageData) {
        bool dxt5   = this->imageFlag & NVG_IMAGE_DXT5;
        size_t size = nearest_po2(imageW) * nearest_po2(imageH);
        if (!dxt5) size >> 1;
        auto* compressed = (uint8_t*)malloc(size);
        dxt_compress(compressed, imageData, imageW, imageH, dxt5);
        freeImageData(imageData, isWebp);
        imageData = compressed;
    }
#endif

#if defined(PS5_NATIVE_APP)
    auto decoded    = std::make_shared<DecodedImage>();
    decoded->data   = imageData;
    decoded->width  = imageW;
    decoded->height = imageH;
    decoded->isWebp = isWebp;
#ifdef BOREALIS_USE_GXM
    decoded->compressed = true;
#endif

    std::shared_ptr<ImageHelper> self;
    {
        std::lock_guard<std::mutex> lock(requestMutex);
        if (this->currentIter != requestPool.end()) self = *this->currentIter;
    }
    if (!self) {
        this->clean();
        return;
    }

    // Note grids are the bursty path; keep avatars and card covers responsive ahead of them.
    const bool priority = this->imageUrl.find("!note-comment-multiple") == std::string::npos;
    const bool queued   = ImageUploadQueue::instance().enqueue(
        [self = std::move(self), decoded = std::move(decoded)] {
            if (self->isCancel) {
                self->clean();
                return;
            }
            int tex = brls::TextureCache::instance().getCache(self->imageUrl);
            if (tex > 0) {
                if (!self->isCancel && self->imageView) self->imageView->innerSetImage(tex);
            } else {
                NVGcontext* vg = brls::Application::getNVGContext();
                if (decoded->data) {
                    tex = nvgCreateImageRGBA(vg, decoded->width, decoded->height, 0, decoded->data);
                } else {
                    brls::Logger::error("Failed to load image: {}", self->imageUrl);
                }
                if (tex > 0) {
                    brls::TextureCache::instance().addCache(self->imageUrl, tex);
                    if (!self->isCancel && self->imageView) {
                        brls::Logger::verbose("load image: {}", self->imageUrl);
                        self->imageView->innerSetImage(tex);
                    }
                }
            }
            self->clean();
        },
        [this] { return this->isCancel.load(); }, priority);
    if (!queued) this->clean();
#else
    brls::sync([this, imageData, imageW, imageH, isWebp]() {
        int tex = brls::TextureCache::instance().getCache(this->imageUrl);
        if (tex > 0) {
            if (this->imageView) this->imageView->innerSetImage(tex);
        } else {
            NVGcontext* vg = brls::Application::getNVGContext();
            if (imageData) {
                tex = nvgCreateImageRGBA(vg, imageW, imageH, 0, imageData);
            } else {
                brls::Logger::error("Failed to load image: {}", this->imageUrl);
            }
            if (tex > 0) {
                brls::TextureCache::instance().addCache(this->imageUrl, tex);
                if (!this->isCancel && this->imageView) {
                    brls::Logger::verbose("load image: {}", this->imageUrl);
                    this->imageView->innerSetImage(tex);
                }
            }
        }
        if (imageData) freeImageData(imageData, isWebp);
        this->clean();
    });
#endif
}

void ImageHelper::clean() {
    std::lock_guard<std::mutex> lock(requestMutex);

    // 允许图片组件销毁
    if (this->imageView) this->imageView->ptrUnlock();
    // 移除请求，复用 ImageHelper (挪到队首)
    requestPool.splice(requestPool.begin(), requestPool, this->currentIter);
    this->imageView   = nullptr;
    this->currentIter = requestPool.end();
}

void ImageHelper::clear(brls::Image* view) {
    brls::TextureCache::instance().removeCache(view->getTexture());
    view->clear();

    std::lock_guard<std::mutex> lock(requestMutex);

    // 请求不存在
    if (requestMap.find(view) == requestMap.end()) return;

    brls::Logger::verbose("clear view: {} {}", (size_t)view, (size_t)(*requestMap[view]).get());

    // 请求没结束，取消请求
    if ((*requestMap[view])->imageView == view) {
        (*requestMap[view])->cancel();
    }
    requestMap.erase(view);
}

void ImageHelper::cancel() {
    brls::Logger::verbose("Cancel request: {}", this->imageUrl);
    this->isCancel = true;
#if defined(PS5_NATIVE_APP)
    ImageUploadQueue::instance().wake();
#endif
}

void ImageHelper::setRequestThreads(size_t num) {
    REQUEST_THREADS                            = num;
    const size_t effectiveThreads              = effectiveImageRequestThreads(num);
    ImageThreadPool::instance().min_thread_num = 1;
    ImageThreadPool::instance().max_thread_num = effectiveThreads;
#if defined(PS5_NATIVE_APP)
    ImageRequestRunner::instance().setMaxInFlight(effectiveThreads);
#endif
}

void ImageHelper::setImageView(brls::Image* view) { this->imageView = view; }

brls::Image* ImageHelper::getImageView() { return this->imageView; }

std::string ImageHelper::parseGifImageUrl(const std::string& url, const std::string& ext) {
#ifdef USE_WEBP
    std::string image_url = url;
    if (pystring::endswith(url, "gif")) {
        // gif 图片暂时按照 jpg 来解析
        image_url += pystring::replace(ext, ".webp", ".jpg");
    } else {
        image_url += ext;
    }
    return image_url;
#else
    return url + ext;
#endif
}

std::string ImageHelper::parseNoteImageUrl(const std::string& url, size_t w, size_t h) {
    return parseGifImageUrl(url, wiliwili::format(note_custom_ext, w, h));
}