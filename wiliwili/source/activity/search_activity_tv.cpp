//
// Created by fang on 2023/4/20.
//
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <borealis/core/thread.hpp>
#include <borealis/core/touch/tap_gesture.hpp>

#include "activity/search_activity_tv.hpp"
#include "fragment/search_hots.hpp"
#include "fragment/search_history.hpp"
#include "view/recycling_grid.hpp"
#include "view/hots_card.hpp"
#include "view/svg_image.hpp"
#include "utils/activity_helper.hpp"
#include "bilibili.h"
#include "bilibili/result/search_result.h"
#include "analytics.h"

using namespace brls::literals;

typedef brls::Event<char> KeyboardEvent;
typedef std::vector<char> KeyboardData;

static void appendUtf8(std::string& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7F) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

static std::string wstringToUtf8(const std::wstring& value) {
    std::string output;
    for (size_t i = 0; i < value.size(); ++i) {
        std::uint32_t codePoint = static_cast<std::uint32_t>(value[i]);
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
            if (i + 1 < value.size()) {
                const std::uint32_t low = static_cast<std::uint32_t>(value[i + 1]);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                    ++i;
                } else {
                    codePoint = 0xFFFD;
                }
            } else {
                codePoint = 0xFFFD;
            }
        } else if ((codePoint >= 0xDC00 && codePoint <= 0xDFFF) || codePoint > 0x10FFFF) {
            codePoint = 0xFFFD;
        }
        appendUtf8(output, codePoint);
    }
    return output;
}

static void appendWchar(std::wstring& output, std::uint32_t codePoint) {
    if constexpr (sizeof(wchar_t) == 2) {
        if (codePoint <= 0xFFFF) {
            output.push_back(static_cast<wchar_t>(codePoint));
        } else {
            output.push_back(static_cast<wchar_t>(0xD800 + ((codePoint - 0x10000) >> 10)));
            output.push_back(static_cast<wchar_t>(0xDC00 + ((codePoint - 0x10000) & 0x3FF)));
        }
    } else {
        output.push_back(static_cast<wchar_t>(codePoint));
    }
}

static std::wstring utf8ToWstring(const std::string& value) {
    std::wstring output;
    for (size_t i = 0; i < value.size();) {
        const auto byte           = [&value](size_t index) { return static_cast<unsigned char>(value[index]); };
        const unsigned char first = byte(i);
        std::uint32_t codePoint   = 0xFFFD;
        size_t consumed           = 1;
        if (first <= 0x7F) {
            codePoint = first;
        } else if (first >= 0xC2 && first <= 0xDF && i + 1 < value.size() && (byte(i + 1) & 0xC0) == 0x80) {
            codePoint = ((first & 0x1F) << 6) | (byte(i + 1) & 0x3F);
            consumed  = 2;
        } else if (first >= 0xE0 && first <= 0xEF && i + 2 < value.size() && (byte(i + 1) & 0xC0) == 0x80 &&
                   (byte(i + 2) & 0xC0) == 0x80) {
            codePoint = ((first & 0x0F) << 12) | ((byte(i + 1) & 0x3F) << 6) | (byte(i + 2) & 0x3F);
            if (codePoint < 0x800 || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
                codePoint = 0xFFFD;
            } else {
                consumed = 3;
            }
        } else if (first >= 0xF0 && first <= 0xF4 && i + 3 < value.size() && (byte(i + 1) & 0xC0) == 0x80 &&
                   (byte(i + 2) & 0xC0) == 0x80 && (byte(i + 3) & 0xC0) == 0x80) {
            codePoint = ((first & 0x07) << 18) | ((byte(i + 1) & 0x3F) << 12) | ((byte(i + 2) & 0x3F) << 6) |
                        (byte(i + 3) & 0x3F);
            if (codePoint < 0x10000 || codePoint > 0x10FFFF) {
                codePoint = 0xFFFD;
            } else {
                consumed = 4;
            }
        }
        appendWchar(output, codePoint);
        i += consumed;
    }
    return output;
}

