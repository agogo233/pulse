// color_rule_panel.cpp — the 颜色规则 panel (see the header).
//
// Same surface as QuickAccessEditorPopup: the card and its shadow are drawn
// with Direct2D into an offscreen bitmap, read back, and shown with
// UpdateLayeredWindow; a modal message loop runs until the panel closes.
//
// Card (DIP): width 440; title row 34; hint 22; rows 34 (at most 10 before the
// list scrolls; the list keeps its height); divider; footer 32 with
// 添加规则 / 上移 / 下移 / 删除 on the left and 完成 on the right.
// Row: enable check box, colour dot, condition summary.
#include "color_rule_panel.h"

#include "../app/color_rule.h"

#include "fluent_components.h"
#include "fluent_menu.h"
#include "typography.h"
#include "ui_compositor.h"

#include <windowsx.h>
#include <wincodec.h>
#include <d2d1effects.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace pulse::ui {

namespace {

// CLSID_D2D1Shadow (d2d1.lib does not export the effect CLSIDs).
constexpr GUID kShadowEffectClsid = { 0xC67EA361, 0x1863, 0x4e69,
    { 0x89, 0xDB, 0x69, 0x5D, 0x3E, 0x9D, 0x5B, 0x53 } };

constexpr wchar_t kClassName[] = L"PulseColorRules";

constexpr float kCardW = 440.0f;
constexpr float kPadX = 16.0f;
constexpr float kPadTop = 10.0f;
constexpr float kTitleH = 34.0f;
constexpr float kHintH = 22.0f;
constexpr float kListGap = 6.0f;
constexpr float kListInset = 8.0f;
constexpr float kRowH = 34.0f;
constexpr size_t kMaxVisibleRows = 10;
constexpr float kFooterGap = 10.0f;
constexpr float kFooterH = 32.0f;
constexpr float kPadBottom = 14.0f;
constexpr int kShadow = FluentMenu::kShadowMargin;

enum class Part { None, Row, Check, Add, Up, Down, Remove, Done };

struct Hit {
    Part part = Part::None;
    int index = -1;   // row index
    bool operator==(const Hit&) const = default;
};

struct State {
    HWND hwnd = nullptr;
    Compositor* compositor = nullptr;
    float scale = 1.0f;
    Theme theme{};
    fluent::Painter painter;
    const ColorRuleText* text = nullptr;
    std::vector<ColorRuleRow>* rows = nullptr;
    ColorRuleView* view = nullptr;
    int max_rules = 100;
    ColorRuleAction result = ColorRuleAction::Cancelled;
    bool done = false;

    int base_x = 0;
    int base_y = 0;
    int present_dy = 0;
    int win_w = 0;
    int win_h = 0;

    // Window pixels, shadow margin included.
    D2D1_RECT_F r_card{};
    D2D1_RECT_F r_title{};
    D2D1_RECT_F r_hint{};
    D2D1_RECT_F r_list{};
    D2D1_RECT_F r_divider{};
    D2D1_RECT_F r_add{};
    D2D1_RECT_F r_up{};
    D2D1_RECT_F r_down{};
    D2D1_RECT_F r_remove{};
    D2D1_RECT_F r_done{};
    float scroll = 0.0f;   // pixels

    Hit hot;
    Hit press;
    int focus = -1;        // row index
    bool keyboard_focus = false;
    bool tracking_leave = false;

