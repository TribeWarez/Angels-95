#pragma once
// ---------------------------------------------------------------------------
// Menu/MenuMetrics.hpp
//
// Pure, raylib-free layout math for the title menu. Every rectangle the menu
// draws comes from here, so a page can never place a control outside its content
// area by hardcoding an offset. Unit-tested by tests/Menu.test.cpp at 960x600
// through 3840x2160 and at several DPI/UI-scale combinations.
//
// The design is deliberately a single-shell layout: title bar, one navigation
// bar, one content area, one footer/status bar. The old floating-pane geometry
// (cascade offsets, per-pane sizes) is gone.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cmath>

namespace angelsmenu {

struct UiRect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
    float CenterX() const { return x + w * 0.5f; }
    float CenterY() const { return y + h * 0.5f; }

    bool Contains(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }

    UiRect Inset(float amount) const {
        return {x + amount, y + amount,
                std::max(0.0f, w - amount * 2.0f),
                std::max(0.0f, h - amount * 2.0f)};
    }

    UiRect ClampInside(const UiRect& outer) const {
        if (w > outer.w) return {outer.x, y, outer.w, h};
        if (h > outer.h) return {x, outer.y, w, outer.h};
        UiRect r = *this;
        if (r.x < outer.x) r.x = outer.x;
        if (r.y < outer.y) r.y = outer.y;
        if (r.Right() > outer.Right()) r.x = outer.Right() - r.w;
        if (r.Bottom() > outer.Bottom()) r.y = outer.Bottom() - r.h;
        return r;
    }
};

struct MenuMetrics {
    float screenW = 1280.0f;
    float screenH = 720.0f;
    float scale = 1.0f;
    float dpi = 1.0f;
    float userScale = 1.0f;

    bool compact = false;   // narrow: stack columns, short labels
    bool wide = false;      // room for a details column

    UiRect frame;      // outer window frame
    UiRect titleBar;   // window chrome / branding
    UiRect navBar;     // Play / Multiplayer / Character / Settings
    UiRect content;    // page body
    UiRect footer;     // status + context help

    float pad = 16.0f;
    float gap = 10.0f;
    float rowH = 30.0f;
    float fontSm = 12.0f;
    float fontMd = 14.0f;
    float fontLg = 20.0f;
    float fontXl = 30.0f;

    static MenuMetrics Compute(float w, float h, float dpiScale = 1.0f,
                               float uiScale = 1.0f) {
        MenuMetrics m;
        m.screenW = std::max(1.0f, w);
        m.screenH = std::max(1.0f, h);
        m.dpi = (dpiScale > 0.01f) ? dpiScale : 1.0f;
        m.userScale = std::max(0.5f, uiScale);

        // Base design resolution 1600x900. The clamp keeps text readable on a
        // small window and prevents it from becoming absurd on a 4K panel.
        float autoScale = std::min(m.screenW / 1600.0f, m.screenH / 900.0f);
        m.scale = std::clamp(autoScale * m.dpi * m.userScale, 0.75f, 2.5f);

        m.compact = m.screenW < 1100.0f;
        m.wide = m.screenW >= 1150.0f;

        m.pad = std::round(16.0f * m.scale);
        m.gap = std::round(10.0f * m.scale);
        m.rowH = std::round(30.0f * m.scale);
        m.fontSm = std::round(12.0f * m.scale);
        m.fontMd = std::round(14.0f * m.scale);
        m.fontLg = std::round(20.0f * m.scale);
        m.fontXl = std::round(30.0f * m.scale);

        const float margin = std::round(8.0f * m.scale);
        m.frame = {margin, margin,
                   std::max(0.0f, m.screenW - margin * 2.0f),
                   std::max(0.0f, m.screenH - margin * 2.0f)};

        const float border = std::max(2.0f, std::round(4.0f * m.scale));
        const float titleH = std::round(32.0f * m.scale);
        const float navH = std::round(40.0f * m.scale);
        const float footerH = std::round(26.0f * m.scale);

        float x = m.frame.x + border;
        float y = m.frame.y + border;
        float iw = std::max(0.0f, m.frame.w - border * 2.0f);
        float ih = std::max(0.0f, m.frame.h - border * 2.0f);

        m.titleBar = {x, y, iw, titleH};
        y += titleH;
        m.navBar = {x, y, iw, navH};
        y += navH;
        const float footerTop = m.frame.y + m.frame.h - border - footerH;
        const float contentH = std::max(0.0f, footerTop - y);
        m.content = {x, y, iw, contentH};
        m.footer = {x, footerTop, iw, footerH};
        return m;
    }

    // Navigation items are evenly distributed across the nav bar. Four items in
    // the shipped design, but the function is count-agnostic so adding one does
    // not require touching page code.
    UiRect NavItem(int index, int count) const {
        if (count <= 0) return {};
        const float leftPad = std::round(12.0f * scale);
        const float rightPad = std::round(12.0f * scale);
        const float usable = std::max(0.0f, navBar.w - leftPad - rightPad);
        const float itemW = usable / (float)count;
        return {navBar.x + leftPad + itemW * (float)index, navBar.y,
                itemW, navBar.h};
    }

    // Standard page body: content inset by `pad`. Pages that need a full-bleed
    // background can use content directly.
    UiRect PageBody() const { return content.Inset(pad); }

    // A footer-aligned button row, e.g. Back / Continue.
    UiRect FooterButton(int index, int count, float width = 0.0f) const {
        const float h = std::round(28.0f * scale);
        const float bw = (width > 0.0f) ? width : std::round(150.0f * scale);
        const float y = footer.y + (footer.h - h) * 0.5f;
        return {footer.Right() - pad - bw * (float)(count - index), y, bw, h};
    }
};

} // namespace angelsmenu
