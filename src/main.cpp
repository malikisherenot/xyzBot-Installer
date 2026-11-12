#include "Installer.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>

using namespace geode::prelude;

class $modify(XyzInstMenu, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        auto menu = this->getChildByID("bottom-menu");
        if (!menu) return true;
        auto spr = CCSprite::create("installer-button.png"_spr);
        if (!spr) return true;
        spr->setScale(1.55f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(XyzInstMenu::onXyzInstaller));
        btn->setID("open-installer"_spr);
        menu->addChild(btn);
        menu->updateLayout();
        return true;
    }

    void onXyzInstaller(CCObject*) { xyzinst::openInstaller(); }
};