    HDC mem_dc = nullptr;
    HBITMAP dib = nullptr;
    void* bits = nullptr;
    int dib_w = 0;
    int dib_h = 0;
};

float Px(const State& s, float dip) { return dip * s.scale; }

bool Contains(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

int RowCount(const State& s) { return static_cast<int>(s.rows->size()); }

ColorRuleRow& RowAt(State& s, int index) { return (*s.rows)[static_cast<size_t>(index)]; }

float RowPx(const State& s) { return Px(s, kRowH); }

float MaxScroll(const State& s) {
    const float content = RowPx(s) * static_cast<float>(RowCount(s));
    return (std::max)(0.0f, content - (s.r_list.bottom - s.r_list.top));
}

void ClampScroll(State& s) { s.scroll = std::clamp(s.scroll, 0.0f, MaxScroll(s)); }

void Layout(State& s) {
    // The list is as tall as the whole rule set needs (up to the cap), so the
    // card never resizes under the pointer.
    const size_t shown = (std::max)(size_t{1}, (std::min)(s.rows->size(), kMaxVisibleRows));
    const float list_h = kRowH * static_cast<float>(shown);
    const float card_h = kPadTop + kTitleH + kHintH + kListGap + list_h + kFooterGap + 1.0f +
                         kFooterGap + kFooterH + kPadBottom;
    const float m = static_cast<float>(kShadow);
    auto rect = [&](float l, float t, float r, float b) {
        return D2D1::RectF(m + Px(s, l), m + Px(s, t), m + Px(s, r), m + Px(s, b));
    };
    s.r_card = rect(0.0f, 0.0f, kCardW, card_h);
    float y = kPadTop;
    const float title_top = m + Px(s, y);
    const float title_bottom = m + Px(s, y + kTitleH);
    s.r_title = D2D1::RectF(s.r_card.left + Px(s, kPadX), title_top,
                            s.r_card.right - Px(s, kPadX), title_bottom);
    y += kTitleH;
    s.r_hint = rect(kPadX, y, kCardW - kPadX, y + kHintH);
    y += kHintH + kListGap;
    s.r_list = rect(kListInset, y, kCardW - kListInset, y + list_h);
    y += list_h + kFooterGap;
    s.r_divider = rect(kPadX, y, kCardW - kPadX, y + 1.0f);
    y += 1.0f + kFooterGap;
    const float footer_top = m + Px(s, y);
    const float footer_bottom = m + Px(s, y + kFooterH);
    const float done_w = (std::max)(Px(s, 80.0f), s.painter.MeasureButtonWidth(s.text->done));
    s.r_done = D2D1::RectF(s.r_card.right - Px(s, kPadX) - done_w, footer_top,
                           s.r_card.right - Px(s, kPadX), footer_bottom);
    float left = s.r_card.left + Px(s, kPadX);
    const auto place = [&](D2D1_RECT_F& r, const std::wstring& label, const wchar_t* glyph) {
        const float w = s.painter.MeasureButtonWidth(label, glyph);
        r = D2D1::RectF(left, footer_top, left + w, footer_bottom);
        left += w + Px(s, 6.0f);
    };
    place(s.r_add, s.text->add, L"\xE8B4");
    place(s.r_up, s.text->up, L"\xE8B7");
    place(s.r_down, s.text->down, L"\xE8B8");
    place(s.r_remove, s.text->remove, L"\xE74D");
    s.win_w = static_cast<int>(std::lround(Px(s, kCardW))) + kShadow * 2;
    s.win_h = static_cast<int>(std::lround(Px(s, card_h))) + kShadow * 2;
    ClampScroll(s);
}

D2D1_RECT_F RowRect(const State& s, int position) {
    const float top = s.r_list.top + RowPx(s) * static_cast<float>(position) - s.scroll;
    return D2D1::RectF(s.r_list.left, top, s.r_list.right, top + RowPx(s));
}

D2D1_RECT_F CheckRect(const State& s, const D2D1_RECT_F& row) {
    const float left = row.left + Px(s, 8.0f);
    return D2D1::RectF(left, row.top, left + Px(s, 18.0f), row.bottom);
}

bool ActionEnabled(const State& s, Part part) {
    switch (part) {
    case Part::Add: return RowCount(s) < s.max_rules;
    case Part::Up: return s.focus > 0;
    case Part::Down: return s.focus >= 0 && s.focus < RowCount(s) - 1;
    case Part::Remove: return s.focus >= 0;
    default: return true;
    }
}

Hit HitTest(const State& s, float x, float y) {
    if (Contains(s.r_add, x, y)) return { Part::Add, -1 };
    if (Contains(s.r_up, x, y)) return { Part::Up, -1 };
    if (Contains(s.r_down, x, y)) return { Part::Down, -1 };
    if (Contains(s.r_remove, x, y)) return { Part::Remove, -1 };
    if (Contains(s.r_done, x, y)) return { Part::Done, -1 };
    if (!Contains(s.r_list, x, y)) return {};
    const int position = static_cast<int>((y - s.r_list.top + s.scroll) / RowPx(s));
    if (position < 0 || position >= RowCount(s)) return {};
    const D2D1_RECT_F row = RowRect(s, position);
    if (x < CheckRect(s, row).right + Px(s, 4.0f)) return { Part::Check, position };
    return { Part::Row, position };
}

void DrawRow(State& s, ID2D1DeviceContext* dc, int position, const D2D1_RECT_F& row) {
    const auto& item = RowAt(s, position);
    const bool hovered = s.hot.index == position &&
        (s.hot.part == Part::Row || s.hot.part == Part::Check);
    const D2D1_RECT_F pill = D2D1::RectF(row.left, row.top + Px(s, 1.5f), row.right,
                                         row.bottom - Px(s, 1.5f));
    const float radius = Px(s, 5.0f);
    if (hovered) s.painter.FillRoundedRect(pill, radius, s.theme.fill_hover);
    if (s.keyboard_focus && s.focus == position) s.painter.DrawFocusRing(pill, radius);

    fluent::ControlState check;
    check.checked = item.rule.enabled;
    check.hovered = hovered && s.hot.part == Part::Check;
    s.painter.DrawCheckBox(CheckRect(s, row), {}, check);

    const float cy = (row.top + row.bottom) * 0.5f;
    const float dot = Px(s, 5.0f);
    const float dx = row.left + Px(s, 40.0f);
    s.painter.FillRoundedRect(D2D1::RectF(dx - dot, cy - dot, dx + dot, cy + dot), dot,
                              HexColor(item.rule.color));

    const float left = row.left + Px(s, 54.0f);
    const float right = row.right - Px(s, 8.0f);
    dc->PushAxisAlignedClip(D2D1::RectF(left, row.top, right, row.bottom),
                            D2D1_ANTIALIAS_MODE_ALIASED);
    const D2D1_COLOR_F background = s.theme.surface_flyout;
    s.painter.DrawText(item.summary, D2D1::RectF(left, row.top, right, row.bottom),
                       s.compositor->SmallFormat(), s.theme.text,
                       fluent::HorizontalAlignment::Left, background);
    dc->PopAxisAlignedClip();
}

void DrawPanelButton(State& s, const D2D1_RECT_F& bounds, std::wstring_view label,
                     std::wstring_view glyph, fluent::ButtonKind kind, Part part, bool bordered) {
    fluent::ButtonSpec spec;
    spec.bounds = bounds;
    spec.text = label;
    spec.glyph = glyph;
    spec.kind = kind;
    spec.bordered = bordered;
    spec.state.enabled = ActionEnabled(s, part);
    spec.state.hovered = spec.state.enabled && s.hot.part == part;
    spec.state.pressed = spec.state.hovered && s.press.part == part;
    s.painter.DrawButton(spec);
}

bool Render(State& s) {
    ID2D1DeviceContext* dc = s.compositor ? s.compositor->Dc() : nullptr;
    if (!dc || !s.text || !s.rows || s.win_w <= 0 || s.win_h <= 0) return false;
    const D2D1_SIZE_U size{ static_cast<UINT32>(s.win_w), static_cast<UINT32>(s.win_h) };
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> bmp;
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &bmp))) return false;

    ComPtr<ID2D1Image> old_target;
    dc->GetTarget(&old_target);
    dc->SetTarget(bmp.get());
    // Grayscale text AA: ClearType fringes survive the premultiplied readback.
    const D2D1_TEXT_ANTIALIAS_MODE old_text_aa = dc->GetTextAntialiasMode();
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    const D2D1_RECT_F card = s.r_card;
    const float radius = Px(s, s.theme.radius_flyout);
    ComPtr<ID2D1CommandList> card_list;
    if (SUCCEEDED(dc->CreateCommandList(&card_list))) {
        dc->SetTarget(card_list.get());
        ComPtr<ID2D1SolidColorBrush> black;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1.0f), &black);
        dc->FillRoundedRectangle(D2D1::RoundedRect(card, radius, radius), black.get());
        card_list->Close();
        dc->SetTarget(bmp.get());
        ComPtr<ID2D1Effect> shadow;
        if (SUCCEEDED(dc->CreateEffect(kShadowEffectClsid, &shadow))) {
            shadow->SetInput(0, card_list.get());
            shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 7.0f);
            shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0.0f, 0.0f, 0.0f, 0.36f));
            dc->DrawImage(shadow.get(), D2D1::Point2F(0.0f, 4.0f * s.scale));
        }
    }

    if (!s.painter.BeginFrame(s.theme, IsHighContrast())) {
        dc->EndDraw();
        dc->SetTextAntialiasMode(old_text_aa);
        dc->SetTarget(old_target.get());
        return false;
    }
    s.painter.DrawMenuSurface(card);

    const D2D1_COLOR_F background = s.theme.surface_flyout;
    s.painter.DrawText(s.text->title, s.r_title, s.compositor->TextFormat(), s.theme.text,
                       fluent::HorizontalAlignment::Left, background);
    const std::wstring& hint = RowCount(s) >= s.max_rules ? s.text->max : s.text->hint;
    s.painter.DrawText(hint, s.r_hint, s.compositor->SmallFormat(),
                       s.theme.text_secondary, fluent::HorizontalAlignment::Left, background);

    dc->PushAxisAlignedClip(s.r_list, D2D1_ANTIALIAS_MODE_ALIASED);
    if (RowCount(s) == 0) {
        const D2D1_RECT_F first = D2D1::RectF(s.r_list.left, s.r_list.top, s.r_list.right,
                                              s.r_list.top + RowPx(s));
        s.painter.DrawText(s.text->empty, first, s.compositor->SmallFormat(),
                           s.theme.text_secondary, fluent::HorizontalAlignment::Center,
                           background);
    }
    for (int p = 0; p < RowCount(s); ++p) {
        const D2D1_RECT_F row = RowRect(s, p);
        if (row.bottom <= s.r_list.top || row.top >= s.r_list.bottom) continue;
        DrawRow(s, dc, p, row);
    }
    dc->PopAxisAlignedClip();

    // A thin thumb tells there is more to scroll.
    if (const float max_scroll = MaxScroll(s); max_scroll > 0.0f) {
        const float track = s.r_list.bottom - s.r_list.top;
        const float content = track + max_scroll;
        const float thumb = (std::max)(Px(s, 24.0f), track * track / content);
        const float top = s.r_list.top + (track - thumb) * (s.scroll / max_scroll);
        D2D1_COLOR_F color = s.theme.text_secondary;
        color.a *= 0.5f;
        s.painter.FillRoundedRect(D2D1::RectF(s.r_list.right - Px(s, 3.0f), top,
                                              s.r_list.right - Px(s, 1.0f), top + thumb),
                                  Px(s, 1.0f), color);
    }

    s.painter.FillRoundedRect(s.r_divider, 0.0f, s.theme.stroke_divider);
    DrawPanelButton(s, s.r_add, s.text->add, L"\xE8B4", fluent::ButtonKind::Standard, Part::Add, true);
    DrawPanelButton(s, s.r_up, s.text->up, L"\xE8B7", fluent::ButtonKind::Standard, Part::Up, true);
    DrawPanelButton(s, s.r_down, s.text->down, L"\xE8B8", fluent::ButtonKind::Standard, Part::Down, true);
    DrawPanelButton(s, s.r_remove, s.text->remove, L"\xE74D", fluent::ButtonKind::Standard, Part::Remove, true);
    DrawPanelButton(s, s.r_done, s.text->done, {}, fluent::ButtonKind::Primary, Part::Done, true);

    HRESULT hr = dc->EndDraw();
    dc->SetTextAntialiasMode(old_text_aa);
    dc->SetTarget(old_target.get());
    if (FAILED(hr)) return false;

    const auto cpu_props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> cpu;
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, cpu_props, &cpu))) return false;
    if (FAILED(cpu->CopyFromBitmap(nullptr, bmp.get(), nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;

    if (!s.mem_dc) s.mem_dc = CreateCompatibleDC(nullptr);
    if (s.dib && (s.dib_w != s.win_w || s.dib_h != s.win_h)) {
        DeleteObject(s.dib);
        s.dib = nullptr;
        s.bits = nullptr;
    }
    if (!s.dib) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = s.win_w;
        bmi.bmiHeader.biHeight = -s.win_h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        s.dib = CreateDIBSection(s.mem_dc, &bmi, DIB_RGB_COLORS, &s.bits, nullptr, 0);
        if (!s.dib) {
            cpu->Unmap();
            return false;
        }
        s.dib_w = s.win_w;
        s.dib_h = s.win_h;
    }
    for (int row = 0; row < s.win_h; ++row) {
        std::memcpy(static_cast<uint8_t*>(s.bits) + static_cast<size_t>(row) * s.win_w * 4,
                    static_cast<const uint8_t*>(mapped.bits) +
                        static_cast<size_t>(row) * mapped.pitch,
                    static_cast<size_t>(s.win_w) * 4);
    }
    cpu->Unmap();
    return true;
}

void Present(State& s, BYTE alpha, int dy) {
    if (!s.dib || !s.bits) return;
    HGDIOBJ old = SelectObject(s.mem_dc, s.dib);
    POINT dst{ s.base_x, s.base_y + dy };
    POINT src{ 0, 0 };
    SIZE sz{ s.win_w, s.win_h };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
    UpdateLayeredWindow(s.hwnd, nullptr, &dst, &sz, s.mem_dc, &src, 0, &blend, ULW_ALPHA);
    SelectObject(s.mem_dc, old);
    s.present_dy = dy;
}

void Repaint(State& s) {
    if (Render(s)) Present(s, 255, s.present_dy);
}

void Finish(State& s, ColorRuleAction result) {
    s.result = result;
    s.done = true;
}

void EnsureVisible(State& s, int position) {
    if (position < 0) return;
    const float top = RowPx(s) * static_cast<float>(position);
    const float view = s.r_list.bottom - s.r_list.top;
    if (top < s.scroll) s.scroll = top;
    else if (top + RowPx(s) > s.scroll + view) s.scroll = top + RowPx(s) - view;
    ClampScroll(s);
}

void RefreshHot(State& s) {
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(s.hwnd, &pt);
    s.hot = HitTest(s, static_cast<float>(pt.x), static_cast<float>(pt.y));
}

// The caller's follow-up menu opens under the focused row (or the pointer).
void FinishAction(State& s, ColorRuleAction action, const POINT* client_pt = nullptr) {
    POINT pt{};
    if (client_pt) {
        pt = *client_pt;
    } else if (s.focus >= 0) {
        const D2D1_RECT_F row = RowRect(s, s.focus);
        pt = POINT{ static_cast<LONG>(row.left + Px(s, 54.0f)), static_cast<LONG>(row.bottom) };
    } else {
        pt = POINT{ static_cast<LONG>(s.r_add.left), static_cast<LONG>(s.r_add.bottom) };
    }
    ClientToScreen(s.hwnd, &pt);
    s.view->action_pt = pt;
    Finish(s, action);
}

bool HandleKey(State& s, WPARAM key) {
    const int count = RowCount(s);
    switch (key) {
    case VK_ESCAPE:
        Finish(s, ColorRuleAction::Cancelled);
        return true;
    case VK_RETURN:
        Finish(s, ColorRuleAction::Closed);
        return true;
    case VK_UP:
    case VK_DOWN:
    case VK_HOME:
    case VK_END: {
        if (count == 0) return true;
        s.keyboard_focus = true;
        int to = s.focus < 0 ? 0 : s.focus;
        if (key == VK_UP) to = (std::max)(0, to - 1);
        else if (key == VK_DOWN) to = (std::min)(count - 1, s.focus < 0 ? 0 : to + 1);
        else to = key == VK_HOME ? 0 : count - 1;
        s.focus = to;
        EnsureVisible(s, s.focus);
        Repaint(s);
        return true;
    }
    case VK_SPACE:
        if (s.focus >= 0) {
            s.keyboard_focus = true;
            RowAt(s, s.focus).rule.enabled = !RowAt(s, s.focus).rule.enabled;
            Repaint(s);
        }
        return true;
    case VK_F2:
        if (s.focus >= 0) FinishAction(s, ColorRuleAction::Edit);
        return true;
    case VK_DELETE:
        if (s.focus >= 0) FinishAction(s, ColorRuleAction::Remove);
        return true;
    default:
        return false;
    }
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    State* s = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        s = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        s->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    }
    if (!s) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const float x = static_cast<float>(GET_X_LPARAM(lp));
        const float y = static_cast<float>(GET_Y_LPARAM(lp));
        SetFocus(hwnd);
        const Hit hit = HitTest(*s, x, y);
        if (msg == WM_LBUTTONDBLCLK && hit.part == Part::Row) {
            s->focus = hit.index;
            FinishAction(*s, ColorRuleAction::Edit);
            return 0;
        }
        if (hit.part == Part::None) return 0;
        s->press = hit;
        s->keyboard_focus = false;
        SetCapture(hwnd);
        Repaint(*s);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const Hit hit = HitTest(*s, static_cast<float>(GET_X_LPARAM(lp)),
                                static_cast<float>(GET_Y_LPARAM(lp)));
        if (!(hit == s->hot)) {
            s->hot = hit;
            Repaint(*s);
        }
        if (!s->tracking_leave) {
            s->tracking_leave = true;
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        s->tracking_leave = false;
        if (!(s->hot == Hit{})) {
            s->hot = {};
            Repaint(*s);
        }
        return 0;
    case WM_LBUTTONUP: {
        const float x = static_cast<float>(GET_X_LPARAM(lp));
        const float y = static_cast<float>(GET_Y_LPARAM(lp));
        const Hit hit = HitTest(*s, x, y);
        // Snapshot before ReleaseCapture: WM_CAPTURECHANGED clears the press.
        const Hit press = s->press;
        s->press = {};
        if (GetCapture() == hwnd) ReleaseCapture();
        if (press == hit && hit.part != Part::None && ActionEnabled(*s, hit.part)) {
            switch (hit.part) {
            case Part::Check:
                RowAt(*s, hit.index).rule.enabled = !RowAt(*s, hit.index).rule.enabled;
                s->focus = hit.index;
                break;
            case Part::Row:
                s->focus = hit.index;
                break;
            case Part::Add: FinishAction(*s, ColorRuleAction::Add); return 0;
            case Part::Up: FinishAction(*s, ColorRuleAction::Up); return 0;
            case Part::Down: FinishAction(*s, ColorRuleAction::Down); return 0;
            case Part::Remove: FinishAction(*s, ColorRuleAction::Remove); return 0;
            case Part::Done: Finish(*s, ColorRuleAction::Closed); return 0;
            default: break;
            }
        }
        s->hot = HitTest(*s, x, y);
        Repaint(*s);
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const Hit hit = HitTest(*s, static_cast<float>(pt.x), static_cast<float>(pt.y));
        if (hit.part != Part::Row && hit.part != Part::Check) return 0;
        s->focus = hit.index;
        FinishAction(*s, ColorRuleAction::Edit, &pt);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        s->scroll -= notches * RowPx(*s) * 2.0f;
        ClampScroll(*s);
        RefreshHot(*s);
        Repaint(*s);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (HandleKey(*s, wp)) return 0;
        break;
    case WM_CAPTURECHANGED:
        s->press = {};
        return 0;
    case WM_CLOSE:
        Finish(*s, ColorRuleAction::Closed);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void DestroySurface(State& s) {
    if (s.dib) DeleteObject(s.dib);
    if (s.mem_dc) DeleteDC(s.mem_dc);
    s.dib = nullptr;
    s.mem_dc = nullptr;
    s.bits = nullptr;
}

bool SavePng(const wchar_t* path, int w, int h, void* bits) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
        return false;
    }
    ComPtr<IWICStream> stream;
    wic->CreateStream(&stream);
    if (!stream.get() || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) {
        return false;
    }
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr))) return false;
    if (FAILED(frame->Initialize(nullptr))) return false;
    if (FAILED(frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h)))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    if (FAILED(frame->WritePixels(static_cast<UINT>(h), static_cast<UINT>(w * 4),
                                  static_cast<UINT>(w * 4 * h), static_cast<BYTE*>(bits)))) {
        return false;
    }
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(encoder->Commit());
}