class KeyboardButton : public RecyclingGridItem {
public:
    KeyboardButton() {
        key = new brls::Label();
        key->setVerticalAlign(brls::VerticalAlign::CENTER);
        key->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        this->setBackgroundColor(brls::Application::getTheme().getColor("color/grey_2"));
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setJustifyContent(brls::JustifyContent::CENTER);
        this->setCornerRadius(4);
        this->addView(key);
    }

    void setValue(const std::string& value) { key->setText(value); }

    void prepareForReuse() override {}

    void cacheForReuse() override {}

    static RecyclingGridItem* create() { return new KeyboardButton(); }

private:
    brls::Label* key = nullptr;
};

class DataSourceKeyboard : public RecyclingGridDataSource {
public:
    explicit DataSourceKeyboard(KeyboardData result) : list(std::move(result)) {}
    RecyclingGridItem* cellForRow(RecyclingGrid* recycler, size_t index) override {
        auto* item = (KeyboardButton*)recycler->dequeueReusableCell("Cell");
        item->setValue(std::string{this->list[index]});
        return item;
    }

    size_t getItemCount() override { return list.size(); }

    void onItemSelected(RecyclingGrid* recycler, size_t index) override { this->keyboardEvent.fire(list[index]); }

    void appendData(const KeyboardData& data) { this->list.insert(this->list.end(), data.begin(), data.end()); }

    KeyboardEvent* getKeyboardEvent() { return &this->keyboardEvent; }

    void clearData() override { this->list.clear(); }

private:
    KeyboardData list;
    KeyboardEvent keyboardEvent;
};

class DataSourceSuggest : public RecyclingGridDataSource {
public:
    DataSourceSuggest(bilibili::SearchSuggestList result, UpdateSearchEvent* u)
        : list(std::move(result)), updateSearchEvent(u) {}

    RecyclingGridItem* cellForRow(RecyclingGrid* recycler, size_t index) override {
        auto* item = (RecyclingGridItemHotsCard*)recycler->dequeueReusableCell("Cell");
        item->setCard(std::to_string(index + 1), list.tag[index].value, "");
        return item;
    }

    void onItemSelected(RecyclingGrid* recycler, size_t index) override {
        if (this->updateSearchEvent) {
            this->updateSearchEvent->fire(list.tag[index].value);
        }
    }

    size_t getItemCount() override { return list.tag.size(); }

    void clearData() override { this->list.tag.clear(); }

private:
    bilibili::SearchSuggestList list;
    UpdateSearchEvent* updateSearchEvent = nullptr;
};

TVSearchActivity::TVSearchActivity() {
    brls::Logger::debug("TVSearchActivity: create");
    GA("open_tv_search")
}

void TVSearchActivity::requestSearchSuggest() {
    ASYNC_RETAIN
    BILI::get_search_suggest_tv(
        getCurrentSearch(),
        [ASYNC_TOKEN](const bilibili::SearchSuggestList& result) {
            brls::Threading::sync([ASYNC_TOKEN, result]() {
                ASYNC_RELEASE
                this->hotsHeaderLabel->setText("wiliwili/search/tv/suggest"_i18n);
                this->searchHots->getRecyclingGrid()->setDataSource(new DataSourceSuggest(result, &updateSearchEvent));
            });
        },
        [ASYNC_TOKEN](BILI_ERR) {
            brls::Logger::error("requestSearchSuggest: {}", error);
            brls::sync([ASYNC_TOKEN, error]() {
                ASYNC_RELEASE
                this->searchHots->getRecyclingGrid()->setError(error);
            });
        });
}

void TVSearchActivity::updateInputLabel() {
    std::string value = getCurrentSearch();
    if (value.empty()) {
        this->inputLabel->setText("wiliwili/search/tv/hint"_i18n);
        this->inputLabel->setTextColor(brls::Application::getTheme().getColor("font/grey"));
        this->searchHots->getRecyclingGrid()->showSkeleton();
        this->searchHots->requestSearch();
        this->hotsHeaderLabel->setText("wiliwili/search/tv/hots"_i18n);
    } else {
        this->inputLabel->setText(getCurrentSearch());
        this->inputLabel->setTextColor(brls::Application::getTheme().getColor("brls/text"));
        this->requestSearchSuggest();
    }
}

