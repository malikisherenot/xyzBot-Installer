#include "Installer.hpp"
#include "Sha256.hpp"
#include <Geode/Geode.hpp>
#include <Geode/ui/MDTextArea.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/web.hpp>
#include <fstream>
#if __has_include(<Geode/utils/game.hpp>)
#include <Geode/utils/game.hpp>
#define XYZI_HAS_RESTART 1
#endif

using namespace geode::prelude;

namespace {
constexpr char const* API = "https://api.github.com/repos/malikisherenot/xyzBot/releases?per_page=100";
constexpr char const* MOD_ID = "itzmalikhere.xyzbot";
constexpr float W = 400.f, H = 270.f;

struct Rel {
    std::string tag, date, body, url, digest;
    bool pre = false;
    size_t size = 0;
};

web::WebRequest req(int seconds) {
    auto r = web::WebRequest();
    r.timeout(std::chrono::seconds(seconds));
    r.userAgent(fmt::format("xyzBotInstaller/{}", Mod::get()->getVersion().toVString()));
    return r;
}

bool isHex64(std::string const& s) {
    if (s.size() != 64) return false;
    for (char c : s) if (!std::isxdigit((unsigned char)c)) return false;
    return true;
}

class InstallerPopup : public geode::Popup {
    enum class Page { Loading, Failed, Select, Changelog, Installing, Done };
    Page m_page = Page::Loading;
    std::string m_msg;
    std::vector<Rel> m_rels;
    size_t m_sel = 0;
    CCNode* m_body = nullptr;
    async::TaskHolder<web::WebResponse> m_list, m_dl;

    static std::string installedTag() {
        auto mod = Loader::get()->getInstalledMod(MOD_ID);
        return mod ? mod->getVersion().toVString() : std::string();
    }

    static std::string norm(std::string s) {
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
        return s;
    }

    std::filesystem::path targetPath() const {
        if (auto mod = Loader::get()->getInstalledMod(MOD_ID)) return mod->getPackagePath();
        return Loader::get()->getGeodeDir() / "mods" / (std::string(MOD_ID) + ".geode");
    }

    CCLabelBMFont* label(std::string const& t, float x, float y, float sc, bool left = false, ccColor3B col = {255, 255, 255}) {
        auto l = CCLabelBMFont::create(t.c_str(), "bigFont.fnt");
        l->setScale(sc);
        l->setColor(col);
        l->setAnchorPoint({left ? 0.f : 0.5f, 0.5f});
        l->setPosition({x, y});
        m_body->addChild(l);
        return l;
    }

    CCMenuItemSpriteExtra* button(CCMenu* menu, std::string const& t, float x, float y, int width, std::function<void()> cb, char const* tex = "GJ_button_04.png", float sc = 0.55f) {
        auto spr = ButtonSprite::create(t.c_str(), width, true, "bigFont.fnt", tex, 28.f, sc);
        auto b = CCMenuItemExt::createSpriteExtra(spr, [cb = std::move(cb)](auto) { queueInMainThread([cb] { cb(); }); });
        b->setPosition({x, y});
        menu->addChild(b);
        return b;
    }

    void fail(std::string why) {
        m_msg = std::move(why);
        m_page = Page::Failed;
        rebuild();
    }

