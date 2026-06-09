#include "lvgl/lvgl.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace {

static lv_obj_t *g_table = nullptr;
static lv_obj_t *g_status = nullptr;
static lv_timer_t *g_ui_timer = nullptr;
static lv_group_t *g_group = nullptr;
static std::atomic<bool> g_scanning{false};
static std::atomic<bool> g_scan_ready{false};
static std::atomic<bool> g_grove_i2c_ready{false};
static std::mutex g_scan_mutex;
static std::array<bool, 128> g_found{};
static lv_obj_t *g_top_fx = nullptr;
static int g_top_fx_step = 0;
static int g_scan_elapsed_ms = 0;
static int g_screen_w = 320;
static int g_screen_h = 170;

static constexpr int kScanIntervalMs = 2000;
 
static void setup_grove_i2c_once()
{
    bool expected = false;
    if (!g_grove_i2c_ready.compare_exchange_strong(expected, true))
    {
        return;
    }

    printf("[GroveI2C] init: switch power on (gpio17=1)\n");
    system("timeout 1 gpioset -c gpiochip0 17=1 >/dev/null 2>&1");
    printf("[GroveI2C] init: switch mux to I2C (gpio4=1)\n");
    system("timeout 1 gpioset -c gpiochip0 4=1 >/dev/null 2>&1");
}

static void update_top_fx()
{
    if (g_top_fx == nullptr)
    {
        return;
    }

    const int y = 5;
    const int speed = 4;
    const int lead_len = 48;
    const int bar_h = 6;
    const int top_margin = 8;
    const int loop_span = (g_screen_w - top_margin * 2) + lead_len;

    g_top_fx_step = (g_top_fx_step + speed) % loop_span;

    const int right_limit = g_screen_w - top_margin;
    int x = top_margin - lead_len + g_top_fx_step;
    int w = lead_len;

    if (x < top_margin)
    {
        w -= (top_margin - x);
        x = top_margin;
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

static std::string run_command_capture(const char *cmd)
{
    std::string out;
    FILE *fp = popen(cmd, "r");
    if (fp == nullptr)
    {
        return "";
    }

    char buf[256];
    while (fgets(buf, sizeof(buf), fp) != nullptr)
    {
        out += buf;
    }
    pclose(fp);
    return out;
}

static bool token_is_hex(const std::string &token)
{
    return token.size() == 2 && std::isxdigit(static_cast<unsigned char>(token[0])) &&
           std::isxdigit(static_cast<unsigned char>(token[1]));
}

static std::array<bool, 128> parse_i2cdetect_output(const std::string &text)
{
    std::array<bool, 128> found{};

    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line))
    {
        if (line.size() < 3 || line[2] != ':')
        {
            continue;
        }

        unsigned int row = 0;
        if (std::sscanf(line.c_str(), "%2x:", &row) != 1)
        {
            continue;
        }

        std::string rest = line.substr(3);
        std::istringstream row_stream(rest);
        std::string token;
        int col = 0;
        while (row_stream >> token && col < 16)
        {
            int addr = static_cast<int>(row) + col;
            if (addr >= 0 && addr < 128)
            {
                if (token == "UU" || token_is_hex(token))
                {
                    found[addr] = true;
                }
            }
            col++;
        }
    }

    return found;
}

static int count_found_addr(const std::array<bool, 128> &found)
{
    int count = 0;
    for (bool v : found)
    {
        if (v)
        {
            ++count;
        }
    }
    return count;
}

static std::string format_scan_table(const std::array<bool, 128> &found)
{
    char line[80];
    std::string table = "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n";
    table += "    ------------------------------------------------\n";

    for (int row = 0; row < 8; ++row)
    {
        std::snprintf(line, sizeof(line), "%02x:", row * 16);
        table += line;

        for (int col = 0; col < 16; ++col)
        {
            int addr = row * 16 + col;
            if (addr < 0x03 || addr > 0x77)
            {
                table += "   ";
            }
            else if (found[addr])
            {
                std::snprintf(line, sizeof(line), " %02x", addr);
                table += line;
            }
            else
            {
                table += " --";
            }
        }
        if (row != 7)
        {
            table += "\n";
        }
    }

    return table;
}

static void apply_scan_result()
{
    std::array<bool, 128> found{};
    {
        std::lock_guard<std::mutex> lock(g_scan_mutex);
        found = g_found;
    }

    if (g_table != nullptr)
    {
        std::string table = format_scan_table(found);
        lv_label_set_text(g_table, table.c_str());
    }

    if (g_status != nullptr)
    {
        char status[64];
        std::snprintf(status, sizeof(status), "bus 1 / found %d / refresh 2s", count_found_addr(found));
        lv_label_set_text(g_status, status);
    }

    if (g_table != nullptr)
    {
        lv_obj_invalidate(g_table);
    }
}