bool RegisterPanelClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    registered = true;
    return true;
}

void Init(State& s, Compositor* compositor, float scale, const Theme& theme,
          const ColorRuleText& text, std::vector<ColorRuleRow>& rows, ColorRuleView& view,
          int max_rules) {
    s.compositor = compositor;
    s.scale = (std::max)(0.5f, scale);
    s.theme = theme;
    s.text = &text;
    s.rows = &rows;
    s.view = &view;
    s.max_rules = max_rules;
    s.painter.SetCompositor(compositor);
    s.painter.SetScale(s.scale);
    Layout(s);
    s.focus = view.focus >= 0 && view.focus < static_cast<int>(rows.size()) ? view.focus : -1;
    s.scroll = view.scroll * s.scale;
    ClampScroll(s);
}

} // namespace

ColorRuleAction ColorRulePopup::Edit(HWND owner, Compositor* compositor, float scale,
                                      POINT screen_pt, bool dark, D2D1_COLOR_F accent,
                                      const ColorRuleText& text,
                                      std::vector<ColorRuleRow>& rows, ColorRuleView& view,
                                      int max_rules) {
    if (!compositor || !compositor->Dc() || !RegisterPanelClass())
        return ColorRuleAction::Cancelled;

    State s;
    Init(s, compositor, scale,
         IsHighContrast() ? MakeHighContrastTheme() : MakeTheme(dark, accent), text,
         rows, view, max_rules);

    s.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, kClassName, L"", WS_POPUP,
                             0, 0, s.win_w, s.win_h, owner, nullptr,
                             GetModuleHandleW(nullptr), &s);
    if (!s.hwnd) return ColorRuleAction::Cancelled;

    HMONITOR monitor = MonitorFromPoint(screen_pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);
    s.base_x = std::clamp(screen_pt.x - kShadow, mi.rcWork.left,
                          (std::max)(mi.rcWork.left, mi.rcWork.right - s.win_w));
    s.base_y = std::clamp(screen_pt.y - kShadow, mi.rcWork.top,
                          (std::max)(mi.rcWork.top, mi.rcWork.bottom - s.win_h));

    // Fade in while sliding down by the menu travel, 170 ms OutCubic; a
    // reopen after a follow-up menu shows at once.
    if (Render(s)) {
        ShowWindow(s.hwnd, SW_SHOW);
        SetForegroundWindow(s.hwnd);
        if (!view.reopen) {
            const auto t0 = std::chrono::steady_clock::now();
            constexpr fluent::MotionSpec slide{ 170.0f, fluent::EasingCurve::OutCubic };
            for (;;) {
                const float elapsed = static_cast<float>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count());
                if (elapsed >= slide.duration_ms) break;
                const float p = fluent::EvaluateMotion(slide, elapsed);
                const int dy = -static_cast<int>(
                    std::lround((1.0f - p) * fluent::motion::MenuTravelDip * s.scale));
                const BYTE alpha = static_cast<BYTE>(
                    std::clamp(elapsed / 120.0f, 0.0f, 1.0f) * 255.0f);
                Present(s, alpha, dy);
                Sleep(10);
            }
        }
        Present(s, 255, 0);
    } else {
        ShowWindow(s.hwnd, SW_SHOW);
        SetForegroundWindow(s.hwnd);
    }
    SetFocus(s.hwnd);

    MSG msg{};
    while (!s.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        const bool click = msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN ||
                           msg.message == WM_MBUTTONDOWN || msg.message == WM_NCLBUTTONDOWN ||
                           msg.message == WM_NCRBUTTONDOWN || msg.message == WM_NCMBUTTONDOWN;
        // A click elsewhere keeps the draft (light dismiss); only Esc drops it.
        if (click && msg.hwnd != s.hwnd) Finish(s, ColorRuleAction::Closed);
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    const ColorRuleAction result = s.result;
    view.focus = s.focus;
    view.anchor = s.focus;
    view.scroll = s.scroll / s.scale;
    DestroyWindow(s.hwnd);
    DestroySurface(s);
    s.painter.SetCompositor(nullptr);
    return result;
}

bool ColorRulePopup::SaveDebugSnapshot(Compositor* compositor, const wchar_t* png_path,
                                      bool dark, D2D1_COLOR_F accent, float scale,
                                      const ColorRuleText& text,
                                      const std::vector<ColorRuleRow>& rows,
                                      const ColorRuleView& view) {
    if (!compositor || !compositor->Dc() || !png_path) return false;
    std::vector<ColorRuleRow> copy = rows;
    ColorRuleView view_copy = view;
    State s;
    Init(s, compositor, scale, MakeTheme(dark, accent), text, copy, view_copy, 100);
    s.hot = { Part::Row, RowCount(s) > 1 ? 1 : -1 };
    const bool ok = Render(s) && SavePng(png_path, s.win_w, s.win_h, s.bits);
    DestroySurface(s);
    s.painter.SetCompositor(nullptr);
    return ok;
}

} // namespace pulse::ui
