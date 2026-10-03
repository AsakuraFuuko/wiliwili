//
// Created by fang on 2022/5/1.
//

#pragma once

#include <nlohmann/json.hpp>
#include <cpr/cpr.h>

#include "bilibili/util/md5.hpp"
#include "bilibili/util/json.hpp"
#include "bilibili/util/wbi.hpp"
#include "utils/number_helper.hpp"
#include <pystring.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <deque>
#include <memory>
#include <future>
#include <netdb.h>
#include <arpa/inet.h>
#include <unordered_map>

#if defined(PS5_NATIVE_APP)
extern "C" void wiliwili_boot_log(const char *message);
#define WILI_HTTP_TRACE(message) wiliwili_boot_log(message)
#else
#define WILI_HTTP_TRACE(message) (void)0
#endif

namespace bilibili {

using Cookies = std::map<std::string, std::string>;

const std::string BILIBILI_APP_KEY    = "aa1e74ee4874176e";
const std::string BILIBILI_APP_SECRET = "54e6a9a31b911cd5fc0daa66ebf94bc4";
const std::string BILIBILI_BUILD      = "1001011000";

using ErrorCallback = std::function<void(const std::string&, int code)>;
#define ERROR_MSG(msg, ...) \
    if (error) error(msg, __VA_ARGS__)
#define HTTP_CALLBACK(data) \
    if (callback) callback(data)

class CurlSharedObject {
public:
    CurlSharedObject() {
        share = curl_share_init();
        curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        /* 只共享 DNS（与 payload 线一致）。**不要**打开 CURL_LOCK_DATA_CONNECT /
         * CURL_LOCK_DATA_SSL_SESSION：真机实测（notes/06 §10.10）并发请求会在共享锁上排队，
         * 一条慢请求能把其它请求阻塞到 ~120 s（http: slow 119758ms dns=0 tcp=18 tls=1134），
         * 表现为"加载卡住、视频出不来"。需要连接复用时，正解是按上游 TODO 做"每线程一个
         * share / 自定义线程池"，而不是全进程共享同一份连接缓存。 */
        curl_share_setopt(share, CURLSHOPT_LOCKFUNC, lock_callback);
        curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, unlock_callback);
        curl_share_setopt(share, CURLSHOPT_USERDATA, lock_array);
    }
    ~CurlSharedObject() {
        curl_share_cleanup(share);
    }

    static void lock_callback(CURL *handle, curl_lock_data data, curl_lock_access access, void *userptr) {
        auto *lock_array = (std::recursive_mutex *)userptr;
        lock_array[data].lock();
    }

    static void unlock_callback(CURL *handle, curl_lock_data data, void *userptr) {
        auto *lock_array = (std::recursive_mutex *)userptr;
        lock_array[data].unlock();
    }

    CURLSH* getShare() {
        return share;
    }

private:
    CURLSH* share;
    std::recursive_mutex lock_array[CURL_LOCK_DATA_LAST];
};



class HTTP {
public:
    static inline cpr::Cookies COOKIES = {false};
    static inline cpr::Header HEADERS  = {
        {"User-Agent", "wiliwili"},
        {"Referer", "https://www.bilibili.com/client"},
        {"Origin", "https://www.bilibili.com"},
    };
    static inline int TIMEOUT = 10000;
    static inline int CONNECTION_TIMEOUT = 0;
    static inline int DNS_CACHE_TIMEOUT = 60;
    static inline cpr::Proxies PROXIES;
    static inline cpr::VerifySsl VERIFY;
    static inline std::string PROTOCOL = "https:";
    static inline CurlSharedObject CURL_SHARE;
#ifdef PS5
#if defined(PS5_NATIVE_APP)
    // Installed titles read their read-only payload from the application image.
    static constexpr char CA_BUNDLE[] = "/app0/assets/ca-bundle.crt";
#else
    static constexpr char CA_BUNDLE[] = "/data/homebrew/wiliwili/ca-bundle.crt";
#endif
#endif

    static std::string getEncodedCookie(const cpr::Cookies& cookies);