static void scan_worker()
{
    printf("[GroveI2C] scan: run i2cdetect -y 1\n");
    std::string output = run_command_capture("i2cdetect -y 1 2>&1");

    auto found = parse_i2cdetect_output(output);
    printf("[GroveI2C] scan: found %d device(s)\n", count_found_addr(found));
    {
        std::lock_guard<std::mutex> lock(g_scan_mutex);
        g_found = found;
    }

    g_scanning.store(false);
    g_scan_ready.store(true);
}

static void start_scan()
{
    if (g_scanning.load())
    {
        return;
    }

    printf("[GroveI2C] scan: start\n");
    g_scan_ready.store(false);
    g_scanning.store(true);

    std::thread worker(scan_worker);
    worker.detach();
}

static void ui_tick_cb(lv_timer_t *timer)
{
    (void)timer;

    update_top_fx();

    if (g_scan_ready.exchange(false))
    {
        apply_scan_result();
    }

    g_scan_elapsed_ms += 28;
    if (g_scan_elapsed_ms >= kScanIntervalMs)
    {
        g_scan_elapsed_ms = 0;
        start_scan();
    }
}

} // namespace

void ui_init()
{
    printf("[GroveI2C] ui_init: setup UI and start periodic scan\n");

    lv_obj_t *screen = lv_screen_active();
    lv_display_t *disp = lv_display_get_default();
    if (disp != nullptr)
    {
        g_screen_w = lv_display_get_horizontal_resolution(disp);
        g_screen_h = lv_display_get_vertical_resolution(disp);
    }

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0C1A2B), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    setup_grove_i2c_once();

    g_top_fx = lv_obj_create(screen);
    lv_obj_remove_style_all(g_top_fx);
    lv_obj_set_size(g_top_fx, 2, 6);
    lv_obj_set_pos(g_top_fx, 8, 5);
    lv_obj_set_style_radius(g_top_fx, 3, 0);
    lv_obj_set_style_bg_opa(g_top_fx, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(g_top_fx, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_color(g_top_fx, lv_color_hex(0x2C92C7), 0);
    lv_obj_set_style_bg_grad_color(g_top_fx, lv_color_hex(0x9FEFFF), 0);
    lv_obj_set_style_shadow_width(g_top_fx, 10, 0);
    lv_obj_set_style_shadow_opa(g_top_fx, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(g_top_fx, lv_color_hex(0x68D5FF), 0);
    lv_obj_clear_flag(g_top_fx, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_top_fx, LV_SCROLLBAR_MODE_OFF);
    lv_obj_move_foreground(g_top_fx);
    lv_obj_add_flag(g_top_fx, LV_OBJ_FLAG_HIDDEN);

    const int header_top = 7;
    const int header_h = 31;
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, g_screen_w, header_h);
    lv_obj_set_pos(header, 0, header_top);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(header, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "i2cdetect -y 1");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF1F7FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 0);

    g_status = lv_label_create(header);
    lv_label_set_text(g_status, "bus 1 / starting");
    lv_obj_set_style_text_font(g_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_status, lv_color_hex(0x93A8BC), 0);
    lv_obj_align(g_status, LV_ALIGN_BOTTOM_LEFT, 10, 1);

    lv_obj_t *panel = lv_obj_create(screen);
    const int panel_top = header_top + header_h + 1;
    lv_obj_set_size(panel, g_screen_w - 12, g_screen_h - panel_top - 4);
    lv_obj_set_pos(panel, 6, panel_top);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x071018), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x2E465F), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 4, 0);
    lv_obj_set_style_pad_all(panel, 5, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);

    g_table = lv_label_create(panel);
    lv_label_set_text(g_table, format_scan_table(g_found).c_str());
    lv_obj_set_style_text_font(g_table, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_table, lv_color_hex(0xBFD7C4), 0);
    lv_obj_set_style_text_line_space(g_table, 0, 0);
    lv_obj_set_style_text_letter_space(g_table, 0, 0);
    lv_obj_set_pos(g_table, 2, 1);

    g_group = lv_group_create();

    g_ui_timer = lv_timer_create(ui_tick_cb, 28, nullptr);
    (void)g_ui_timer;

    g_scan_elapsed_ms = kScanIntervalMs - 300;
}

lv_group_t *ui_get_input_group()
{
    return g_group;
}
