#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "ai.h"
#include "curve.h"
#include "render.h"
#include "ui_style.h"

namespace fg {

// ---------------- 基础 UI ----------------

namespace {
// 输入框的字符白名单（逐字符输入与剪贴板粘贴共用一份判定：两处行为不会漂移）
bool box_accepts_char(const TextBox& box, int cp) {
    if (box.digits_only) {
        return (cp >= '0' && cp <= '9') || cp == '.' || cp == '-' || cp == '+' ||
               (box.allow_e && cp == 'e');
    }
    if (box.expr_chars_only) {
        return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') ||
               (cp >= 'A' && cp <= 'Z') || cp == '_' || cp == '+' || cp == '-' ||
               cp == '*' || cp == '/' || cp == '^' || cp == '(' || cp == ')' ||
               cp == '.' || cp == ',' || cp == ' ';
    }
    return true;
}
}  // namespace

void TextBox::handle_input() {
    // 失焦期间光标归位到尾部：重新聚焦时从文本末尾继续输入
    //（点框内任意处定位光标暂不提供，点击只负责聚焦）
    if (!focused) {
        cursor = text.size();
        anchor = cursor;
        return;
    }
    // 外部直接改过 text（脚本赋值/回填）时防越界、并保证落在字符边界上
    if (cursor > text.size()) cursor = text.size();
    while (cursor > 0 && (static_cast<unsigned char>(text[cursor]) & 0xC0) == 0x80) --cursor;
    if (anchor > text.size()) anchor = text.size();
    while (anchor > 0 && (static_cast<unsigned char>(text[anchor]) & 0xC0) == 0x80) --anchor;
    // 按字节回退到前一个字符起点 / 前进到下一个字符起点（中文整字跳跃）
    auto prev_boundary = [&](size_t i) {
        if (i == 0) return static_cast<size_t>(0);
        --i;
        while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) --i;
        return i;
    };
    auto next_boundary = [&](size_t i) {
        const size_t n = text.size();
        if (i >= n) return n;
        ++i;
        while (i < n && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) ++i;
        return i;
    };
    // 当前选择（anchor 与 cursor 的区间，恒为字符边界）；anchor == cursor 即无选择
    const size_t sel_lo = std::min(anchor, cursor);
    const size_t sel_hi = std::max(anchor, cursor);
    const bool has_sel = sel_lo != sel_hi;
    // 输入/粘贴/退格遇到选择时都表现为"替换整个选择"
    auto drop_selection = [&]() {
        if (sel_lo == sel_hi) return;
        text.erase(sel_lo, sel_hi - sel_lo);
        cursor = sel_lo;
        anchor = cursor;
    };

