//
// Created by fang on 2022/7/16.
//

#pragma once

#include <cpr/cpr.h>
#include <atomic>
#include <ctime>
#include <mutex>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <borealis/views/image.hpp>

/**
 * 图片加载请求，每个请求对应一个ImageHelper的实例
 */
class ImageHelper {
    typedef std::list<std::shared_ptr<ImageHelper>> Pool;

public:
    ImageHelper(brls::Image* view);

    virtual ~ImageHelper();

    /**
     * 取消图片请求
     */
    void cancel();

    void setImageView(brls::Image* view);

    brls::Image* getImageView();

    /**
     * 设置要加载内容的图片组件。此函数需要工作在主线程。
     */
    static std::shared_ptr<ImageHelper> with(brls::Image* view);

    /**
     * 加载网络图片。此函数需要工作在主线程。
     */
    void load(const std::string& url);

    /**
     * 取消请求，并清空图片。此函数需要工作在主线程。
     */
    static void clear(brls::Image* view);

    static void setRequestThreads(size_t num);

    /**
     * Hold back new network image requests while the user is scrolling fast.
     * The owner token lets nested or side-by-side grids defer independently;
     * loads are resumed (and deferred views fetched) when the last owner
     * releases the gate. Must be called from the main thread.
     */
    static void setLoadDeferSource(const void* owner, bool defer);

    static bool loadsDeferred();

    static std::string parseGifImageUrl(const std::string& url, const std::string& ext);

    static std::string parseNoteImageUrl(const std::string& url, size_t w, size_t h);

    /// 图片请求后缀，用来控制图片大小
#ifdef USE_WEBP
#define IMAGE_EXT ".webp"
#else
#define IMAGE_EXT ".jpg"
#endif

#ifdef __PSV__
#ifdef BOREALIS_USE_GXM
    inline static const std::string h_ext           = "@256w_144h_1c" IMAGE_EXT;
    inline static const std::string v_ext           = "@190w_256h_1c" IMAGE_EXT;
    inline static const std::string face_ext        = "@64w_64h_1c_1s" IMAGE_EXT;
    inline static const std::string face_large_ext  = "@128w_128h_1c_1s" IMAGE_EXT;
    inline static const std::string emoji_size1_ext = "@32w_32h" IMAGE_EXT;
    inline static const std::string emoji_size2_ext = "@64w_64h" IMAGE_EXT;
    inline static const std::string note_ext        = "@64w_64h_85q_!note-comment-multiple" IMAGE_EXT;
    inline static const std::string note_raw_ext    = "@512h" IMAGE_EXT;
    static constexpr float note_small               = 1.0f;
#else
    inline static const std::string h_ext           = "@224w_126h_1c" IMAGE_EXT;
    inline static const std::string v_ext           = "@156w_210h_1c" IMAGE_EXT;
    inline static const std::string face_ext        = "@48w_48h_1c_1s" IMAGE_EXT;
    inline static const std::string face_large_ext  = "@60w_60h_1c_1s" IMAGE_EXT;
    inline static const std::string emoji_size1_ext = "@24w_24h" IMAGE_EXT;
    inline static const std::string emoji_size2_ext = "@36w_36h" IMAGE_EXT;
    inline static const std::string note_ext        = "@180w_180h_85q_!note-comment-multiple" IMAGE_EXT;
    inline static const std::string note_raw_ext    = "@256h" IMAGE_EXT;
    static constexpr float note_small               = 0.5f;
#endif
#else
    inline static const std::string h_ext           = "@672w_378h_1c" IMAGE_EXT;
    inline static const std::string v_ext           = "@312w_420h_1c" IMAGE_EXT;
    inline static const std::string face_ext        = "@96w_96h_1c_1s" IMAGE_EXT;
    inline static const std::string face_large_ext  = "@160w_160h_1c_1s" IMAGE_EXT;
    inline static const std::string emoji_size1_ext = "@48w_48h" IMAGE_EXT;
    inline static const std::string emoji_size2_ext = "@72w_72h" IMAGE_EXT;
    inline static const std::string note_ext        = "@540w_540h_85q_!note-comment-multiple" IMAGE_EXT;
    inline static const std::string note_raw_ext    = "@!web-comment-note" IMAGE_EXT;
    static constexpr float note_small               = 5.0f;
#endif
    inline static const std::string note_custom_ext = "@{}w_{}h_85q_!note-comment-multiple" IMAGE_EXT;

    /// 图片请求线程数
    inline static size_t REQUEST_THREADS = 1;

protected:
    virtual void requestImage(unsigned generation);
    void handleImageResponse(cpr::Response response, unsigned generation);

    /**
     * 图片请求结束时调用
     */
    void clean();

    /// Clean only if `generation` still owns this helper; a newer load keeps it.
    void cleanIfCurrent(unsigned generation);

    /// Bind a texture to the view, releasing the view's previous cache reference.
    void bindImage(int tex);

    /// Body of clean(); caller must hold requestMutex.
    void cleanUnlocked();

private:
    std::atomic<bool> isCancel{};
    brls::Image* imageView;
    std::string imageUrl;
    unsigned decodeAttempts = 0;
    Pool::iterator currentIter;

    /// Bumped for every new load; responses from an older generation are dropped
    /// so a reused cell can never receive a stale cover.
    std::atomic<unsigned> imageGeneration{0};
    /// True while this load is waiting for the scroll gate to open.
    bool deferred = false;

#ifdef BOREALIS_USE_GXM
    int imageFlag;
#endif

    /// 清理图片或取消请求时，用来定位 ImageHelper
    inline static std::unordered_map<brls::Image*, Pool::iterator> requestMap;

    /// 请求队列，可复用其中的 ImageHelper
    inline static Pool requestPool;
    inline static std::mutex requestMutex;

    /// Owners currently requesting that new loads be deferred.
    inline static std::mutex deferMutex;
    inline static std::unordered_set<const void*> deferSources;
    inline static std::atomic<bool> deferLoads{false};

    /// Start network requests for helpers whose load was deferred.
    /// Returns how many deferred loads were restarted.
    static size_t flushDeferred();
};
