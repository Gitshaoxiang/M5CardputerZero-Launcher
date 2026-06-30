#include "lvgl/lvgl.h"

#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <linux/input.h>
#include <string>

namespace {

enum class EditField {
    Hour = 0,
    Minute,
    Second,
    Year,
    Month,
    Day,
};

enum class ClockTheme : uint8_t {
    Flip = 0,
    Aura,
    Strap,
    Mono,
};

struct ThemePalette {
    lv_color_t normal_time;
    lv_color_t second_time;
    lv_color_t normal_aux;
    lv_color_t selected;
    lv_color_t card_normal;
    lv_color_t card_selected;
    lv_color_t card_border;
    lv_color_t card_border_selected;
    lv_color_t second_card_normal;
    lv_color_t second_card_selected;
    lv_color_t second_border;
    lv_color_t second_border_selected;
    lv_color_t progress_fill;
    lv_color_t progress_head;
    lv_color_t progress_dim;
    lv_color_t progress_glow;
    lv_color_t footer;
};

static constexpr uint32_t kUiTickMs = 250;
static constexpr lv_opa_t kOpaSoft = (lv_opa_t)32;
static constexpr lv_opa_t kOpaFaint = (lv_opa_t)26;
static constexpr lv_opa_t kOpaGlow = (lv_opa_t)40;
static constexpr int32_t kFlipAnimDistance = 8;
static constexpr uint32_t kFlipAnimDuration = 110;
static constexpr size_t kSecondCellCount = 60;
static constexpr std::array<EditField, 6> kEditOrder = {
    EditField::Hour,
    EditField::Minute,
    EditField::Second,
    EditField::Year,
    EditField::Month,
    EditField::Day,
};

static lv_group_t *g_group = nullptr;
static lv_timer_t *g_ui_timer = nullptr;
static lv_obj_t *g_screen = nullptr;
static std::array<lv_obj_t *, kSecondCellCount> g_second_cells{};
static lv_obj_t *g_time_box = nullptr;
static lv_obj_t *g_hour_card = nullptr;
static lv_obj_t *g_minute_card = nullptr;
static lv_obj_t *g_second_card = nullptr;
static lv_obj_t *g_hour_label = nullptr;
static lv_obj_t *g_hour_next_label = nullptr;
static lv_obj_t *g_minute_label = nullptr;
static lv_obj_t *g_minute_next_label = nullptr;
static lv_obj_t *g_colon_label = nullptr;
static lv_obj_t *g_second_colon_label = nullptr;
static lv_obj_t *g_seconds_label = nullptr;
static lv_obj_t *g_seconds_next_label = nullptr;
static lv_obj_t *g_date_label = nullptr;
static lv_obj_t *g_footer_label = nullptr;
static lv_obj_t *g_theme_label = nullptr;
static lv_obj_t *g_hour_hand = nullptr;
static lv_obj_t *g_minute_hand = nullptr;
static lv_obj_t *g_second_hand = nullptr;
static lv_obj_t *g_theme_motion_bar = nullptr;
static lv_obj_t *g_theme_motion_head = nullptr;
static lv_obj_t *g_theme_motion_block = nullptr;

static int g_screen_w = 320;
static int g_screen_h = 170;
static time_t g_last_time_value = static_cast<time_t>(-1);
static bool g_edit_mode = false;
static size_t g_selected_field = 0;
static long long g_time_offset_seconds = 0;
static int g_debug_clock_log_count = 0;
static bool g_debug_enabled = false;
static ClockTheme g_theme = ClockTheme::Flip;
static char g_last_hour_text[3] = "";
static char g_last_minute_text[3] = "";
static char g_last_second_text[3] = "";

static void update_clock_labels(bool force);
static constexpr double kPi = 3.14159265358979323846;

static void debug_log(const char *fmt, ...)
{
    if (!g_debug_enabled)
    {
        return;
    }

    va_list args;
    va_start(args, fmt);
    std::fputs("[RTC] ", stderr);
    std::vfprintf(stderr, fmt, args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    va_end(args);
}

static constexpr int theme_count()
{
    return 4;
}

static bool theme_uses_flip()
{
    return g_theme == ClockTheme::Flip;
}

static const char *theme_name(ClockTheme theme)
{
    switch (theme)
    {
    case ClockTheme::Flip: return "FLIP";
    case ClockTheme::Aura: return "DIAL";
    case ClockTheme::Strap: return "DASH";
    case ClockTheme::Mono: return "POSTER";
    }
    return "RTC";
}

static const char *field_name(EditField field)
{
    switch (field)
    {
    case EditField::Hour: return "HR";
    case EditField::Minute: return "MIN";
    case EditField::Second: return "SEC";
    case EditField::Year: return "YR";
    case EditField::Month: return "MON";
    case EditField::Day: return "DAY";
    }
    return "TIME";
}

static ThemePalette current_palette()
{
    switch (g_theme)
    {
    case ClockTheme::Flip:
        return {
            lv_color_hex(0xF8FBFF), lv_color_hex(0xD8F2FF), lv_color_hex(0xB9D4F2), lv_color_hex(0x7FF6C7),
            lv_color_hex(0x0C1826), lv_color_hex(0x11273A), lv_color_hex(0x22384E), lv_color_hex(0x7FF6C7),
            lv_color_hex(0x102131), lv_color_hex(0x11273A), lv_color_hex(0x2B445C), lv_color_hex(0x7FF6C7),
            lv_color_hex(0x3F98FF), lv_color_hex(0xA8FFF0), lv_color_hex(0x21405F), lv_color_hex(0x8AE8FF),
            lv_color_hex(0x89A7C4),
        };
    case ClockTheme::Aura:
        return {
            lv_color_hex(0xF7FCFF), lv_color_hex(0xEAFBFF), lv_color_hex(0xBED6E7), lv_color_hex(0x7CF7D8),
            lv_color_hex(0x10253A), lv_color_hex(0x15344D), lv_color_hex(0x486D8F), lv_color_hex(0x7CF7D8),
            lv_color_hex(0x153046), lv_color_hex(0x1A3850), lv_color_hex(0x5A86A8), lv_color_hex(0x7CF7D8),
            lv_color_hex(0x6AB8FF), lv_color_hex(0xF6F3A0), lv_color_hex(0x2A4868), lv_color_hex(0xA7F7FF),
            lv_color_hex(0x95B7CE),
        };
    case ClockTheme::Strap:
        return {
            lv_color_hex(0xF8F2EB), lv_color_hex(0xF2D9B8), lv_color_hex(0xB49C8D), lv_color_hex(0xFFD36C),
            lv_color_hex(0x171313), lv_color_hex(0x221D1B), lv_color_hex(0x4F433D), lv_color_hex(0xFFD36C),
            lv_color_hex(0x1D1818), lv_color_hex(0x282120), lv_color_hex(0x6A584E), lv_color_hex(0xFFD36C),
            lv_color_hex(0xD98241), lv_color_hex(0xFFF0BF), lv_color_hex(0x443832), lv_color_hex(0xFFD08A),
            lv_color_hex(0xB79E8A),
        };
    case ClockTheme::Mono:
        return {
            lv_color_hex(0x181513), lv_color_hex(0xAF553E), lv_color_hex(0x6F655E), lv_color_hex(0xD45F42),
            lv_color_hex(0xEFE7DB), lv_color_hex(0xF4ECE2), lv_color_hex(0xD5C7B7), lv_color_hex(0xD45F42),
            lv_color_hex(0xF1EAE1), lv_color_hex(0xF6EFE7), lv_color_hex(0xD5C7B7), lv_color_hex(0xD45F42),
            lv_color_hex(0xC86043), lv_color_hex(0x1E1A18), lv_color_hex(0xD7CDC1), lv_color_hex(0xE08167),
            lv_color_hex(0x746A63),
        };
    }

    return {
        lv_color_white(), lv_color_white(), lv_color_white(), lv_color_white(),
        lv_color_black(), lv_color_black(), lv_color_black(), lv_color_white(),
        lv_color_black(), lv_color_black(), lv_color_black(), lv_color_white(),
        lv_color_white(), lv_color_white(), lv_color_black(), lv_color_white(),
        lv_color_white(),
    };
}

static lv_obj_t *create_card(lv_obj_t *parent,
                             lv_coord_t x,
                             lv_coord_t y,
                             lv_coord_t w,
                             lv_coord_t h,
                             lv_color_t bg,
                             lv_color_t border)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, bg, 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_grad_color(card, bg, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, border, 0);
    lv_obj_set_style_shadow_width(card, 18, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(card, border, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    return card;
}

static lv_obj_t *create_plain_block(lv_obj_t *parent,
                                    lv_coord_t x,
                                    lv_coord_t y,
                                    lv_coord_t w,
                                    lv_coord_t h,
                                    lv_color_t color,
                                    lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    return obj;
}

static lv_obj_t *create_disc(lv_obj_t *parent,
                             lv_coord_t x,
                             lv_coord_t y,
                             lv_coord_t size,
                             lv_color_t color,
                             lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, size, size);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    return obj;
}

static lv_obj_t *create_text(lv_obj_t *parent,
                             const char *text,
                             const lv_font_t *font,
                             lv_color_t color,
                             lv_coord_t x,
                             lv_coord_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

static lv_obj_t *create_hand(lv_obj_t *parent,
                             lv_coord_t center_x,
                             lv_coord_t center_y,
                             lv_coord_t length,
                             lv_coord_t thickness,
                             lv_coord_t tail,
                             lv_color_t color,
                             lv_opa_t opa)
{
    lv_obj_t *hand = lv_obj_create(parent);
    lv_obj_remove_style_all(hand);
    lv_obj_set_pos(hand, center_x - (thickness / 2), center_y - length + tail);
    lv_obj_set_size(hand, thickness, length);
    lv_obj_set_style_radius(hand, thickness / 2, 0);
    lv_obj_set_style_bg_color(hand, color, 0);
    lv_obj_set_style_bg_opa(hand, opa, 0);
    lv_obj_set_style_transform_pivot_x(hand, thickness / 2, 0);
    lv_obj_set_style_transform_pivot_y(hand, length - tail, 0);
    lv_obj_clear_flag(hand, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(hand, LV_SCROLLBAR_MODE_OFF);
    return hand;
}

static void set_hand_angle(lv_obj_t *hand, int degrees_tenths)
{
    if (hand == nullptr)
    {
        return;
    }
    lv_obj_set_style_transform_rotation(hand, degrees_tenths, 0);
}

static void create_dial_ticks(lv_obj_t *parent,
                              lv_coord_t cx,
                              lv_coord_t cy,
                              lv_coord_t radius)
{
    for (int i = 0; i < 12; ++i)
    {
        const double angle = ((i * 30.0) - 90.0) * (kPi / 180.0);
        const int outer_x = static_cast<int>(std::lround(cx + std::cos(angle) * radius));
        const int outer_y = static_cast<int>(std::lround(cy + std::sin(angle) * radius));
        const int inner_x = static_cast<int>(std::lround(cx + std::cos(angle) * (radius - (i % 3 == 0 ? 12 : 7))));
        const int inner_y = static_cast<int>(std::lround(cy + std::sin(angle) * (radius - (i % 3 == 0 ? 12 : 7))));
        const lv_coord_t w = (i % 3 == 0) ? 3 : 2;
        const lv_coord_t h = (i % 3 == 0) ? 12 : 7;
        create_plain_block(parent,
                           inner_x - (w / 2),
                           inner_y - (h / 2),
                           w,
                           h,
                           i % 3 == 0 ? lv_color_hex(0xEAC78D) : lv_color_hex(0x4A6E8E),
                           i % 3 == 0 ? LV_OPA_80 : LV_OPA_50);
        create_disc(parent, outer_x - 1, outer_y - 1, 2, lv_color_hex(0x27445F), LV_OPA_50);
    }
}

static time_t display_time_now()
{
    return std::time(nullptr) + static_cast<time_t>(g_time_offset_seconds);
}

static std::string format_delta(long long total_seconds)
{
    char text[32];
    const long long abs_seconds = total_seconds < 0 ? -total_seconds : total_seconds;
    const long long hours = abs_seconds / 3600;
    const long long minutes = (abs_seconds / 60) % 60;
    const long long seconds = abs_seconds % 60;
    std::snprintf(text,
                  sizeof(text),
                  "%c%02lld:%02lld:%02lld",
                  total_seconds < 0 ? '-' : '+',
                  hours,
                  minutes,
                  seconds);
    return text;
}

static void reset_ui_refs()
{
    g_second_cells.fill(nullptr);
    g_time_box = nullptr;
    g_hour_card = nullptr;
    g_minute_card = nullptr;
    g_second_card = nullptr;
    g_hour_label = nullptr;
    g_hour_next_label = nullptr;
    g_minute_label = nullptr;
    g_minute_next_label = nullptr;
    g_colon_label = nullptr;
    g_second_colon_label = nullptr;
    g_seconds_label = nullptr;
    g_seconds_next_label = nullptr;
    g_date_label = nullptr;
    g_footer_label = nullptr;
    g_theme_label = nullptr;
    g_hour_hand = nullptr;
    g_minute_hand = nullptr;
    g_second_hand = nullptr;
    g_theme_motion_bar = nullptr;
    g_theme_motion_head = nullptr;
    g_theme_motion_block = nullptr;
}

static void create_glow(lv_obj_t *parent,
                        lv_coord_t x,
                        lv_coord_t y,
                        lv_coord_t size,
                        lv_color_t color,
                        lv_opa_t opa,
                        lv_coord_t shadow)
{
    lv_obj_t *glow = lv_obj_create(parent);
    lv_obj_remove_style_all(glow);
    lv_obj_set_pos(glow, x, y);
    lv_obj_set_size(glow, size, size);
    lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(glow, opa, 0);
    lv_obj_set_style_bg_color(glow, color, 0);
    lv_obj_set_style_shadow_width(glow, shadow, 0);
    lv_obj_set_style_shadow_opa(glow, opa, 0);
    lv_obj_set_style_shadow_color(glow, color, 0);
}

static void create_scanlines(lv_obj_t *parent, lv_color_t color)
{
    for (int y = 0; y < g_screen_h; y += 6)
    {
        create_plain_block(parent, 0, y, g_screen_w, 1, color, (lv_opa_t)10);
    }
}

static void create_second_row(lv_obj_t *screen, lv_coord_t y, lv_coord_t row_h, lv_coord_t cell_h)
{
    lv_obj_t *second_row = lv_obj_create(screen);
    lv_obj_remove_style_all(second_row);
    lv_obj_set_pos(second_row, 10, y);
    lv_obj_set_size(second_row, g_screen_w - 20, row_h);
    lv_obj_set_layout(second_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(second_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(second_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(second_row, 1, 0);
    lv_obj_clear_flag(second_row, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < g_second_cells.size(); ++i)
    {
        lv_obj_t *cell = lv_obj_create(second_row);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, 4, cell_h);
        lv_obj_set_style_radius(cell, 2, 0);
        g_second_cells[i] = cell;
    }
}

static void create_title_area(lv_obj_t *screen,
                              lv_color_t title_color,
                              lv_color_t badge_color,
                              lv_coord_t y,
                              lv_coord_t badge_w)
{
    lv_obj_t *title_label = create_text(screen,
                                        "RTC RX8130CE",
                                        &lv_font_montserrat_12,
                                        title_color,
                                        12,
                                        y);
    lv_obj_set_width(title_label, 132);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_CLIP);
    lv_obj_move_foreground(title_label);

    g_theme_label = create_text(screen,
                                theme_name(g_theme),
                                &lv_font_montserrat_12,
                                badge_color,
                                g_screen_w - badge_w - 12,
                                y);
    lv_obj_set_width(g_theme_label, badge_w);
    lv_obj_set_style_text_align(g_theme_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(g_theme_label, LV_LABEL_LONG_CLIP);
    lv_obj_move_foreground(g_theme_label);
}

static void build_theme_flip()
{
    create_glow(g_screen, -42, 28, 120, lv_color_hex(0x22B8FF), kOpaSoft, 28);
    create_glow(g_screen, g_screen_w - 52, 12, 86, lv_color_hex(0x64F4C5), kOpaFaint, 24);
    create_second_row(g_screen, 6, 7, 5);
    create_title_area(g_screen, lv_color_hex(0xDDEBFA), lv_color_hex(0x98C8FF), 21, 66);

    const lv_coord_t hero_x = 8;
    const lv_coord_t hero_y = 40;
    const lv_coord_t hero_w = g_screen_w - 16;
    const lv_coord_t hero_h = 90;
    const lv_coord_t time_box_x = 12;
    const lv_coord_t time_box_y = 13;
    const lv_coord_t time_box_w = hero_w - 24;
    const lv_coord_t time_box_h = 64;
    const lv_coord_t flip_card_w = 82;
    const lv_coord_t flip_card_h = 60;
    const lv_coord_t flip_card_y = (time_box_h - flip_card_h) / 2;
    const lv_coord_t colon_w = 10;
    const lv_coord_t flip_gap = 2;
    const lv_coord_t group_w = (flip_card_w * 3) + (colon_w * 2) + (flip_gap * 4);
    const lv_coord_t flip_left_x = (time_box_w - group_w) / 2;
    const lv_coord_t colon_x = flip_left_x + flip_card_w + flip_gap;
    const lv_coord_t minute_x = colon_x + colon_w + flip_gap;
    const lv_coord_t second_colon_x = minute_x + flip_card_w + flip_gap;
    const lv_coord_t second_x = second_colon_x + colon_w + flip_gap;

    lv_obj_t *hero = create_card(g_screen,
                                 hero_x,
                                 hero_y,
                                 hero_w,
                                 hero_h,
                                 lv_color_hex(0x08111E),
                                 lv_color_hex(0x2B4F79));

    g_time_box = create_card(hero,
                             time_box_x,
                             time_box_y,
                             time_box_w,
                             time_box_h,
                             lv_color_hex(0x09131F),
                             lv_color_hex(0x355D88));
    lv_obj_set_style_radius(g_time_box, 12, 0);
    lv_obj_set_style_bg_grad_color(g_time_box, lv_color_hex(0x09131F), 0);
    lv_obj_set_style_border_width(g_time_box, 0, 0);
    lv_obj_set_style_shadow_width(g_time_box, 0, 0);

    lv_obj_t *flip_top = create_plain_block(g_time_box, 0, 0, time_box_w, time_box_h / 2, lv_color_hex(0x132335), LV_OPA_COVER);
    lv_obj_set_style_radius(flip_top, 12, 0);
    lv_obj_t *flip_bottom = create_plain_block(g_time_box, 0, time_box_h / 2, time_box_w, time_box_h - time_box_h / 2, lv_color_hex(0x0D1825), LV_OPA_COVER);
    lv_obj_set_style_radius(flip_bottom, 12, 0);
    lv_obj_t *hinge = create_plain_block(g_time_box, 12, (time_box_h / 2) - 1, time_box_w - 24, 2, lv_color_hex(0x304C69), LV_OPA_50);
    lv_obj_move_foreground(hinge);

    g_hour_card = create_card(g_time_box, flip_left_x, flip_card_y, flip_card_w, flip_card_h, lv_color_hex(0x0C1826), lv_color_hex(0x22384E));
    g_minute_card = create_card(g_time_box, minute_x, flip_card_y, flip_card_w, flip_card_h, lv_color_hex(0x0C1826), lv_color_hex(0x22384E));
    g_second_card = create_card(g_time_box, second_x, flip_card_y, flip_card_w, flip_card_h, lv_color_hex(0x102131), lv_color_hex(0x2B445C));

    for (lv_obj_t *card : {g_hour_card, g_minute_card})
    {
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_t *top = create_plain_block(card, 0, 0, flip_card_w, flip_card_h / 2, lv_color_hex(0x152637), LV_OPA_COVER);
        lv_obj_set_style_radius(top, 10, 0);
        lv_obj_t *bottom = create_plain_block(card, 0, flip_card_h / 2, flip_card_w, flip_card_h - flip_card_h / 2, lv_color_hex(0x0D1825), LV_OPA_COVER);
        lv_obj_set_style_radius(bottom, 10, 0);
        lv_obj_t *card_hinge = create_plain_block(card, 8, (flip_card_h / 2) - 1, flip_card_w - 16, 2, lv_color_hex(0x35506B), LV_OPA_50);
        lv_obj_move_foreground(card_hinge);
    }

    lv_obj_set_style_radius(g_second_card, 10, 0);
    lv_obj_set_style_border_width(g_second_card, 0, 0);
    lv_obj_set_style_shadow_width(g_second_card, 0, 0);
    lv_obj_t *second_top = create_plain_block(g_second_card, 0, 0, flip_card_w, flip_card_h / 2, lv_color_hex(0x183044), LV_OPA_COVER);
    lv_obj_set_style_radius(second_top, 10, 0);
    lv_obj_t *second_bottom = create_plain_block(g_second_card, 0, flip_card_h / 2, flip_card_w, flip_card_h - flip_card_h / 2, lv_color_hex(0x102131), LV_OPA_COVER);
    lv_obj_set_style_radius(second_bottom, 10, 0);
    lv_obj_t *second_hinge = create_plain_block(g_second_card, 8, (flip_card_h / 2) - 1, flip_card_w - 16, 2, lv_color_hex(0x40607D), LV_OPA_50);
    lv_obj_move_foreground(second_hinge);

    g_colon_label = create_text(g_time_box, ":", &lv_font_montserrat_48, lv_color_hex(0xDCEBFF), colon_x - 6, -6);
    lv_obj_set_size(g_colon_label, colon_w + 12, flip_card_h);
    lv_obj_set_style_text_align(g_colon_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_colon_label, LV_ALIGN_TOP_LEFT, colon_x - 6, flip_card_y + 1);
    lv_obj_move_foreground(g_colon_label);

    g_second_colon_label = create_text(g_time_box, ":", &lv_font_montserrat_48, lv_color_hex(0xBEE2FF), second_colon_x - 6, -6);
    lv_obj_set_size(g_second_colon_label, colon_w + 12, flip_card_h);
    lv_obj_set_style_text_align(g_second_colon_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_second_colon_label, LV_ALIGN_TOP_LEFT, second_colon_x - 6, flip_card_y + 1);
    lv_obj_move_foreground(g_second_colon_label);

    g_hour_label = create_text(g_hour_card, "00", &lv_font_montserrat_48, lv_color_hex(0xF5FAFF), 0, -2);
    g_hour_next_label = create_text(g_hour_card, "", &lv_font_montserrat_48, lv_color_hex(0xF5FAFF), 0, -2);
    g_minute_label = create_text(g_minute_card, "00", &lv_font_montserrat_48, lv_color_hex(0xF5FAFF), 0, -2);
    g_minute_next_label = create_text(g_minute_card, "", &lv_font_montserrat_48, lv_color_hex(0xF5FAFF), 0, -2);
    g_seconds_label = create_text(g_second_card, "00", &lv_font_montserrat_48, lv_color_hex(0xD8F2FF), 0, -2);
    g_seconds_next_label = create_text(g_second_card, "", &lv_font_montserrat_48, lv_color_hex(0xD8F2FF), 0, -2);

    for (lv_obj_t *label : {g_hour_label, g_hour_next_label, g_minute_label, g_minute_next_label, g_seconds_label, g_seconds_next_label})
    {
        if (label == nullptr)
        {
            continue;
        }
        lv_obj_set_size(label, flip_card_w, flip_card_h);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    }

    lv_obj_align(g_hour_label, LV_ALIGN_CENTER, 0, 6);
    lv_obj_align(g_hour_next_label, LV_ALIGN_CENTER, 0, -4);
    lv_obj_set_style_opa(g_hour_next_label, LV_OPA_TRANSP, 0);
    lv_obj_align(g_minute_label, LV_ALIGN_CENTER, 0, 6);
    lv_obj_align(g_minute_next_label, LV_ALIGN_CENTER, 0, -4);
    lv_obj_set_style_opa(g_minute_next_label, LV_OPA_TRANSP, 0);
    lv_obj_align(g_seconds_label, LV_ALIGN_CENTER, 0, 6);
    lv_obj_align(g_seconds_next_label, LV_ALIGN_CENTER, 0, 6);
    lv_obj_set_style_opa(g_seconds_next_label, LV_OPA_TRANSP, 0);

    lv_obj_move_foreground(g_hour_label);
    lv_obj_move_foreground(g_hour_next_label);
    lv_obj_move_foreground(g_minute_label);
    lv_obj_move_foreground(g_minute_next_label);
    lv_obj_move_foreground(g_seconds_label);
    lv_obj_move_foreground(g_seconds_next_label);

    g_date_label = create_text(g_screen, "0000-00-00", &lv_font_montserrat_16, lv_color_hex(0xB9D4F2), 0, 134);
    lv_obj_set_width(g_date_label, g_screen_w);
    lv_obj_set_style_text_align(g_date_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(g_date_label, 1, 0);
    lv_label_set_long_mode(g_date_label, LV_LABEL_LONG_CLIP);

    g_footer_label = create_text(g_screen, "", &lv_font_montserrat_12, lv_color_hex(0x89A7C4), 12, 154);
    lv_obj_set_width(g_footer_label, g_screen_w - 24);
    lv_obj_set_style_text_align(g_footer_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_footer_label, LV_LABEL_LONG_CLIP);
}

static void build_theme_aura()
{
    create_glow(g_screen, -18, 24, 104, lv_color_hex(0x4ED8FF), (lv_opa_t)20, 22);
    create_glow(g_screen, g_screen_w - 64, 12, 92, lv_color_hex(0xFFCA67), (lv_opa_t)14, 20);
    create_second_row(g_screen, 8, 7, 4);
    create_title_area(g_screen, lv_color_hex(0xE9F5FF), lv_color_hex(0xFFD897), 19, 70);

    lv_obj_t *dial = create_card(g_screen,
                                 20,
                                 28,
                                 116,
                                 116,
                                 lv_color_hex(0x09131C),
                                 lv_color_hex(0x355D7D));
    lv_obj_set_style_radius(dial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_grad_color(dial, lv_color_hex(0x10202E), 0);
    lv_obj_set_style_shadow_width(dial, 18, 0);
    g_time_box = dial;

    create_disc(dial, 6, 6, 104, lv_color_hex(0x0F1B26), LV_OPA_COVER);
    create_disc(dial, 18, 18, 80, lv_color_hex(0x132635), LV_OPA_COVER);
    create_dial_ticks(dial, 58, 58, 46);

    g_hour_hand = create_hand(dial, 58, 58, 34, 6, 8, lv_color_hex(0xF4F7FB), LV_OPA_COVER);
    g_minute_hand = create_hand(dial, 58, 58, 44, 4, 8, lv_color_hex(0x8DDCFF), LV_OPA_COVER);
    g_second_hand = create_hand(dial, 58, 58, 48, 2, 12, lv_color_hex(0xFFD36C), LV_OPA_COVER);
    create_disc(dial, 53, 53, 10, lv_color_hex(0xFFE9A9), LV_OPA_COVER);
    create_disc(dial, 55, 55, 6, lv_color_hex(0x0E1923), LV_OPA_COVER);

    const lv_coord_t digital_card_y = 42;
    const lv_coord_t digital_card_w = 72;
    const lv_coord_t digital_card_h = 52;
    const lv_coord_t hour_card_x = 146;
    const lv_coord_t minute_card_x = 224;

    g_hour_card = create_card(g_screen,
                              hour_card_x,
                              digital_card_y,
                              digital_card_w,
                              digital_card_h,
                              lv_color_hex(0x112433),
                              lv_color_hex(0x486B89));
    g_minute_card = create_card(g_screen,
                                minute_card_x,
                                digital_card_y,
                                digital_card_w,
                                digital_card_h,
                                lv_color_hex(0x112433),
                                lv_color_hex(0x486B89));
    g_second_card = create_card(g_screen, 152, 106, 46, 18, lv_color_hex(0x183047), lv_color_hex(0x5A86A8));
    lv_obj_set_style_radius(g_hour_card, 18, 0);
    lv_obj_set_style_radius(g_minute_card, 18, 0);
    lv_obj_set_style_radius(g_second_card, 12, 0);
    lv_obj_set_style_shadow_width(g_hour_card, 0, 0);
    lv_obj_set_style_shadow_width(g_minute_card, 0, 0);
    lv_obj_set_style_shadow_width(g_second_card, 0, 0);

    lv_obj_t *hour_title = create_text(g_screen, "HR", &lv_font_montserrat_12, lv_color_hex(0x89B7D6), hour_card_x, 30);
    lv_obj_t *minute_title = create_text(g_screen, "MIN", &lv_font_montserrat_12, lv_color_hex(0x89B7D6), minute_card_x, 30);
    lv_obj_set_width(hour_title, digital_card_w);
    lv_obj_set_width(minute_title, digital_card_w);
    lv_obj_set_style_text_align(hour_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(minute_title, LV_TEXT_ALIGN_CENTER, 0);

    g_colon_label = nullptr;
    g_second_colon_label = nullptr;

    g_hour_label = create_text(g_hour_card, "00", &lv_font_montserrat_48, lv_color_hex(0xF7FCFF), 0, -2);
    g_minute_label = create_text(g_minute_card, "00", &lv_font_montserrat_48, lv_color_hex(0xDDF4FF), 0, -2);
    g_seconds_label = create_text(g_second_card, "00", &lv_font_montserrat_12, lv_color_hex(0xFFF3D0), 0, 0);
    lv_obj_set_size(g_hour_label, digital_card_w, digital_card_h);
    lv_obj_set_size(g_minute_label, digital_card_w, digital_card_h);
    lv_obj_set_size(g_seconds_label, 46, 18);
    lv_obj_set_style_text_align(g_hour_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(g_minute_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(g_seconds_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_hour_label, LV_ALIGN_CENTER, 0, 6);
    lv_obj_align(g_minute_label, LV_ALIGN_CENTER, 0, 6);
    lv_obj_align(g_seconds_label, LV_ALIGN_CENTER, 0, 1);

    g_date_label = create_text(g_screen, "0000-00-00", &lv_font_montserrat_12, lv_color_hex(0xC5DCEC), 204, 108);
    lv_obj_set_width(g_date_label, 82);
    lv_obj_set_style_text_align(g_date_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_letter_space(g_date_label, 2, 0);

    g_footer_label = create_text(g_screen, "", &lv_font_montserrat_12, lv_color_hex(0x95B7CE), 12, 154);
    lv_obj_set_width(g_footer_label, g_screen_w - 24);
    lv_obj_set_style_text_align(g_footer_label, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_theme_strap()
{
    create_second_row(g_screen, 8, 7, 4);
    create_title_area(g_screen, lv_color_hex(0xEEE4D8), lv_color_hex(0xE7C98B), 19, 70);

    create_plain_block(g_screen, 18, 50, g_screen_w - 36, 1, lv_color_hex(0x3A3130), LV_OPA_60);
    create_plain_block(g_screen, 18, 126, g_screen_w - 36, 1, lv_color_hex(0x3A3130), LV_OPA_60);

    g_time_box = create_plain_block(g_screen, 18, 54, g_screen_w - 36, 70, lv_color_hex(0x141111), LV_OPA_0);

    const lv_coord_t card_y = 58;
    const lv_coord_t card_h = 54;
    const lv_coord_t card_w = 78;
    const lv_coord_t gap = 18;
    const lv_coord_t colon_w = 16;
    const lv_coord_t total_w = (card_w * 2) + colon_w + gap;
    const lv_coord_t start_x = (g_screen_w - total_w) / 2;
    const lv_coord_t colon_x = start_x + card_w + (gap / 2) - (colon_w / 2);
    const lv_coord_t minute_x = start_x + card_w + gap + colon_w;

    g_hour_card = create_card(g_screen, start_x, card_y, card_w, card_h, lv_color_hex(0x171313), lv_color_hex(0x4B403B));
    g_minute_card = create_card(g_screen, minute_x, card_y, card_w, card_h, lv_color_hex(0x171313), lv_color_hex(0x4B403B));
    g_second_card = create_card(g_screen, 250, 74, 40, 22, lv_color_hex(0x1C1717), lv_color_hex(0x5D4D45));
    for (lv_obj_t *card : {g_hour_card, g_minute_card, g_second_card})
    {
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
    }
    lv_obj_set_style_border_width(g_hour_card, 0, 0);
    lv_obj_set_style_border_width(g_minute_card, 0, 0);
    lv_obj_set_style_border_width(g_second_card, 0, 0);

    g_colon_label = create_text(g_screen, ":", &lv_font_montserrat_48, lv_color_hex(0xE2C8AC), colon_x, 55);
    g_second_colon_label = nullptr;
    lv_obj_set_size(g_colon_label, colon_w, 52);
    lv_obj_set_style_text_align(g_colon_label, LV_TEXT_ALIGN_CENTER, 0);

    g_hour_label = create_text(g_hour_card, "00", &lv_font_montserrat_48, lv_color_hex(0xF8F2EB), 0, -1);
    g_minute_label = create_text(g_minute_card, "00", &lv_font_montserrat_48, lv_color_hex(0xF8F2EB), 0, -1);
    g_seconds_label = create_text(g_second_card, "00", &lv_font_montserrat_16, lv_color_hex(0xF2D9B8), 0, 0);
    lv_obj_set_size(g_hour_label, card_w, card_h);
    lv_obj_set_size(g_minute_label, card_w, card_h);
    lv_obj_set_size(g_seconds_label, 42, 22);
    lv_obj_set_style_text_align(g_hour_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(g_minute_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(g_seconds_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_hour_label, LV_ALIGN_CENTER, 0, 7);
    lv_obj_align(g_minute_label, LV_ALIGN_CENTER, 0, 7);
    lv_obj_align(g_seconds_label, LV_ALIGN_CENTER, 0, 1);

    g_date_label = create_text(g_screen, "0000-00-00", &lv_font_montserrat_16, lv_color_hex(0xB49C8D), 0, 114);
    lv_obj_set_width(g_date_label, g_screen_w);
    lv_obj_set_style_text_align(g_date_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(g_date_label, 2, 0);
    lv_label_set_long_mode(g_date_label, LV_LABEL_LONG_CLIP);

    create_text(g_screen, "SEC", &lv_font_montserrat_12, lv_color_hex(0x7C6C64), 248, 60);
    g_theme_motion_bar = create_plain_block(g_screen, 18, 136, 0, 2, lv_color_hex(0xD98241), LV_OPA_COVER);
    g_theme_motion_head = create_plain_block(g_screen, 18, 133, 10, 8, lv_color_hex(0xFFF0BF), LV_OPA_COVER);
    lv_obj_set_style_radius(g_theme_motion_bar, 1, 0);
    lv_obj_set_style_radius(g_theme_motion_head, 4, 0);

    g_footer_label = create_text(g_screen, "", &lv_font_montserrat_12, lv_color_hex(0xB79E8A), 12, 154);
    lv_obj_set_width(g_footer_label, g_screen_w - 24);
    lv_obj_set_style_text_align(g_footer_label, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_theme_mono()
{
    create_second_row(g_screen, 8, 7, 4);
    create_title_area(g_screen, lv_color_hex(0xF3EBDD), lv_color_hex(0xC86043), 19, 78);

    create_plain_block(g_screen, 18, 36, g_screen_w - 36, 106, lv_color_hex(0xF1EAE1), LV_OPA_COVER);
    create_plain_block(g_screen, 28, 48, 2, 70, lv_color_hex(0xD7CDC1), LV_OPA_80);
    create_plain_block(g_screen, 290, 48, 2, 70, lv_color_hex(0xD7CDC1), LV_OPA_80);

    g_time_box = create_plain_block(g_screen, 32, 44, 256, 58, lv_color_hex(0xF1EAE1), LV_OPA_0);
    g_theme_motion_block = create_plain_block(g_screen, 40, 132, 232, 1, lv_color_hex(0xD7CDC1), LV_OPA_80);

    g_hour_card = create_plain_block(g_screen, 42, 46, 88, 52, lv_color_hex(0xF1EAE1), LV_OPA_0);
    g_minute_card = create_plain_block(g_screen, 174, 46, 88, 52, lv_color_hex(0xF1EAE1), LV_OPA_0);
    g_second_card = create_card(g_screen, 242, 108, 48, 24, lv_color_hex(0xEEE4D8), lv_color_hex(0xD7CDC1));
    lv_obj_set_style_radius(g_second_card, 12, 0);
    lv_obj_set_style_shadow_width(g_second_card, 0, 0);
    lv_obj_set_style_bg_grad_color(g_second_card, lv_color_hex(0xEEE4D8), 0);

    g_colon_label = create_text(g_screen, ":", &lv_font_montserrat_48, lv_color_hex(0xC86043), 144, 50);
    g_second_colon_label = nullptr;
    lv_obj_set_size(g_colon_label, 16, 52);
    lv_obj_set_style_text_align(g_colon_label, LV_TEXT_ALIGN_CENTER, 0);

    g_hour_label = create_text(g_hour_card, "00", &lv_font_montserrat_48, lv_color_hex(0x181513), 0, -1);
    g_minute_label = create_text(g_minute_card, "00", &lv_font_montserrat_48, lv_color_hex(0x181513), 0, -1);
    g_seconds_label = create_text(g_second_card, "00", &lv_font_montserrat_16, lv_color_hex(0xAF553E), 0, 0);
    lv_obj_set_size(g_hour_label, 88, 52);
    lv_obj_set_size(g_minute_label, 88, 52);
    lv_obj_set_size(g_seconds_label, 48, 24);
    lv_obj_set_style_text_align(g_hour_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(g_minute_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(g_seconds_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_hour_label, LV_ALIGN_CENTER, 0, 7);
    lv_obj_align(g_minute_label, LV_ALIGN_CENTER, 0, 7);
    lv_obj_align(g_seconds_label, LV_ALIGN_CENTER, 0, 2);

    g_date_label = create_text(g_screen, "0000-00-00", &lv_font_montserrat_16, lv_color_hex(0x6F655E), 40, 110);
    lv_obj_set_width(g_date_label, 160);
    lv_obj_set_style_text_align(g_date_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_letter_space(g_date_label, 1, 0);
    lv_label_set_long_mode(g_date_label, LV_LABEL_LONG_CLIP);

    create_text(g_screen, "SEC", &lv_font_montserrat_12, lv_color_hex(0xA4978A), 244, 94);
    g_theme_motion_bar = create_plain_block(g_screen, 204, 110, 0, 3, lv_color_hex(0xC86043), LV_OPA_COVER);
    g_theme_motion_head = create_plain_block(g_screen, 204, 107, 10, 9, lv_color_hex(0x1E1A18), LV_OPA_COVER);
    lv_obj_set_style_radius(g_theme_motion_bar, 2, 0);
    lv_obj_set_style_radius(g_theme_motion_head, 4, 0);

    g_footer_label = create_text(g_screen, "", &lv_font_montserrat_12, lv_color_hex(0x746A63), 12, 154);
    lv_obj_set_width(g_footer_label, g_screen_w - 24);
    lv_obj_set_style_text_align(g_footer_label, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_theme_ui()
{
    if (g_screen == nullptr)
    {
        return;
    }

    reset_ui_refs();
    lv_obj_clean(g_screen);
    g_last_time_value = static_cast<time_t>(-1);

    switch (g_theme)
    {
    case ClockTheme::Flip:
        lv_obj_set_style_bg_opa(g_screen, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(g_screen, lv_color_hex(0x040812), 0);
        lv_obj_set_style_bg_grad_dir(g_screen, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_grad_color(g_screen, lv_color_hex(0x071A2E), 0);
        build_theme_flip();
        break;
    case ClockTheme::Aura:
        lv_obj_set_style_bg_opa(g_screen, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(g_screen, lv_color_hex(0x07111A), 0);
        lv_obj_set_style_bg_grad_dir(g_screen, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_grad_color(g_screen, lv_color_hex(0x103149), 0);
        build_theme_aura();
        break;
    case ClockTheme::Strap:
        lv_obj_set_style_bg_opa(g_screen, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(g_screen, lv_color_hex(0x0E0C0B), 0);
        lv_obj_set_style_bg_grad_dir(g_screen, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_grad_color(g_screen, lv_color_hex(0x161211), 0);
        build_theme_strap();
        break;
    case ClockTheme::Mono:
        lv_obj_set_style_bg_opa(g_screen, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(g_screen, lv_color_hex(0xE8DFD2), 0);
        lv_obj_set_style_bg_grad_dir(g_screen, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_grad_color(g_screen, lv_color_hex(0xF2EBE2), 0);
        build_theme_mono();
        break;
    }

    lv_obj_clear_flag(g_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_update_layout(g_screen);
    update_clock_labels(true);
}

static void update_second_progress(int second)
{
    if (second < 0)
    {
        second = 0;
    }
    if (second > 59)
    {
        second = 59;
    }

    const ThemePalette palette = current_palette();
    for (size_t i = 0; i < g_second_cells.size(); ++i)
    {
        lv_obj_t *cell = g_second_cells[i];
        if (cell == nullptr)
        {
            continue;
        }

        if ((int)i < second)
        {
            lv_obj_set_style_bg_opa(cell, LV_OPA_70, 0);
            lv_obj_set_style_bg_color(cell, palette.progress_fill, 0);
            lv_obj_set_style_shadow_width(cell, 0, 0);
        }
        else if ((int)i == second)
        {
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(cell, palette.progress_head, 0);
            lv_obj_set_style_shadow_width(cell, 10, 0);
            lv_obj_set_style_shadow_opa(cell, kOpaGlow, 0);
            lv_obj_set_style_shadow_color(cell, palette.progress_glow, 0);
        }
        else
        {
            lv_obj_set_style_bg_opa(cell, LV_OPA_20, 0);
            lv_obj_set_style_bg_color(cell, palette.progress_dim, 0);
            lv_obj_set_style_shadow_width(cell, 0, 0);
        }
    }
}

static void update_edit_highlight()
{
    const ThemePalette palette = current_palette();
    const bool hour_active = g_edit_mode && kEditOrder[g_selected_field] == EditField::Hour;
    const bool minute_active = g_edit_mode && kEditOrder[g_selected_field] == EditField::Minute;
    const bool second_active = g_edit_mode && kEditOrder[g_selected_field] == EditField::Second;
    const bool date_active = g_edit_mode &&
                             (kEditOrder[g_selected_field] == EditField::Year ||
                              kEditOrder[g_selected_field] == EditField::Month ||
                              kEditOrder[g_selected_field] == EditField::Day);

    if (g_hour_label != nullptr)
    {
        lv_obj_set_style_text_color(g_hour_label, hour_active ? palette.selected : palette.normal_time, 0);
    }
    if (g_hour_next_label != nullptr)
    {
        lv_obj_set_style_text_color(g_hour_next_label, hour_active ? palette.selected : palette.normal_time, 0);
    }
    if (g_hour_card != nullptr)
    {
        lv_obj_set_style_bg_color(g_hour_card, hour_active ? palette.card_selected : palette.card_normal, 0);
        lv_obj_set_style_border_color(g_hour_card, hour_active ? palette.card_border_selected : palette.card_border, 0);
        lv_obj_set_style_border_width(g_hour_card, hour_active ? 1 : 0, 0);
        lv_obj_set_style_shadow_width(g_hour_card, hour_active ? 10 : 0, 0);
        lv_obj_set_style_shadow_opa(g_hour_card, hour_active ? LV_OPA_20 : LV_OPA_0, 0);
        lv_obj_set_style_shadow_color(g_hour_card, hour_active ? palette.card_border_selected : palette.card_border, 0);
    }

    if (g_minute_label != nullptr)
    {
        lv_obj_set_style_text_color(g_minute_label, minute_active ? palette.selected : palette.normal_time, 0);
    }
    if (g_minute_next_label != nullptr)
    {
        lv_obj_set_style_text_color(g_minute_next_label, minute_active ? palette.selected : palette.normal_time, 0);
    }
    if (g_minute_card != nullptr)
    {
        lv_obj_set_style_bg_color(g_minute_card, minute_active ? palette.card_selected : palette.card_normal, 0);
        lv_obj_set_style_border_color(g_minute_card, minute_active ? palette.card_border_selected : palette.card_border, 0);
        lv_obj_set_style_border_width(g_minute_card, minute_active ? 1 : 0, 0);
        lv_obj_set_style_shadow_width(g_minute_card, minute_active ? 10 : 0, 0);
        lv_obj_set_style_shadow_opa(g_minute_card, minute_active ? LV_OPA_20 : LV_OPA_0, 0);
        lv_obj_set_style_shadow_color(g_minute_card, minute_active ? palette.card_border_selected : palette.card_border, 0);
    }

    if (g_seconds_label != nullptr)
    {
        lv_obj_set_style_text_color(g_seconds_label, second_active ? palette.selected : palette.second_time, 0);
    }
    if (g_seconds_next_label != nullptr)
    {
        lv_obj_set_style_text_color(g_seconds_next_label, second_active ? palette.selected : palette.second_time, 0);
    }
    if (g_second_card != nullptr)
    {
        lv_obj_set_style_bg_color(g_second_card, second_active ? palette.second_card_selected : palette.second_card_normal, 0);
        lv_obj_set_style_border_color(g_second_card, second_active ? palette.second_border_selected : palette.second_border, 0);
        lv_obj_set_style_border_width(g_second_card, second_active ? 1 : 0, 0);
        lv_obj_set_style_shadow_width(g_second_card, second_active ? 10 : 0, 0);
        lv_obj_set_style_shadow_opa(g_second_card, second_active ? LV_OPA_20 : LV_OPA_0, 0);
        lv_obj_set_style_shadow_color(g_second_card, second_active ? palette.second_border_selected : palette.second_border, 0);
    }

    if (g_date_label != nullptr)
    {
        lv_obj_set_style_text_color(g_date_label, date_active ? palette.selected : palette.normal_aux, 0);
    }
}

static void update_status_texts()
{
    if (g_footer_label == nullptr)
    {
        return;
    }

    std::string footer_text;
    const int theme_index = static_cast<int>(g_theme) + 1;
    if (g_edit_mode)
    {
        footer_text = std::to_string(theme_index);
        footer_text += "/4 [";
        footer_text += field_name(kEditOrder[g_selected_field]);
        footer_text += "]  4< 5- 6o 7+ 8>";
    }
    else if (g_time_offset_seconds == 0)
    {
        footer_text = std::to_string(theme_index);
        footer_text += "/4  4< 5- 6* 7+ 8>";
    }
    else
    {
        footer_text = std::to_string(theme_index);
        footer_text += "/4 D";
        footer_text += format_delta(g_time_offset_seconds);
        footer_text += "  6*";
    }

    lv_label_set_text(g_footer_label, footer_text.c_str());
}

static void set_flip_visual(lv_obj_t *label, int32_t translate_y, lv_opa_t opa)
{
    if (label == nullptr)
    {
        return;
    }
    lv_obj_set_style_translate_y(label, translate_y, 0);
    lv_obj_set_style_opa(label, opa, 0);
}

static void finish_flip(lv_obj_t *label, lv_obj_t *next_label, char *cache_text)
{
    if (label == nullptr || next_label == nullptr)
    {
        return;
    }

    lv_anim_delete(label, nullptr);
    lv_anim_delete(next_label, nullptr);

    const char *next_text = lv_label_get_text(next_label);
    if (next_text != nullptr && next_text[0] != '\0')
    {
        std::snprintf(cache_text, 3, "%s", next_text);
        lv_label_set_text(label, next_text);
    }

    set_flip_visual(label, 0, LV_OPA_COVER);
    set_flip_visual(next_label, -kFlipAnimDistance, LV_OPA_TRANSP);
    lv_label_set_text(next_label, "");
}

static void flip_out_exec_cb(void *var, int32_t value)
{
    lv_obj_t *label = static_cast<lv_obj_t *>(var);
    const uint32_t distance = (uint32_t)(value < 0 ? -value : value);
    const lv_opa_t opa = (distance >= (uint32_t)kFlipAnimDistance) ? LV_OPA_40 :
                         (lv_opa_t)(LV_OPA_COVER - (distance * 120U) / (uint32_t)kFlipAnimDistance);
    set_flip_visual(label, value, opa);
}

static void flip_in_exec_cb(void *var, int32_t value)
{
    lv_obj_t *label = static_cast<lv_obj_t *>(var);
    const uint32_t distance = (uint32_t)(value < 0 ? -value : value);
    const lv_opa_t opa = (distance >= (uint32_t)kFlipAnimDistance) ? LV_OPA_40 :
                         (lv_opa_t)(LV_OPA_COVER - (distance * 120U) / (uint32_t)kFlipAnimDistance);
    set_flip_visual(label, value, opa);
}

static void flip_ready_cb(lv_anim_t *anim)
{
    lv_obj_t *label = static_cast<lv_obj_t *>(lv_anim_get_user_data(anim));
    if (label == g_hour_next_label)
    {
        finish_flip(g_hour_label, g_hour_next_label, g_last_hour_text);
    }
    else if (label == g_minute_next_label)
    {
        finish_flip(g_minute_label, g_minute_next_label, g_last_minute_text);
    }
}

static void animate_flip(lv_obj_t *label, lv_obj_t *next_label, char *cache_text, const char *next_text)
{
    if (label == nullptr || next_label == nullptr || next_text == nullptr)
    {
        return;
    }

    finish_flip(label, next_label, cache_text);
    lv_label_set_text(next_label, next_text);
    set_flip_visual(label, 0, LV_OPA_COVER);
    set_flip_visual(next_label, -kFlipAnimDistance, LV_OPA_40);

    lv_anim_t out_anim;
    lv_anim_init(&out_anim);
    lv_anim_set_var(&out_anim, label);
    lv_anim_set_exec_cb(&out_anim, flip_out_exec_cb);
    lv_anim_set_values(&out_anim, 0, kFlipAnimDistance);
    lv_anim_set_duration(&out_anim, kFlipAnimDuration);
    lv_anim_set_path_cb(&out_anim, lv_anim_path_ease_in);
    lv_anim_start(&out_anim);

    lv_anim_t in_anim;
    lv_anim_init(&in_anim);
    lv_anim_set_var(&in_anim, next_label);
    lv_anim_set_exec_cb(&in_anim, flip_in_exec_cb);
    lv_anim_set_values(&in_anim, -kFlipAnimDistance, 0);
    lv_anim_set_duration(&in_anim, kFlipAnimDuration);
    lv_anim_set_path_cb(&in_anim, lv_anim_path_ease_out);
    lv_anim_set_user_data(&in_anim, next_label);
    lv_anim_set_ready_cb(&in_anim, flip_ready_cb);
    lv_anim_start(&in_anim);
}

static void update_clock_labels(bool force)
{
    const time_t shown_time = display_time_now();
    if (!force && shown_time == g_last_time_value)
    {
        return;
    }
    g_last_time_value = shown_time;

    struct tm local_tm {};
    localtime_r(&shown_time, &local_tm);

    char hour_text[3];
    char minute_text[3];
    char seconds_text[3];
    char date_text[11];
    std::snprintf(hour_text, sizeof(hour_text), "%02d", local_tm.tm_hour);
    std::snprintf(minute_text, sizeof(minute_text), "%02d", local_tm.tm_min);
    std::snprintf(seconds_text, sizeof(seconds_text), "%02d", local_tm.tm_sec);
    std::snprintf(date_text, sizeof(date_text), "%04d-%02d-%02d",
                  local_tm.tm_year + 1900,
                  local_tm.tm_mon + 1,
                  local_tm.tm_mday);

    if (g_hour_hand != nullptr || g_minute_hand != nullptr || g_second_hand != nullptr)
    {
        const int hour_angle = ((local_tm.tm_hour % 12) * 300) + (local_tm.tm_min * 5);
        const int minute_angle = (local_tm.tm_min * 60) + local_tm.tm_sec;
        const int second_angle = local_tm.tm_sec * 60;
        set_hand_angle(g_hour_hand, hour_angle);
        set_hand_angle(g_minute_hand, minute_angle);
        set_hand_angle(g_second_hand, second_angle);
    }

    if (theme_uses_flip())
    {
        if (g_hour_label != nullptr)
        {
            if (g_last_hour_text[0] == '\0' || force)
            {
                std::snprintf(g_last_hour_text, sizeof(g_last_hour_text), "%s", hour_text);
                finish_flip(g_hour_label, g_hour_next_label, g_last_hour_text);
                lv_label_set_text(g_hour_label, hour_text);
            }
            else if (std::strcmp(g_last_hour_text, hour_text) != 0)
            {
                animate_flip(g_hour_label, g_hour_next_label, g_last_hour_text, hour_text);
            }
        }

        if (g_minute_label != nullptr)
        {
            if (g_last_minute_text[0] == '\0' || force)
            {
                std::snprintf(g_last_minute_text, sizeof(g_last_minute_text), "%s", minute_text);
                finish_flip(g_minute_label, g_minute_next_label, g_last_minute_text);
                lv_label_set_text(g_minute_label, minute_text);
            }
            else if (std::strcmp(g_last_minute_text, minute_text) != 0)
            {
                animate_flip(g_minute_label, g_minute_next_label, g_last_minute_text, minute_text);
            }
        }
    }
    else
    {
        std::snprintf(g_last_hour_text, sizeof(g_last_hour_text), "%s", hour_text);
        std::snprintf(g_last_minute_text, sizeof(g_last_minute_text), "%s", minute_text);
        if (g_hour_label != nullptr) lv_label_set_text(g_hour_label, hour_text);
        if (g_minute_label != nullptr) lv_label_set_text(g_minute_label, minute_text);
    }

    std::snprintf(g_last_second_text, sizeof(g_last_second_text), "%s", seconds_text);
    if (g_seconds_label != nullptr) lv_label_set_text(g_seconds_label, seconds_text);
    if (g_seconds_next_label != nullptr) lv_label_set_text(g_seconds_next_label, "");
    if (g_date_label != nullptr) lv_label_set_text(g_date_label, date_text);

    if (g_theme == ClockTheme::Strap && g_theme_motion_bar != nullptr && g_theme_motion_head != nullptr)
    {
        const int track_w = 264;
        const int fill_w = ((local_tm.tm_sec + 1) * track_w) / 60;
        lv_obj_set_width(g_theme_motion_bar, fill_w);
        lv_obj_set_x(g_theme_motion_head, 20 + (fill_w > 8 ? fill_w - 8 : 0));
    }
    else if (g_theme == ClockTheme::Mono && g_theme_motion_bar != nullptr && g_theme_motion_head != nullptr && g_theme_motion_block != nullptr)
    {
        const int track_w = 86;
        const int fill_w = ((local_tm.tm_sec + 1) * track_w) / 60;
        lv_obj_set_width(g_theme_motion_bar, fill_w);
        lv_obj_set_x(g_theme_motion_head, 204 + (fill_w > 10 ? fill_w - 10 : 0));

        const int block_w = 60 + ((local_tm.tm_sec % 10) * 18);
        lv_obj_set_width(g_theme_motion_block, block_w);
    }

    if (force || g_debug_clock_log_count < 5)
    {
        debug_log("clock update force=%d epoch=%lld hm=%s:%s sec=%s date=%s theme=%s",
                  force ? 1 : 0,
                  static_cast<long long>(shown_time),
                  hour_text,
                  minute_text,
                  seconds_text,
                  date_text,
                  theme_name(g_theme));

        if (g_time_box != nullptr)
        {
            debug_log("time box x=%d y=%d w=%d h=%d",
                      lv_obj_get_x(g_time_box),
                      lv_obj_get_y(g_time_box),
                      lv_obj_get_width(g_time_box),
                      lv_obj_get_height(g_time_box));
        }
        ++g_debug_clock_log_count;
    }

    update_second_progress(local_tm.tm_sec);
    update_status_texts();
    update_edit_highlight();
}

static void update_blink_state()
{
    (void)kOpaSoft;
}

static void step_field_selection(int delta)
{
    const int count = static_cast<int>(kEditOrder.size());
    int next = static_cast<int>(g_selected_field) + delta;
    while (next < 0)
    {
        next += count;
    }
    g_selected_field = static_cast<size_t>(next % count);
}

static void apply_selected_delta(int delta)
{
    const time_t now = std::time(nullptr);
    time_t shown_time = now + static_cast<time_t>(g_time_offset_seconds);
    struct tm local_tm {};
    localtime_r(&shown_time, &local_tm);

    switch (kEditOrder[g_selected_field])
    {
    case EditField::Hour:
        local_tm.tm_hour += delta;
        break;
    case EditField::Minute:
        local_tm.tm_min += delta;
        break;
    case EditField::Second:
        local_tm.tm_sec += delta;
        break;
    case EditField::Year:
        local_tm.tm_year += delta;
        break;
    case EditField::Month:
        local_tm.tm_mon += delta;
        break;
    case EditField::Day:
        local_tm.tm_mday += delta;
        break;
    }

    const time_t updated = mktime(&local_tm);
    if (updated == static_cast<time_t>(-1))
    {
        return;
    }

    g_time_offset_seconds = static_cast<long long>(updated) - static_cast<long long>(now);
}

static int decode_control_key(uint32_t key_code, const char *utf8)
{
    if (utf8 != nullptr && utf8[0] != '\0' && utf8[1] == '\0')
    {
        switch (utf8[0])
        {
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
            return utf8[0];
        default:
            break;
        }
    }

    switch (key_code)
    {
    case KEY_4:
    case KEY_KP4:
        return '4';
    case KEY_5:
    case KEY_KP5:
        return '5';
    case KEY_6:
    case KEY_KP6:
        return '6';
    case KEY_7:
    case KEY_KP7:
        return '7';
    case KEY_8:
    case KEY_KP8:
        return '8';
    default:
        return 0;
    }
}

static void switch_theme(int delta)
{
    int next = static_cast<int>(g_theme) + delta;
    while (next < 0)
    {
        next += theme_count();
    }
    g_theme = static_cast<ClockTheme>(next % theme_count());
    debug_log("theme switch -> %s", theme_name(g_theme));
    build_theme_ui();
}

static void handle_control_key(int key)
{
    if (key == 0)
    {
        return;
    }

    if (!g_edit_mode && key != '6')
    {
        g_edit_mode = true;
    }

    switch (key)
    {
    case '4':
        step_field_selection(-1);
        break;
    case '5':
        apply_selected_delta(-1);
        break;
    case '6':
        g_edit_mode = !g_edit_mode;
        break;
    case '7':
        apply_selected_delta(1);
        break;
    case '8':
        step_field_selection(1);
        break;
    default:
        break;
    }

    debug_log("key=%c edit=%d field=%s delta=%s",
              key,
              g_edit_mode ? 1 : 0,
              field_name(kEditOrder[g_selected_field]),
              format_delta(g_time_offset_seconds).c_str());

    update_clock_labels(true);
}

static void ui_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    update_blink_state();
    update_clock_labels(false);
}

} // namespace

void ui_init()
{
    g_screen = lv_screen_active();
    lv_display_t *disp = lv_display_get_default();
    if (disp != nullptr)
    {
        g_screen_w = lv_display_get_horizontal_resolution(disp);
        g_screen_h = lv_display_get_vertical_resolution(disp);
    }

    g_debug_enabled = (std::getenv("RTC_DEBUG") != nullptr);
    debug_log("ui init screen=%dx%d", g_screen_w, g_screen_h);

    if (g_group == nullptr)
    {
        g_group = lv_group_create();
    }
    if (g_ui_timer == nullptr)
    {
        g_ui_timer = lv_timer_create(ui_tick_cb, kUiTickMs, nullptr);
    }

    build_theme_ui();
}

lv_group_t *ui_get_input_group()
{
    return g_group;
}

void ui_handle_key_item(uint32_t key_code, const char *utf8, int key_state)
{
    if (key_state == 0)
    {
        return;
    }

    if (key_code == KEY_LEFT)
    {
        switch_theme(-1);
        return;
    }
    if (key_code == KEY_RIGHT)
    {
        switch_theme(1);
        return;
    }

    handle_control_key(decode_control_key(key_code, utf8));
}

void ui_handle_lvgl_key(uint32_t key)
{
    char utf8[2] = {0, 0};
    if (key >= 0x20 && key < 0x7F)
    {
        utf8[0] = (char)key;
    }

    uint32_t mapped = key;
    if (key == LV_KEY_UP) mapped = KEY_UP;
    else if (key == LV_KEY_DOWN) mapped = KEY_DOWN;
    else if (key == LV_KEY_LEFT) mapped = KEY_LEFT;
    else if (key == LV_KEY_RIGHT) mapped = KEY_RIGHT;
    else if (key == LV_KEY_ESC) mapped = KEY_ESC;
    else if (key == LV_KEY_ENTER) mapped = KEY_ENTER;
    else if (key == LV_KEY_BACKSPACE) mapped = KEY_BACKSPACE;

    ui_handle_key_item(mapped, utf8[0] ? utf8 : nullptr, 1);
}
