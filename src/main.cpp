#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/utils/file.hpp>
#include <vector>
#include <string>
#include <algorithm>

using namespace geode::prelude;

struct Click { int down; int up; };

struct State {
    int frame = 0, clicks = 0;
    int downFrame = -1, lastDown = -1;
    int lastGap = -1, minGap = -1, lastHold = -1, minHold = -1;
    std::vector<Click> log;
    void reset() { *this = State(); }
};
static State S;

static std::string bar(int n, int mx) {
    int len = mx > 0 ? (n * 20) / mx : 0;
    if (n > 0 && len == 0) len = 1;
    return std::string(len, '#');
}

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
    auto mxv = [](const std::vector<int>& v) { return v.empty() ? -1 : *std::max_element(v.begin(), v.end()); };

    double secs = S.frame / 240.0;
    double cps = secs > 0 ? S.clicks / secs : 0;

    const char* names[] = {"1f", "2f", "3-4f", "5-8f", "9-16f", "17+f"};
    int b[6] = {0};
    for (int g : gaps) {
        int i = g <= 1 ? 0 : g == 2 ? 1 : g <= 4 ? 2 : g <= 8 ? 3 : g <= 16 ? 4 : 5;
        b[i]++;
    }
    int mb = *std::max_element(b, b + 6);

    std::string out = fmt::format(
        "Frames: {} (~{:.2f}s @240)\nClicks: {}   CPS: {:.2f}\n\n"
        "Gap  min {} / avg {:.1f} / max {}\n"
        "Hold min {} / avg {:.1f} / max {}\n\n"
        "Gap windows:\n",
        S.frame, secs, S.clicks, cps,
        mn(gaps), avg(gaps), mxv(gaps),
        mn(holds), avg(holds), mxv(holds));

    for (int i = 0; i < 6; i++)
        out += fmt::format("{:>6} {:>4} {}\n", names[i], b[i], bar(b[i], mb));

    return out;
}

class $modify(InspectGJBGL, GJBaseGameLayer) {
    void processCommands(float dt) {
        GJBaseGameLayer::processCommands(dt);
        if (PlayLayer::get()) S.frame++;
    }
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (!PlayLayer::get() || button != 1 || !isPlayer1) return;
        if (down) {
            if (S.lastDown >= 0) {
                S.lastGap = S.frame - S.lastDown;
                if (S.minGap < 0 || S.lastGap < S.minGap) S.minGap = S.lastGap;
            }
            S.lastDown = S.downFrame = S.frame;
            S.clicks++;
            S.log.push_back({S.frame, -1});
        } else if (S.downFrame >= 0 && !S.log.empty()) {
            S.log.back().up = S.frame;
            S.lastHold = S.frame - S.downFrame;
            if (S.minHold < 0 || S.lastHold < S.minHold) S.minHold = S.lastHold;
            S.downFrame = -1;
        }
    }
};

class $modify(InspectPL, PlayLayer) {
    struct Fields { CCLabelBMFont* label = nullptr; };

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        S.reset();
        auto l = CCLabelBMFont::create("", "chatFont.fnt");
        l->setAnchorPoint({0.f, 1.f});
        l->setScale(0.5f);
        l->setOpacity(190);
        l->setAlignment(kCCTextAlignmentLeft);
        auto win = CCDirector::get()->getWinSize();
        l->setPosition({6.f, win.height - 6.f});
        m_uiLayer->addChild(l, 100);
        m_fields->label = l;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto l = m_fields->label;
        if (!l) return;
        bool show = Mod::get()->getSettingValue<bool>("show-overlay");
        l->setVisible(show);
        if (!show) return;
        l->setString(fmt::format(
            "Frame: {}\nClicks: {}\nLast gap: {}  Min gap: {}\nLast hold: {}  Min hold: {}",
            S.frame, S.clicks, S.lastGap, S.minGap, S.lastHold, S.minHold).c_str());
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
        auto path = Mod::get()->getSaveDir() / "clicks.csv";
        auto _ = utils::file::writeString(path, out);
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