    void loadList() {
        m_page = Page::Loading;
        rebuild();
        m_list.spawn(req(20).header("Accept", "application/vnd.github+json").get(API), [this](web::WebResponse r) {
            if (!r.ok()) { fail(r.code() <= 0 ? "Could not reach GitHub" : fmt::format("GitHub answered with error {}", r.code())); return; }
            auto json = r.json().unwrapOrDefault();
            m_rels.clear();
            for (auto const& rel : json) {
                if (rel["draft"].asBool().unwrapOr(false)) continue;
                Rel out;
                out.tag = rel["tag_name"].asString().unwrapOrDefault();
                out.date = rel["published_at"].asString().unwrapOrDefault().substr(0, 10);
                out.body = rel["body"].asString().unwrapOrDefault();
                out.pre = rel["prerelease"].asBool().unwrapOr(false);
                for (auto const& a : rel["assets"]) {
                    auto name = a["name"].asString().unwrapOrDefault();
                    if (name.size() > 6 && name.compare(name.size() - 6, 6, ".geode") == 0) {
                        out.url = a["browser_download_url"].asString().unwrapOrDefault();
                        out.size = (size_t)a["size"].asInt().unwrapOr(0);
                        auto dg = a["digest"].asString().unwrapOrDefault();
                        if (dg.rfind("sha256:", 0) == 0) dg.erase(0, 7);
                        for (auto& ch : dg) ch = (char)std::tolower((unsigned char)ch);
                        out.digest = isHex64(dg) ? dg : std::string();
                    }
                }
                if (!out.tag.empty() && !out.url.empty()) m_rels.push_back(std::move(out));
            }
            if (m_rels.empty()) { fail("No installable versions were found"); return; }
            m_page = Page::Select;
            rebuild();
        });
    }

    void startInstall() {
        auto const& r = m_rels[m_sel];
        m_page = Page::Installing;
        m_msg = "Downloading " + r.tag + "...";
        rebuild();
        m_dl.spawn(req(180).get(r.url), [this](web::WebResponse resp) {
            auto const& r = m_rels[m_sel];
            if (!resp.ok()) { fail(resp.code() <= 0 ? "The download failed" : fmt::format("The download failed (error {})", resp.code())); return; }
            auto bytes = std::move(resp).data();
            if (bytes.size() < 4096 || bytes[0] != 'P' || bytes[1] != 'K') { fail("The downloaded file is not a valid mod"); return; }
            if (!r.digest.empty() && xyzinst::sha256Hex(bytes) != r.digest) { fail("The download does not match its checksum, nothing was installed"); return; }
            auto pkg = targetPath();
            std::error_code ec;
            std::filesystem::create_directories(pkg.parent_path(), ec);
            auto fresh = pkg; fresh += ".update";
            auto backup = pkg; backup += ".bak";
            {
                std::ofstream f(fresh, std::ios::binary);
                if (!f || !f.write(reinterpret_cast<char const*>(bytes.data()), (std::streamsize)bytes.size())) { fail("Could not write the mod file"); return; }
            }
            bool had = std::filesystem::exists(pkg, ec);
            if (had) {
                std::filesystem::remove(backup, ec);
                std::filesystem::rename(pkg, backup, ec);
                if (ec) { fail("Could not replace the mod file (" + ec.message() + ")"); return; }
            }
            std::filesystem::rename(fresh, pkg, ec);
            if (ec) {
                std::error_code undo;
                if (had) std::filesystem::rename(backup, pkg, undo);
                fail("Could not place the new mod file (" + ec.message() + ")");
                return;
            }
            if (had) { std::error_code rm; std::filesystem::remove(backup, rm); }
            m_msg = "xyzBot " + r.tag + " is installed. Restart the game to use it.";
            m_page = Page::Done;
            rebuild();
        });
    }

