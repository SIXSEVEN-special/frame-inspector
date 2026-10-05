#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include "gdr/gdr.hpp"
#include <vector>
#include <string>
#include <array>
#include <algorithm>

using namespace geode::prelude;

// rows top to bottom: 17+, 13+, 11+, 9+, 7+, 5+, 4, 3, 2, 1
static const char* NAMES[10] = {"17+", "13+", "11+", "9+", "7+", "5+", "4", "3", "2", "1"};
static const ccColor3B COLS[10] = {
    {255,105,215}, {190,120,255}, {110,120,255}, {70,150,255}, {60,200,255},
    {100,230,120}, {255,255,255}, {255,235,110}, {255,160,60}, {255,90,90}
};

static int bucketRow(int gap) {
    if (gap <= 1) return 9;
    if (gap == 2) return 8;
    if (gap == 3) return 7;
    if (gap == 4) return 6;
    if (gap <= 6) return 5;
    if (gap <= 8) return 4;
    if (gap <= 10) return 3;
    if (gap <= 12) return 2;
    if (gap <= 16) return 1;
    return 0;
}

static ccColor4F col4(int row, float a = 1.f) {
    auto c = COLS[row];
    return {c.r / 255.f, c.g / 255.f, c.b / 255.f, a};
}

// ---------- macro ----------
using Macro = gdr::Replay<>;
static Macro M;
static bool hasMacro = false;
static size_t idx = 0;
static bool injecting = false;

static bool playbackOn() { return hasMacro && Mod::get()->getSettingValue<bool>("playback"); }

static void loadMacro() {
    hasMacro = false;
    M = Macro();
    auto p = Mod::get()->getSettingValue<std::filesystem::path>("macro-file");
    std::error_code ec;
    if (p.empty() || !std::filesystem::is_regular_file(p, ec)) return;
    auto r = Macro::importData(p);
    if (r.isErr()) {
        Notification::create("Macro load failed: " + r.unwrapErr(), NotificationIcon::Error)->show();
        return;
    }
    M = r.unwrap();
    M.sortInputs();
    hasMacro = true;
}

// ---------- stats ----------
struct Click { int down; int up; };

struct State {
    int frame = 0, clicks = 0, maxCps = 0;
    int downFrame = -1, lastDown = -1;
    int lastGap = -1, minGap = -1, lastHold = -1, minHold = -1;
    int rows[10] = {0};
    std::vector<Click> log;
    void reset() { *this = State(); }
    int curCps() const {
        int n = 0;
        for (int i = (int)log.size() - 1; i >= 0 && log[i].down > frame - 240; i--) n++;
        return n;
    }
};
static State S;

static std::string buildOverview() {
    std::string head = hasMacro
        ? fmt::format("Macro: {} by {} ({} inputs)\n\n", M.levelInfo.name, M.author, M.inputs.size())
        : std::string("No macro loaded\n\n");
    if (S.log.empty()) return head + "No clicks yet this attempt.";
    std::vector<int> gaps, holds;
    for (size_t i = 0; i < S.log.size(); i++) {
        if (i > 0) gaps.push_back(S.log[i].down - S.log[i - 1].down);
        if (S.log[i].up >= 0) holds.push_back(S.log[i].up - S.log[i].down);
    }
    auto avg = [](const std::vector<int>& v) {
        if (v.empty()) return 0.0;
        double s = 0; for (int x : v) s += x; return s / v.size();
    };
    auto mn = [](const std::vector<int>& v) { return v.empty() ? -1 : *std::min_element(v.begin(), v.end()); };
    auto mx = [](const std::vector<int>& v) { return v.empty() ? -1 : *std::max_element(v.begin(), v.end()); };
    std::string out = head + fmt::format(
        "Frames: {} (~{:.2f}s)\nClicks: {}   Max CPS: {}\n\n"
        "Gap  min {} / avg {:.1f} / max {}\nHold min {} / avg {:.1f} / max {}\n\nWindows:\n",
        S.frame, S.frame / 240.0, S.clicks, S.maxCps,
        mn(gaps), avg(gaps), mx(gaps), mn(holds), avg(holds), mx(holds));
    for (int i = 0; i < 10; i++) out += fmt::format("{}: {}\n", NAMES[i], S.rows[i]);
    return out;
}

