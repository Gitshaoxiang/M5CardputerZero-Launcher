#include "lvgl/lvgl.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

namespace {

static constexpr uint32_t kUiTickMs = 250;
static constexpr lv_opa_t kOpaSoft = (lv_opa_t)32;
static constexpr lv_opa_t kOpaFaint = (lv_opa_t)26;
static constexpr lv_opa_t kOpaGlow = (lv_opa_t)40;

static lv_group_t *g_group = nullptr;
static lv_timer_t *g_ui_timer = nullptr;
static lv_obj_t *g_top_fx = nullptr;
static lv_obj_t *g_hour_label = nullptr;
static lv_obj_t *g_colon_label = nullptr;
static lv_obj_t *g_minute_label = nullptr;
static lv_obj_t *g_seconds_label = nullptr;
static lv_obj_t *g_date_label = nullptr;
static lv_obj_t *g_weekday_value = nullptr;
static lv_obj_t *g_zone_value = nullptr;
static lv_obj_t *g_source_value = nullptr;
static lv_obj_t *g_footer_label = nullptr;

static int g_screen_w = 320;
static int g_screen_h = 170;
static int g_top_fx_step = 0;
static time_t g_last_time_value = static_cast<time_t>(-1);

static void uppercase_ascii(char *text)
{
    if (text == nullptr)
    {
        return;
    }

    for (char *p = text; *p != '\0'; ++p)
    {
        if (*p >= 'a' && *p <= 'z')
        {
            *p = static_cast<char>(*p - ('a' - 'A'));
        }
    }
}

static std::string format_offset(const char *raw_offset)
{
    if (raw_offset == nullptr)
    {
        return "LOCAL";
    }

    std::string offset = raw_offset;
    if (offset.size() == 5 &&
        (offset[0] == '+' || offset[0] == '-') &&
        std::isdigit(static_cast<unsigned char>(offset[1])) &&
        std::isdigit(static_cast<unsigned char>(offset[2])) &&
        std::isdigit(static_cast<unsigned char>(offset[3])) &&
        std::isdigit(static_cast<unsigned char>(offset[4])))
    {
        return "UTC" + offset.substr(0, 3) + ":" + offset.substr(3, 2);
    }

    return offset.empty() ? "LOCAL" : offset;
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
    lv_obj_set_style_bg_grad_color(card, lv_color_hex(0x0B1727), 0);
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

static void update_top_fx()
{
    if (g_top_fx == nullptr)
    {
        return;
    }

    const int y = 7;
    const int speed = 3;
    const int lead_len = 56;
    const int bar_h = 6;
    const int margin = 10;
    const int loop_span = (g_screen_w - margin * 2) + lead_len;

    g_top_fx_step = (g_top_fx_step + speed) % loop_span;

    const int right_limit = g_screen_w - margin;
    int x = margin - lead_len + g_top_fx_step;
    int w = lead_len;

    if (x < margin)
    {
        w -= (margin - x);
        x = margin;
    }

    if (x + w > right_limit)
    {
        w = right_limit - x;
    }

    if (w < 2)
    {
        lv_obj_add_flag(g_top_fx, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(g_top_fx, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(g_top_fx, x, y);
    lv_obj_set_size(g_top_fx, w, bar_h);
}

static void update_clock_labels(bool force)
{
    const time_t now = std::time(nullptr);
    if (!force && now == g_last_time_value)
    {
        return;
    }
    g_last_time_value = now;

    struct tm local_tm {};
    localtime_r(&now, &local_tm);

    char hour_text[3];
    char minute_text[3];
    char seconds_text[3];
    char date_text[20];
    char weekday_text[16];
    char zone_name[16];
    char zone_offset[8];
    char footer_text[32];

    std::snprintf(hour_text, sizeof(hour_text), "%02d", local_tm.tm_hour);
    std::snprintf(minute_text, sizeof(minute_text), "%02d", local_tm.tm_min);
    std::snprintf(seconds_text, sizeof(seconds_text), "%02d", local_tm.tm_sec);
    std::strftime(date_text, sizeof(date_text), "%Y-%m-%d", &local_tm);
    std::strftime(weekday_text, sizeof(weekday_text), "%A", &local_tm);
    std::strftime(zone_name, sizeof(zone_name), "%Z", &local_tm);
    std::strftime(zone_offset, sizeof(zone_offset), "%z", &local_tm);
    std::snprintf(footer_text, sizeof(footer_text), "DAY %03d  |  LOCAL TIME", local_tm.tm_yday + 1);

    uppercase_ascii(weekday_text);
    uppercase_ascii(zone_name);

    if (g_hour_label != nullptr)
    {
        lv_label_set_text(g_hour_label, hour_text);
    }
    if (g_minute_label != nullptr)
    {
        lv_label_set_text(g_minute_label, minute_text);
    }
    if (g_seconds_label != nullptr)
    {
        lv_label_set_text(g_seconds_label, seconds_text);
    }
    if (g_date_label != nullptr)
    {
        lv_label_set_text(g_date_label, date_text);
    }
    if (g_weekday_value != nullptr)
    {
        lv_label_set_text(g_weekday_value, weekday_text);
    }
    if (g_zone_value != nullptr)
    {
        std::string zone_text = zone_name[0] ? zone_name : "LOCAL";
        zone_text += "  ";
        zone_text += format_offset(zone_offset);
        lv_label_set_text(g_zone_value, zone_text.c_str());
    }
    if (g_source_value != nullptr)
    {
        lv_label_set_text(g_source_value, "SYSTEM CLOCK");
    }
    if (g_footer_label != nullptr)
    {
        lv_label_set_text(g_footer_label, footer_text);
    }
}

static void update_blink_state()
{
    if (g_colon_label == nullptr)
    {
        return;
    }

    const uint32_t ms = lv_tick_get();
    const bool bright = ((ms / 500U) % 2U) == 0U;
    lv_obj_set_style_text_opa(g_colon_label, bright ? LV_OPA_COVER : kOpaSoft, 0);
}

static void ui_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    update_top_fx();
    update_blink_state();
    update_clock_labels(false);
}

} // namespace

void ui_init()
{
    lv_obj_t *screen = lv_screen_active();
    lv_display_t *disp = lv_display_get_default();
    if (disp != nullptr)
    {
        g_screen_w = lv_display_get_horizontal_resolution(disp);
        g_screen_h = lv_display_get_vertical_resolution(disp);
    }

    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x040812), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x071A2E), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *glow_left = lv_obj_create(screen);
    lv_obj_remove_style_all(glow_left);
    lv_obj_set_size(glow_left, 120, 120);
    lv_obj_set_pos(glow_left, -42, 28);
    lv_obj_set_style_radius(glow_left, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(glow_left, kOpaSoft, 0);
    lv_obj_set_style_bg_color(glow_left, lv_color_hex(0x22B8FF), 0);
    lv_obj_set_style_shadow_width(glow_left, 28, 0);
    lv_obj_set_style_shadow_opa(glow_left, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(glow_left, lv_color_hex(0x22B8FF), 0);

    lv_obj_t *glow_right = lv_obj_create(screen);
    lv_obj_remove_style_all(glow_right);
    lv_obj_set_size(glow_right, 86, 86);
    lv_obj_set_pos(glow_right, g_screen_w - 52, 12);
    lv_obj_set_style_radius(glow_right, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(glow_right, kOpaFaint, 0);
    lv_obj_set_style_bg_color(glow_right, lv_color_hex(0x64F4C5), 0);
    lv_obj_set_style_shadow_width(glow_right, 24, 0);
    lv_obj_set_style_shadow_opa(glow_right, kOpaSoft, 0);
    lv_obj_set_style_shadow_color(glow_right, lv_color_hex(0x64F4C5), 0);

    g_top_fx = lv_obj_create(screen);
    lv_obj_remove_style_all(g_top_fx);
    lv_obj_set_size(g_top_fx, 2, 6);
    lv_obj_set_pos(g_top_fx, 10, 7);
    lv_obj_set_style_radius(g_top_fx, 3, 0);
    lv_obj_set_style_bg_opa(g_top_fx, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(g_top_fx, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_color(g_top_fx, lv_color_hex(0x2B8FFF), 0);
    lv_obj_set_style_bg_grad_color(g_top_fx, lv_color_hex(0xA8FFF0), 0);
    lv_obj_set_style_shadow_width(g_top_fx, 12, 0);
    lv_obj_set_style_shadow_opa(g_top_fx, kOpaGlow, 0);
    lv_obj_set_style_shadow_color(g_top_fx, lv_color_hex(0x76D8FF), 0);
    lv_obj_add_flag(g_top_fx, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_top_fx);

    create_text(screen,
                "RTC CLOCK",
                &lv_font_montserrat_16,
                lv_color_hex(0xF3F7FF),
                12,
                16);

    lv_obj_t *header_hint = create_text(screen,
                                        "320x170 DIGITAL FACE",
                                        &lv_font_montserrat_12,
                                        lv_color_hex(0x8EA6BE),
                                        12,
                                        36);
    lv_obj_set_style_text_letter_space(header_hint, 1, 0);

    lv_obj_t *hero = create_card(screen,
                                 8,
                                 54,
                                 g_screen_w - 16,
                                 72,
                                 lv_color_hex(0x08111E),
                                 lv_color_hex(0x2B4F79));

    lv_obj_t *time_row = lv_obj_create(hero);
    lv_obj_remove_style_all(time_row);
    lv_obj_set_size(time_row, 198, 56);
    lv_obj_set_pos(time_row, 14, 8);
    lv_obj_set_layout(time_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);

    g_hour_label = lv_label_create(time_row);
    lv_obj_set_style_text_font(g_hour_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(g_hour_label, lv_color_hex(0xF5FAFF), 0);

    g_colon_label = lv_label_create(time_row);
    lv_label_set_text(g_colon_label, ":");
    lv_obj_set_style_text_font(g_colon_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(g_colon_label, lv_color_hex(0x74D7FF), 0);
    lv_obj_set_style_pad_left(g_colon_label, 2, 0);
    lv_obj_set_style_pad_right(g_colon_label, 2, 0);

    g_minute_label = lv_label_create(time_row);
    lv_obj_set_style_text_font(g_minute_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(g_minute_label, lv_color_hex(0xF5FAFF), 0);

    lv_obj_t *seconds_card = create_card(hero,
                                         lv_obj_get_width(hero) - 74,
                                         10,
                                         58,
                                         52,
                                         lv_color_hex(0x0B1A2A),
                                         lv_color_hex(0x285A7B));
    lv_obj_set_style_radius(seconds_card, 16, 0);

    create_text(seconds_card,
                "SEC",
                &lv_font_montserrat_12,
                lv_color_hex(0x7DA6C4),
                14,
                7);
    g_seconds_label = create_text(seconds_card,
                                  "00",
                                  &lv_font_montserrat_20,
                                  lv_color_hex(0x7FF6C7),
                                  13,
                                  24);

    g_date_label = create_text(hero,
                               "0000-00-00",
                               &lv_font_montserrat_20,
                               lv_color_hex(0xB9D4F2),
                               16,
                               48);

    lv_obj_t *weekday_card = create_card(screen,
                                         8,
                                         132,
                                         148,
                                         30,
                                         lv_color_hex(0x09131F),
                                         lv_color_hex(0x25435E));
    create_text(weekday_card,
                "WEEKDAY",
                &lv_font_montserrat_12,
                lv_color_hex(0x7F98AF),
                10,
                5);
    g_weekday_value = create_text(weekday_card,
                                  "------",
                                  &lv_font_montserrat_12,
                                  lv_color_hex(0xEDF7FF),
                                  74,
                                  5);

    lv_obj_t *zone_card = create_card(screen,
                                      164,
                                      132,
                                      g_screen_w - 172,
                                      30,
                                      lv_color_hex(0x09131F),
                                      lv_color_hex(0x25435E));
    create_text(zone_card,
                "ZONE",
                &lv_font_montserrat_12,
                lv_color_hex(0x7F98AF),
                10,
                5);
    g_zone_value = create_text(zone_card,
                               "LOCAL",
                               &lv_font_montserrat_12,
                               lv_color_hex(0xEDF7FF),
                               44,
                               5);

    g_source_value = create_text(screen,
                                 "SYSTEM CLOCK",
                                 &lv_font_montserrat_12,
                                 lv_color_hex(0x95B7D4),
                                 g_screen_w - 104,
                                 18);

    g_footer_label = create_text(screen,
                                 "DAY 000  |  LOCAL TIME",
                                 &lv_font_montserrat_12,
                                 lv_color_hex(0x89A7C4),
                                 12,
                                 g_screen_h - 18);

    g_group = lv_group_create();
    g_ui_timer = lv_timer_create(ui_tick_cb, kUiTickMs, nullptr);
    (void)g_ui_timer;

    update_top_fx();
    update_blink_state();
    update_clock_labels(true);
}

lv_group_t *ui_get_input_group()
{
    return g_group;
}