    void rebuild() {
        m_body->removeAllChildren();
        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_body->addChild(menu);
        float cx = W / 2.f;

        switch (m_page) {
        case Page::Loading:
            label("Loading versions...", cx, H / 2.f, 0.5f);
            break;
        case Page::Failed:
            label(m_msg, cx, H / 2.f + 10.f, 0.4f, false, {255, 120, 120})->setWidth(W - 40.f);
            button(menu, "Retry", cx - 55.f, 44.f, 80, [this] { loadList(); });
            button(menu, "Close", cx + 55.f, 44.f, 80, [this] { this->onClose(nullptr); });
            break;
        case Page::Select: {
            this->setTitle("Select a version");
            auto inst = norm(installedTag());
            label(inst.empty() ? "xyzBot is not installed" : "Installed: v" + inst, cx, H - 42.f, 0.34f, false, {180, 180, 180});
            float lw = W - 50.f, lh = H - 100.f, rowH = 34.f;
            auto scroll = ScrollLayer::create({lw, lh});
            scroll->setPosition({(W - lw) / 2.f, 30.f});
            float contentH = std::max(lh, rowH * m_rels.size());
            scroll->m_contentLayer->setContentSize({lw, contentH});
            auto smenu = CCMenu::create();
            smenu->setPosition({0, 0});
            smenu->setContentSize({lw, contentH});
            scroll->m_contentLayer->addChild(smenu);
            for (size_t i = 0; i < m_rels.size(); ++i) {
                auto const& r = m_rels[i];
                std::string t = r.tag + (i == 0 ? "  (latest)" : "") + (r.pre ? "  (pre)" : "") + (norm(r.tag) == inst ? "  *" : "");
                if (!r.date.empty()) t += "  " + r.date;
                auto spr = ButtonSprite::create(t.c_str(), (int)lw - 20, true, "bigFont.fnt", "GJ_button_04.png", 28.f, 0.5f);
                auto b = CCMenuItemExt::createSpriteExtra(spr, [this, i](auto) { queueInMainThread([this, i] { m_sel = i; m_page = Page::Changelog; rebuild(); }); });
                b->setPosition({lw / 2.f, contentH - rowH * (i + 0.5f)});
                smenu->addChild(b);
            }
            scroll->moveToTop();
            m_body->addChild(scroll);
            button(menu, "Close", cx, 14.f, 70, [this] { this->onClose(nullptr); }, "GJ_button_06.png", 0.45f);
            break;
        }
        case Page::Changelog: {
            auto const& r = m_rels[m_sel];
            this->setTitle("Changelog " + r.tag);
            auto body = r.body.empty() ? std::string("No changelog was provided for this version.") : r.body;
            auto md = MDTextArea::create(body, {W - 50.f, H - 112.f});
            md->setPosition({cx, H - 48.f - (H - 112.f) / 2.f});
            m_body->addChild(md);
            std::string info = r.tag + (r.date.empty() ? "" : "  |  " + r.date) + (r.size ? fmt::format("  |  {:.1f} MB", r.size / 1024.0 / 1024.0) : "");
            label(info, cx, 46.f, 0.3f, false, {170, 170, 170});
            bool same = norm(r.tag) == norm(installedTag());
            button(menu, "Back", 70.f, 20.f, 80, [this] { m_page = Page::Select; rebuild(); }, "GJ_button_06.png", 0.5f);
            button(menu, (same ? "Reinstall " : "Install ") + r.tag, W - 120.f, 20.f, 190, [this] { startInstall(); }, "GJ_button_01.png", 0.5f);
            break;
        }
        case Page::Installing:
            label(m_msg, cx, H / 2.f, 0.45f);
            break;
        case Page::Done:
            this->setTitle("Done");
            label(m_msg, cx, H / 2.f + 14.f, 0.4f, false, {120, 255, 150});
            button(menu, "Restart Game", cx - 70.f, 44.f, 120, [] {
#ifdef XYZI_HAS_RESTART
                geode::utils::game::restart();
#else
                Notification::create("Please restart the game manually", NotificationIcon::Info)->show();
#endif
            }, "GJ_button_01.png");
            button(menu, "Close", cx + 70.f, 44.f, 80, [this] { this->onClose(nullptr); });
            break;
        }
    }

public:
    static void open() {
        auto p = new InstallerPopup();
        if (p->setup()) { p->autorelease(); p->show(); }
        else delete p;
    }

    bool setup() {
        if (!Popup::init(W, H)) return false;
        this->setTitle("xyzBot Installer");
        m_body = CCNode::create();
        m_body->setPosition({0, 0});
        m_mainLayer->addChild(m_body);
        loadList();
        return true;
    }
};
}

namespace xyzinst {
void openInstaller() { InstallerPopup::open(); }
}