    // 剪贴板快捷键（按下即动作、不连发）：全选 / 复制 / 粘贴。
    // Ctrl+C 用按下瞬间的选择（即时计算），不受上方快照影响
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl && IsKeyPressed(KEY_A)) {
        anchor = 0;
        cursor = text.size();
    }
    if (ctrl && IsKeyPressed(KEY_C)) {
        const size_t lo = std::min(anchor, cursor);
        const size_t hi = std::max(anchor, cursor);
        if (lo != hi) SetClipboardText(text.substr(lo, hi - lo).c_str());
    }
    if (ctrl && IsKeyPressed(KEY_V)) {
        const char* clip = GetClipboardText();
        if (clip != nullptr && clip[0] != '\0') {
            if (sel_lo != sel_hi) {  // 有选择：粘贴先替换掉它
                text.erase(sel_lo, sel_hi - sel_lo);
                cursor = sel_lo;
                anchor = cursor;
            }
            // 逐字符过滤后插入：控制字符（换行/制表）与框的字符白名单都要过——
            // 粘贴是外来内容，不能绕过输入框自己的限制
            const char* pstr = clip;
            int cp = 0;
            int csize = 0;
            while ((cp = GetCodepointNext(pstr, &csize)) > 0 && csize > 0) {
                pstr += csize;
                if (cp < 32 || !box_accepts_char(*this, cp)) continue;
                int nsize = 0;
                const char* utf8 = CodepointToUTF8(cp, &nsize);
                if (utf8 != nullptr && nsize > 0 &&
                    text.size() + static_cast<size_t>(nsize) <= max_len) {
                    text.insert(cursor, utf8, static_cast<size_t>(nsize));
                    cursor += static_cast<size_t>(nsize);
                }
            }
            anchor = cursor;
        }
    }

    // 光标移动（首发 + 按住连发都按一个字符一步）；有选择时方向键跳到选择两端（标准行为）
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) {
        cursor = has_sel ? sel_lo : prev_boundary(cursor);
        anchor = cursor;
    }
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) {
        cursor = has_sel ? sel_hi : next_boundary(cursor);
        anchor = cursor;
    }

    int c = 0;
    while ((c = GetCharPressed()) > 0) {
        if (!box_accepts_char(*this, c)) continue;
        drop_selection();  // 有选择时：输入即替换
        // GetCharPressed 给的是 Unicode 码点：转成 UTF-8 再插入到光标处，
        // 直接把码点塞进 char 会把中文等非 ASCII 输入截成乱码
        int size = 0;
        const char* utf8 = CodepointToUTF8(c, &size);
        if (utf8 && size > 0 && text.size() + static_cast<size_t>(size) <= max_len) {
            text.insert(cursor, utf8, static_cast<size_t>(size));
            cursor += static_cast<size_t>(size);
        }
        anchor = cursor;
    }
    // 退格删光标左侧整字（有选择则整个替换掉）；Delete 同理删右侧
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (has_sel) {
            drop_selection();
        } else if (cursor > 0) {
            const size_t pb = prev_boundary(cursor);
            text.erase(pb, cursor - pb);
            cursor = pb;
            anchor = cursor;
        }
    }
    if (IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE)) {
        if (has_sel) drop_selection();
        else if (cursor < text.size()) text.erase(cursor, next_boundary(cursor) - cursor);
    }
}

void App::clear_text_focus() {
    expr_a_.focused = expr_b_.focused = p_start_.focused = p_end_.focused = false;
    host_box_.focused = name_box_.focused = seed_box_.focused = false;
    create_seed_box_.focused = room_seed_box_.focused = false;
    create_password_box_.focused = join_password_box_.focused = false;
    angle_box_.focused = false;  // 漏掉任何一个输入框都会令两个框同时吃键盘
}

void App::set_status(const std::string& s) {
    status_ = s;
    ensure_glyphs(status_);
}

void App::text(const std::string& s, float x, float y, float size, Color c) {
    // 缺字形就地补登记：界面文案不靠 kUiTexts 手工同步兜底——漏登记的字符下一帧
    // 图集重建后正常显示（预载过的不会闪，没预载的只闪一帧占位符）
    ensure_glyphs(s);
    // 取最近的档位字体并按档位基字号绘制（缩放=1，笔画才清晰）；字体加载失败时
    // pick_tier_font 回退默认字体并给回请求字号（默认位图字体没有档位概念）
    const auto [font, fs] = pick_tier_font(&fonts_, size);
    DrawTextEx(font, s.c_str(), Vector2{x, y}, fs, 1.0f, c);
}

float App::text_width(const std::string& s, float size) {
    ensure_glyphs(s);  // 与 text 用同一字形集，先量宽后绘制宽度才不会变
    const auto [font, fs] = pick_tier_font(&fonts_, size);  // 与 text 同一档，量宽=绘制
    return MeasureTextEx(font, s.c_str(), fs, 1.0f).x;
}

