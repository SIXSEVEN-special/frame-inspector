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
static int injFrame = 0;

static bool playbackOn() { return hasMacro && Mod::get()->getSettingValue<bool>("playback"); }
static int fps() { return (hasMacro && M.framerate > 0) ? (int)M.framerate : 240; }

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

static void seekMacro(uint64_t frame) {
    idx = std::lower_bound(M.inputs.begin(), M.inputs.end(), frame,
        [](auto const& in, uint64_t f) { return in.frame < f; }) - M.inputs.begin();
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
        int n = 0, w = fps();
        for (int i = (int)log.size() - 1; i >= 0 && log[i].down > frame - w; i--) n++;
        return n;
    }
};
static State S;

// ---------- rings ----------
struct Ring { CCDrawNode* node; CCLabelBMFont* label; int born; int row; };
static std::vector<Ring> rings;

static void spawnRing(GJBaseGameLayer* gl, int gap, int row) {
    if (!Mod::get()->getSettingValue<bool>("show-circles")) return;
    if (!gl->m_player1 || !gl->m_objectLayer) return;
    auto node = CCDrawNode::create();
    node->setPosition(gl->m_player1->getPosition());
    auto lbl = CCLabelBMFont::create(gap >= 0 ? std::to_string(gap).c_str() : "", "bigFont.fnt");
    lbl->setScale(0.5f);
    lbl->setColor(COLS[row]);
    lbl->setAnchorPoint({1.f, 0.5f});
    lbl->setPosition({-26.f, 0.f});
    node->addChild(lbl);
    gl->m_objectLayer->addChild(node, 1000);
    rings.push_back({node, lbl, S.frame, row});
}

static void clearRings(bool removeNodes) {
    if (removeNodes) for (auto& r : rings) r.node->removeFromParent();
    rings.clear();
}

static void registerDown(int frame, GJBaseGameLayer* gl) {
    int gap = S.lastDown >= 0 ? frame - S.lastDown : -1;
    int row = gap >= 0 ? bucketRow(gap) : 0;
    if (gap >= 0) {
        S.lastGap = gap;
        if (S.minGap < 0 || gap < S.minGap) S.minGap = gap;
        S.rows[row]++;
        if (Mod::get()->getSettingValue<bool>("click-sound") &&
            gap <= Mod::get()->getSettingValue<int64_t>("sound-max-gap")) {
            FMODAudioEngine::sharedEngine()->playEffect("counter003.ogg", 1.f, 0.f, 1.f);
        }
    }
    S.lastDown = S.downFrame = frame;
    S.clicks++;
    S.log.push_back({frame, -1});
    S.maxCps = std::max(S.maxCps, S.curCps());
    spawnRing(gl, gap, row);
}

static void registerUp(int frame) {
    if (S.downFrame < 0 || S.log.empty()) return;
    S.log.back().up = frame;
    S.lastHold = frame - S.downFrame;
    if (S.minHold < 0 || S.lastHold < S.minHold) S.minHold = S.lastHold;
    S.downFrame = -1;
}

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
        S.frame, S.frame / (double)fps(), S.clicks, S.maxCps,
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
                    injFrame = (int)in.frame;
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

        // with a macro, stats use the macro's own frame numbers
        int f = injecting ? injFrame : S.frame;
        if (down) registerDown(f, this);
        else registerUp(f);
    }
};

class $modify(InspectPL, PlayLayer) {
    struct Fields {
        std::array<CCLabelBMFont*, 10> names{};
        std::array<CCLabelBMFont*, 10> nums{};
        CCLabelBMFont* cps = nullptr;
    };

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        loadMacro();
        clearRings(false);
        idx = 0;
        S.reset();

        auto win = CCDirector::get()->getWinSize();
        float top = win.height - (float)Mod::get()->getSettingValue<int64_t>("counter-y");
        for (int i = 0; i < 10; i++) {
            float y = top - i * 17.f;
            auto n = CCLabelBMFont::create("", "bigFont.fnt");
            n->setAnchorPoint({0.f, 1.f});
            n->setScale(0.45f);
            n->setColor(COLS[i]);
            n->setPosition({8.f, y});
            n->setString(fmt::format("{}:", NAMES[i]).c_str());
            m_uiLayer->addChild(n, 100);
            m_fields->names[i] = n;

            auto v = CCLabelBMFont::create("0", "bigFont.fnt");
            v->setAnchorPoint({0.f, 1.f});
            v->setScale(0.45f);
            v->setColor(COLS[i]);
            v->setPosition({70.f, y});
            m_uiLayer->addChild(v, 100);
            m_fields->nums[i] = v;
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
            auto n = m_fields->names[i];
            auto v = m_fields->nums[i];
            if (!n || !v) continue;
            n->setVisible(show);
            v->setVisible(show);
            if (show) v->setString(std::to_string(S.rows[i]).c_str());
        }
        if (auto c = m_fields->cps) {
            c->setVisible(show);
            if (show) c->setString(fmt::format("{}/{}/{} CPS", S.curCps(), S.maxCps, S.clicks).c_str());
        }

        // click rings: fade out and expand a little
        for (size_t i = 0; i < rings.size();) {
            auto& r = rings[i];
            float age = std::max(0, S.frame - r.born) / (float)fps();
            float a = 1.f - age / 0.8f;
            if (a <= 0.f) {
                r.node->removeFromParent();
                rings.erase(rings.begin() + i);
                continue;
            }
            auto c = col4(r.row, a);
            r.node->clear();
            r.node->drawCircle({0.f, 0.f}, 18.f + age * 14.f, ccColor4F{c.r, c.g, c.b, 0.15f * a}, 2.5f, c, 36);
            r.label->setOpacity((GLubyte)(a * 255.f));
            i++;
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        clearRings(true);
        S.reset();
        seekMacro((uint64_t)m_gameState.m_currentProgress);
    }

    void onQuit() {
        clearRings(false);
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