// ---------- hooks ----------
class $modify(InspectGJBGL, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (PlayLayer::get() && !isHalfTick) {
            S.frame = (int)m_gameState.m_currentProgress;
            if (playbackOn()) {
                while (idx < M.inputs.size() && M.inputs[idx].frame <= (uint64_t)S.frame) {
                    auto& in = M.inputs[idx++];
                    injecting = true;
                    this->handleButton(in.down, in.button, !in.player2);
                    injecting = false;
                }
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        // ignore real clicks while a macro is playing
        if (!injecting && PlayLayer::get() && playbackOn()) return;

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (!PlayLayer::get() || button != 1 || !isPlayer1) return;

        if (down) {
            if (S.lastDown >= 0) {
                S.lastGap = S.frame - S.lastDown;
                if (S.minGap < 0 || S.lastGap < S.minGap) S.minGap = S.lastGap;
                S.rows[bucketRow(S.lastGap)]++;
                if (Mod::get()->getSettingValue<bool>("click-sound") &&
                    S.lastGap <= Mod::get()->getSettingValue<int64_t>("sound-max-gap")) {
                    FMODAudioEngine::sharedEngine()->playEffect("counter003.ogg", 1.f, 0.f, 1.f);
                }
            }
            S.lastDown = S.downFrame = S.frame;
            S.clicks++;
            S.log.push_back({S.frame, -1});
            S.maxCps = std::max(S.maxCps, S.curCps());
        } else if (S.downFrame >= 0 && !S.log.empty()) {
            S.log.back().up = S.frame;
            S.lastHold = S.frame - S.downFrame;
            if (S.minHold < 0 || S.lastHold < S.minHold) S.minHold = S.lastHold;
            S.downFrame = -1;
        }
    }
};

class $modify(InspectPL, PlayLayer) {
    struct Fields {
        std::array<CCLabelBMFont*, 10> rows{};
        CCLabelBMFont* cps = nullptr;
        CCDrawNode* tl = nullptr;
    };

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        loadMacro();
        idx = 0;
        S.reset();

        auto win = CCDirector::get()->getWinSize();
        for (int i = 0; i < 10; i++) {
            auto l = CCLabelBMFont::create("", "bigFont.fnt");
            l->setAnchorPoint({0.f, 1.f});
            l->setScale(0.42f);
            l->setColor(COLS[i]);
            l->setPosition({8.f, win.height - 8.f - i * 17.f});
            m_uiLayer->addChild(l, 100);
            m_fields->rows[i] = l;
        }
        auto c = CCLabelBMFont::create("", "bigFont.fnt");
        c->setAnchorPoint({1.f, 1.f});
        c->setScale(0.4f);
        c->setOpacity(170);
        c->setPosition({win.width - 8.f, win.height - 8.f});
        m_uiLayer->addChild(c, 100);
        m_fields->cps = c;

        auto d = CCDrawNode::create();
        m_uiLayer->addChild(d, 99);
        m_fields->tl = d;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        bool show = Mod::get()->getSettingValue<bool>("show-overlay");
        for (int i = 0; i < 10; i++) {
            auto l = m_fields->rows[i];
            if (!l) continue;
            l->setVisible(show);
            if (show) l->setString(fmt::format("{}: {}", NAMES[i], S.rows[i]).c_str());
        }
        if (auto c = m_fields->cps) {
            c->setVisible(show);
            if (show) c->setString(fmt::format("{}/{}/{} CPS", S.curCps(), S.maxCps, S.clicks).c_str());
        }

        // click circle timeline: last 120 frames, newest on the right
        auto d = m_fields->tl;
        if (!d) return;
        d->clear();
        if (!Mod::get()->getSettingValue<bool>("show-circles")) return;

        auto win = CCDirector::get()->getWinSize();
        const int WIN = 120;
        const float w = 300.f, y = 16.f;
        const float x0 = win.width / 2.f - w / 2.f;
        auto xAt = [&](int age) { return x0 + w - std::min(age, WIN) * (w / WIN); };

        d->drawSegment({x0, y}, {x0 + w, y}, 0.6f, {1.f, 1.f, 1.f, 0.25f});
        for (size_t i = S.log.size(); i-- > 0;) {
            auto& c = S.log[i];
            int end = c.up >= 0 ? c.up : S.frame;
            if (S.frame - end > WIN) break;
            int gap = i > 0 ? c.down - S.log[i - 1].down : 99;
            int row = bucketRow(gap);
            d->drawSegment({xAt(S.frame - c.down), y}, {xAt(S.frame - end), y}, 2.f, col4(row, 0.55f));
            if (S.frame - c.down <= WIN)
                d->drawDot({xAt(S.frame - c.down), y}, 5.f, col4(row));
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        idx = 0;
        S.reset();
    }
};

class $modify(InspectPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto menu = this->getChildByID("right-button-menu");
        if (!menu) return;
        auto spr = ButtonSprite::create("Macro");
        spr->setScale(0.6f);
        auto btn = CCMenuItemExt::createSpriteExtra(spr, [](CCObject*) {
            FLAlertLayer::create(nullptr, "Macro Overview", buildOverview(), "OK", nullptr, 380.f)->show();
        });
        menu->addChild(btn);
        menu->updateLayout();
    }
};