// 圆角按钮：悬停变亮、选中填充强调色。
// enabled=false（如"只有房主可点"的按钮对非房主）时画禁用样式，且不响应、不消费点击
bool App::ui_button(Rectangle r, const std::string& label, bool selected, bool enabled) {
    const bool hover = enabled && CheckCollisionPointRec(GetMousePosition(), r);
    const bool pressed = hover && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    Color bg = kPanelSoft;
    if (selected) bg = kAccent;
    else if (pressed) bg = ColorLerp(kAccent, BLACK, 0.35f);
    else if (hover) {
        // 悬停呼吸：背景从面板色向文字色微调（深色主题提亮、浅色主题微沉），随正弦
        // 起伏——基准跟随主题，才能与 kTextHover 始终保持足够对比
        const float p = 0.5f + 0.5f * std::sin(static_cast<float>(GetTime()) * 6.0f);
        bg = ColorLerp(kPanelSoft, kTextMain, 0.06f + 0.08f * p);
    }

    DrawRectangleRounded(r, 0.35f, 8, enabled ? bg : kPanelSoft);
    DrawRectangleRoundedLines(r, 0.35f, 8,
                              selected ? ColorLerp(kAccent, WHITE, 0.35f) : kPanelBorder);
    const float fs = 18.0f;
    const float tw = text_width(label, fs);
    const Color fg = !enabled ? kTextFaint
                              : (selected ? kOnAccent : (hover ? kTextHover : kTextMain));
    text(label, r.x + (r.width - tw) * 0.5f, r.y + (r.height - fs) * 0.5f - 1.0f, fs, fg);
    // 帮助面板（模态）打开时按钮只画不响应，也不消费点击（让点击走"点空白"逻辑）
    const bool clicked =
        enabled && hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !help_modal_active();
    if (clicked) ui_click_consumed_ = true;  // 点击被按钮消费：本帧不清输入框焦点
    return clicked;
}

// 圆角输入框：聚焦时边框发光
void App::ui_textbox(TextBox& box, Rectangle r, const std::string& hint, bool enabled) {
    const bool hover = CheckCollisionPointRec(GetMousePosition(), r);
    // 禁用（如非房主看"固定种子"框）：不可点击聚焦。输入框本就"拿到焦点才吃键"，
    // 点不进即可，无需在输入处另行拦截
    if (enabled && hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !help_modal_active()) {
        ui_click_consumed_ = true;
        // 点击任意处先取消全部焦点，再聚焦本框（漏掉任何一个都会令两个框同时吃键盘）
        clear_text_focus();
        box.focused = true;
    }
    const Color border =
        box.focused ? kAccent : (hover ? ColorLerp(kPanelBorder, kTextMain, 0.3f) : kPanelBorder);
    DrawRectangleRounded(r, 0.28f, 8, kInputBg);
    DrawRectangleRoundedLines(r, 0.28f, 8, border);

    const std::string shown = box.text.empty() ? hint : box.text;
    const Color c = box.text.empty() ? kTextFaint : kTextMain;
    const float fs = 19.0f;
    // 滚动显示跟随光标：光标越过右缘时文本左移，回到左半区即恢复左侧显示
    //（无状态的简化滚动：文本短于视宽时永不滚动）
    const size_t cur = std::min(box.cursor, shown.size());
    const float cursor_w = text_width(shown.substr(0, cur), fs);
    const float limit = r.width - 20.0f;
    float x = r.x + 10.0f;
    if (cursor_w > limit) x -= (cursor_w - limit);
    BeginScissorMode(static_cast<int>(r.x + 4), static_cast<int>(r.y),
                     static_cast<int>(r.width - 8), static_cast<int>(r.height));
    // 选择高亮（Ctrl+A 全选后可见）：垫在文字下层；无选择时不画
    if (box.focused && box.anchor != box.cursor) {
        const size_t lo = std::min(std::min(box.anchor, box.cursor), shown.size());
        const size_t hi = std::min(std::max(box.anchor, box.cursor), shown.size());
        const float w1 = text_width(shown.substr(0, lo), fs);
        const float w2 = text_width(shown.substr(0, hi), fs);
        DrawRectangle(static_cast<int>(x + w1), static_cast<int>(r.y + 5),
                      static_cast<int>(w2 - w1), static_cast<int>(r.height - 10),
                      Fade(kAccent, 0.35f));
    }
    text(shown, x, r.y + (r.height - fs) * 0.5f - 1.0f, fs, c);
    // 光标画在字符边界处（hint 灰字期间光标停在起点——空文本的真实光标不在提示字后面）
    if (box.focused && std::fmod(GetTime(), 1.0) < 0.5) {
        const float cx = x + cursor_w + 1.0f;
        DrawRectangle(static_cast<int>(cx), static_cast<int>(r.y + 7), 2,
                      static_cast<int>(r.height - 14), kAccent);
    }
    EndScissorMode();
}