void TVSearchActivity::onContentAvailable() {
    brls::Logger::debug("TVSearchActivity: onContentAvailable");

    if (brls::Application::getThemeVariant() == brls::ThemeVariant::LIGHT) {
        searchSVG->setImageFromSVGRes("svg/header-search-dark.svg");
    } else {
        searchSVG->setImageFromSVGRes("svg/header-search.svg");
    }

    inputLabel->setText("wiliwili/search/tv/hint"_i18n);
    inputLabel->setTextColor(brls::Application::getTheme().getColor("font/grey"));

    inputLabel->getParent()->registerClickAction([this](...) {
        brls::Application::getImeManager()->openForText(
            [this](const std::string& text) {
                this->setCurrentSearch(text);
                this->updateInputLabel();
            },
            "wiliwili/search/tv/hint"_i18n, "", 32, getCurrentSearch(), 0);
        inputLabel->getParent()->setCustomNavigationRoute(brls::FocusDirection::DOWN, searchLabel);
        return true;
    });
    inputLabel->getParent()->addGestureRecognizer(new brls::TapGestureRecognizer(this->inputLabel->getParent()));

    KeyboardData keyboard;
    for (int i = 'A'; i <= 'Z'; i++) {
        keyboard.emplace_back(i);
    }
    for (int i = '1'; i <= '9'; i++) {
        keyboard.emplace_back(i);
    }
    keyboard.emplace_back('0');
    auto* ds = new DataSourceKeyboard(keyboard);
    ds->getKeyboardEvent()->subscribe([this](char key) {
        this->currentSearch += key;
        this->updateInputLabel();
    });
    recyclingGrid->registerCell("Cell", []() { return KeyboardButton::create(); });
    recyclingGrid->setDataSource(ds);

    clearLabel->registerClickAction([this](...) {
        this->currentSearch.clear();
        this->updateInputLabel();
        inputLabel->getParent()->setCustomNavigationRoute(brls::FocusDirection::DOWN, clearLabel);
        return true;
    });
    clearLabel->addGestureRecognizer(new brls::TapGestureRecognizer(clearLabel));

    deleteLabel->registerClickAction([this](...) {
        if (currentSearch.empty()) {
            return true;
        }
        currentSearch.erase(currentSearch.size() - 1, 1);
        this->updateInputLabel();
        inputLabel->getParent()->setCustomNavigationRoute(brls::FocusDirection::DOWN, deleteLabel);
        return true;
    });
    deleteLabel->addGestureRecognizer(new brls::TapGestureRecognizer(deleteLabel));

    searchLabel->registerClickAction([this](...) {
        this->search(getCurrentSearch());
        inputLabel->getParent()->setCustomNavigationRoute(brls::FocusDirection::DOWN, searchLabel);
        return true;
    });
    searchLabel->addGestureRecognizer(new brls::TapGestureRecognizer(searchLabel));

    updateSearchEvent.subscribe([this](const std::string& value) {
        brls::Application::giveFocus(this->inputLabel->getParent());
        this->setCurrentSearch(value);
        this->inputLabel->setText(value);
        this->inputLabel->setTextColor(brls::Application::getTheme().getColor("brls/text"));
        this->search(value);
    });

    searchHots->setSearchCallback(&updateSearchEvent);
    searchHistory->setSearchCallback(&updateSearchEvent);
    searchHistory->requestHistory();

    // 强制设置搜索历史的 TabBar 为输入栏
    // 在清空历史时，会尝试将焦点切换到对应的 TabBar，这时在 TV 搜索页就能刚好将焦点切换到输入栏
    searchHistory->setTabBar(reinterpret_cast<AutoSidebarItem*>(inputLabel.getView()->getParent()));
}

TVSearchActivity::~TVSearchActivity() { brls::Logger::debug("TVSearchActivity: delete"); }

std::string TVSearchActivity::getCurrentSearch() { return wstringToUtf8(currentSearch); }

void TVSearchActivity::setCurrentSearch(const std::string& value) { currentSearch = utf8ToWstring(value); }

void TVSearchActivity::search(const std::string& key) {
    if (key.empty()) return;
    Intent::openSearch(key);
}

void TVSearchActivity::onResume() { this->searchHistory->requestHistory(); }
