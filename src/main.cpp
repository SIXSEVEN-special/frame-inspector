#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/utils/file.hpp>
#include <vector>
#include <string>
#include <array>
#include <algorithm>

using namespace geode::prelude;

// rows are displayed top to bottom: 17+, 13+, 11+, 9+, 7+, 5+, 4, 3, 2, 1
static const char* NAMES[10]  = {"17+", "13+", "11+", "9+", "7+", "5+", "4", "3", "2", "1"};
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
    if (S.log.empty()) return "No clicks recorded this attempt yet.";
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
    double secs = S.frame / 240.0;
    std::string out = fmt::format(
        "Frames: {} (~{:.2f}s)\nClicks: {}   Max CPS: {}\n\n"
        "Gap  min {} / avg {:.1f} / max {}\nHold min {} / avg {:.1f} / max {}\n\nWindows:\n",
        S.frame, secs, S.clicks, S.maxCps,
        mn(gaps), avg(gaps), mx(gaps), mn(holds), avg(holds), mx(holds));
    for (int i = 0; i < 10; i++) out += fmt::format("{}: {}\n", NAMES[i], S.rows[i]);
    return out;
}

class $modify(InspectGJBGL, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (PlayLayer::get() && !isHalfTick) S.frame++;
    }
    void handleButton(bool down, int button, bool isPlayer1) {
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
    };

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
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
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        S.reset();
    }

    void onQuit() {
        std::string out = "down_frame,up_frame,hold,gap_from_prev\n";
        int prev = -1;
        for (auto& c : S.log) {
            out += fmt::format("{},{},{},{}\n", c.down, c.up,
                c.up >= 0 ? c.up - c.down : -1, prev >= 0 ? c.down - prev : -1);
            prev = c.down;
        }
        auto _ = utils::file::writeString(Mod::get()->getSaveDir() / "clicks.csv", out);
        PlayLayer::onQuit();
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