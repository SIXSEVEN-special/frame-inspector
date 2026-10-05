#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/utils/file.hpp>
#include <vector>
#include <string>

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