void App::add_codepoints(const std::string& s) {
    const char* p = s.c_str();
    int size = 0;
    int cp = 0;
    while ((cp = GetCodepointNext(p, &size)) > 0 && size > 0) {
        p += size;
        if (cp < 128) continue;  // ASCII 固定包含
        // 上限兜底：异常长的文本（异常服务器送来的超长名字等）不应无限撑大字形图集
        if (glyph_cps_.size() >= 4096) return;
        const auto it = std::lower_bound(glyph_cps_.begin(), glyph_cps_.end(), cp);
        if (it == glyph_cps_.end() || *it != cp) glyph_cps_.insert(it, cp);
    }
}

void App::reload_font() {
    // 等线/黑体是 .ttf，微软雅黑/宋体是 .ttc（raylib 对 ttc 的兼容不稳），ttf 优先
    const char* paths[] = {"C:/Windows/Fonts/Deng.ttf", "C:/Windows/Fonts/simhei.ttf",
                           "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simsun.ttc"};
    const unsigned int default_tex = GetFontDefault().texture.id;
    // 每个档位（kFontTiers）单独加载一份字形图集：绘制尺寸==档位基字号、缩放=1，
    // 小字不再是 56px 大图集的缩水版，笔画才清晰
    std::map<int, Font> built;
    for (const char* path : paths) {
        if (!FileExists(path)) continue;
        bool ok = true;
        for (const int tier : kFontTiers) {
            Font f = LoadFontEx(path, tier, glyph_cps_.data(), static_cast<int>(glyph_cps_.size()));
            // LoadFontEx 失败时会返回默认字体，仅凭 glyphCount 判断不出来；
            // 以"能否找到常用汉字'中'的字形"为准
            if (f.texture.id == default_tex || GetGlyphIndex(f, 0x4E2D) == 0) {
                if (f.texture.id != default_tex) UnloadFont(f);
                ok = false;
                break;
            }
            built[tier] = f;
        }
        if (ok) break;
        for (auto& [tier, f] : built) UnloadFont(f);
        built.clear();
    }
    // 原地换代：先建后删，绘制中途不会引用到已卸载的图集
    for (auto& [tier, f] : fonts_) UnloadFont(f);
    fonts_ = std::move(built);
    font_is_default_ = fonts_.empty();  // 全部候选失败：回退默认字体（中文显示为占位符）
}

void App::load_ui_font() {
    glyph_cps_.clear();
    for (int c = 32; c < 127; ++c) glyph_cps_.push_back(c);
    for (const char* s : kUiTexts) add_codepoints(s);
    reload_font();
}

void App::ensure_glyphs(const std::string& s) {
    if (font_is_default_) return;
    const size_t before = glyph_cps_.size();
    add_codepoints(s);
    if (glyph_cps_.size() != before) glyph_dirty_ = true;
}

bool App::help_modal_active() const { return screen_ == Screen::Battle && help_open_; }

void App::flush_glyph_reload() {
    // 一帧内收集的所有新字形只重建一次图集：一条 room_state / game_start 会带来
    // 整队（甚至全场）玩家名，逐个立即重建等于同一帧里把整张图集反复重做
    if (!glyph_dirty_) return;
    glyph_dirty_ = false;
    reload_font();
}
}  // namespace fg