    static std::shared_ptr<cpr::Session> createSession() {
        auto session = std::make_shared<cpr::Session>();
        CURL* curl = session->GetCurlHolder()->handle;
        curl_easy_setopt(curl, CURLOPT_SHARE, HTTP::CURL_SHARE.getShare());
        curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, HTTP::DNS_CACHE_TIMEOUT);
#ifdef PS5
        curl_easy_setopt(curl, CURLOPT_CAINFO, HTTP::CA_BUNDLE);
#endif
        session->SetTimeout(cpr::Timeout{bilibili::HTTP::TIMEOUT});
        session->SetConnectTimeout(cpr::ConnectTimeout{bilibili::HTTP::CONNECTION_TIMEOUT});
        session->SetHeader(bilibili::HTTP::HEADERS);
        session->SetProxies(bilibili::HTTP::PROXIES);
        session->SetVerifySsl(bilibili::HTTP::VERIFY);
        return session;
    }

    static void _cpr_post(const std::string& url, const cpr::Parameters& parameters = {},
                           const cpr::Payload& payload                               = {},
                           const std::function<void(const cpr::Response&)>& callback = nullptr,
                           const ErrorCallback& error                                = nullptr) {
        auto session = createSession();;
        session->SetUrl(cpr::Url{parseLink(url)});
        session->SetParameters(parameters);
        session->SetPayload(payload);

        /* 不要走 cpr 的 async（multi + 自带线程池）——那条驱动方式在本平台会 fault
         * （见 runAsync 注释）。这里在线程里用阻塞 POST，语义与 GET 一致。 */
        try {
            std::thread([session, callback, error]() {
                cpr::Response response = session->Post();
                WILI_HTTP_TRACE("http: request done");
                dispatchResponse(response, callback, error);
                WILI_HTTP_TRACE("http: callback done");
            }).detach();
        } catch (const std::exception&) {
        }
    }


    /**
     * Run one request on a dedicated thread using curl's easy interface.
     *
     * cpr's callback API drives curl's multi interface from its own pool. That
     * path faults inside the platform runtime for an installed title, so the
     * request is performed with the blocking interface instead and the
     * callback still runs off the calling thread, matching cpr's contract.
     */
    static void runAsync(const std::shared_ptr<cpr::Session>& session,
                         const std::function<void(const cpr::Response&)>& callback) {
        try {
            std::thread([session, callback]() {
                WILI_HTTP_TRACE("http: request start");
                const auto start_at = std::chrono::steady_clock::now();
                CURL* handle = session->GetCurlHolder()->handle;
                cpr::Response response = session->Get();
                WILI_HTTP_TRACE("http: request done");
                /* 慢请求检查点：真机上出现过"请求发出但迟迟不回来"（加载卡住）。
                 * 只报 >2 s 的请求，并带上 curl 的分段耗时，直接指出卡在 DNS/建连/TLS/首字节
                 * 哪一段——mpv 的日志在本平台关着，这类"卡在哪"只能靠这里取证。 */
                const long long elapsed_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_at)
                        .count();
                if (elapsed_ms > 2000) {
                    double dns_t = 0, conn_t = 0, tls_t = 0, first_t = 0;
                    curl_easy_getinfo(handle, CURLINFO_NAMELOOKUP_TIME, &dns_t);
                    curl_easy_getinfo(handle, CURLINFO_CONNECT_TIME, &conn_t);
                    curl_easy_getinfo(handle, CURLINFO_APPCONNECT_TIME, &tls_t);
                    curl_easy_getinfo(handle, CURLINFO_STARTTRANSFER_TIME, &first_t);
                    char line[256];
                    std::snprintf(line, sizeof(line),
                                  "http: slow %lldms dns=%.0f tcp=%.0f tls=%.0f first=%.0f code=%ld err=%d",
                                  elapsed_ms, dns_t * 1000, conn_t * 1000, tls_t * 1000, first_t * 1000,
                                  (long)response.status_code, (int)response.error.code);
                    WILI_HTTP_TRACE(line);
                }
                callback(response);
                WILI_HTTP_TRACE("http: callback done");
            }).detach();
        } catch (const std::exception& failure) {
        }
    }

    /* 响应分发：成功/失败路径与日志（GET 两条路径共用）。 */
    static void dispatchResponse(const cpr::Response& r, const std::function<void(const cpr::Response&)>& callback,
                                 const ErrorCallback& error) {
        {
            char message[96];
            std::snprintf(message, sizeof(message), "http: code=%ld err=%d", (long)r.status_code, (int)r.error.code);
            WILI_HTTP_TRACE(message);
        }
        if (r.error) {
            ERROR_MSG(r.error.message, -1);
            return;
        }
        if (r.status_code != 200) {
            ERROR_MSG("Network error. [Status code: " + std::to_string(r.status_code) + " ]", r.status_code);
            return;
        }
        callback(r);
    }

    static void _cpr_get(const std::string& url, const cpr::Parameters& parameters = {},
                          const std::function<void(const cpr::Response&)>& callback = nullptr,
                          const ErrorCallback& error                                = nullptr) {
        auto session = createSession();;
        session->SetUrl(cpr::Url{parseLink(url)});
        session->SetParameters(parameters);

        runAsync(session, [callback, error](const cpr::Response& r) { dispatchResponse(r, callback, error); });
    }

    template <typename ReturnType>
    static int parseJson(const cpr::Response& r, const std::function<void(ReturnType)>& callback = nullptr,
                          const ErrorCallback& error = nullptr) {
        try {
            WILI_HTTP_TRACE("http: parsing json");
            nlohmann::json res = nlohmann::json::parse(r.text);
            WILI_HTTP_TRACE("http: json parsed");
            int code           = res.at("code").get<int>();
            if (code == 0) {
                if (res.contains("data") && (res.at("data").is_object() || res.at("data").is_array())) {
                    HTTP_CALLBACK(res.at("data").get<ReturnType>());
                    return 0;
                } else if (res.contains("result") && res.at("result").is_object()) {
                    HTTP_CALLBACK(res.at("result").get<ReturnType>());
                    return 0;
                } else {
                    printf("data: %s\n", r.text.c_str());
                    ERROR_MSG("Cannot find data", -1);
                }
            } else if (res.at("message").is_string()) {
                ERROR_MSG(res.at("message").get<std::string>(), code);
            } else {
                ERROR_MSG("Param error", -1);
            }
        } catch (const std::exception& e) {
            ERROR_MSG("Api error. \n" + std::string{e.what()}, 200);
            printf("data: %s\n", r.text.c_str());
            printf("ERROR: %s\n", e.what());
        }
        return 1;
    }

    static void signParameters(cpr::Parameters& parameters);

    template <typename ReturnType>
    static void getResultAsync(const std::string& url,
                               cpr::Parameters parameters                      = {},
                               const std::function<void(ReturnType)>& callback = nullptr,
                               const ErrorCallback& error                      = nullptr,
                               bool needSign                                   = false) {
        if (needSign) {
            signParameters(parameters);
        }
        _cpr_get(
            url,
            parameters,
            [callback, error](const cpr::Response& r) {
                parseJson<ReturnType>(r, callback, error);
            },
            error);
    }

    template <typename ReturnType>
    static void getResultWithWbiAsync(const std::string& url,
                                      cpr::Parameters parameters                      = {},
                                      const std::function<void(ReturnType)>& callback = nullptr,
                                      const ErrorCallback& error                      = nullptr,
                                      bool needSign                                   = false) {
        wbi::updateWbiKeys([url, parameters, callback, error, needSign]() mutable {
            if (needSign) {
                signParameters(parameters);
            }
            wbi::encWbi(parameters);
            _cpr_get(
                url,
                parameters,
                [callback, error](const cpr::Response& r) {
                    parseJson<ReturnType>(r, callback, error);
                },
                error);
        }, error);
    }

    template <typename ReturnType>
    static void postResultAsync(const std::string& url,
                                cpr::Parameters parameters                      = {},
                                const cpr::Payload& payload                     = {},
                                const std::function<void(ReturnType)>& callback = nullptr,
                                const ErrorCallback& error                      = nullptr,
                                bool needSign                                   = false) {
        if (needSign) {
            signParameters(parameters);
        }
        _cpr_post(
            url, parameters, payload,
            [callback, error](const cpr::Response& r) {
                try {
                    nlohmann::json res = nlohmann::json::parse(r.text);
                    const int code     = res.at("code").get<int>();
                    if (code == 0) {
                        if (res.contains("data") && res.at("data").is_object()) {
                            HTTP_CALLBACK(res.at("data").get<ReturnType>());
                        } else if (res.contains("result") && res.at("result").is_object()) {
                            HTTP_CALLBACK(res.at("result").get<ReturnType>());
                        } else {
                            printf("data: %s\n", r.text.c_str());
                            ERROR_MSG("Cannot find data", -1);
                        }
                        return;
                    }
                    ERROR_MSG(res.at("message").get<std::string>(), code);
                } catch (const std::exception& e) {
                    ERROR_MSG(std::string(e.what()), r.status_code);
                    printf("data: %s\n", r.text.c_str());
                    printf("ERROR: %s\n", e.what());
                }
            },
            error);
    }

    static void postResultAsync(const std::string& url,
                                const cpr::Parameters& parameters     = {},
                                const cpr::Payload& payload           = {},
                                const std::function<void()>& callback = nullptr,
                                const ErrorCallback& error            = nullptr) {
        _cpr_post(
            url, parameters, payload,
            [callback, error](const cpr::Response& r) {
                try {
                    nlohmann::json res = nlohmann::json::parse(r.text);
                    const int code     = res.at("code").get<int>();
                    if (code == 0) {
                        if (callback) callback();
                        return;
                    }
                    ERROR_MSG(res.at("message").get<std::string>(), code);
                } catch (const std::exception& e) {
                    ERROR_MSG(std::string(e.what()), r.status_code);
                    printf("data: %s\n", r.text.c_str());
                    printf("ERROR: %s\n", e.what());
                }
            },
            error);
    }
};

}  // namespace bilibili

