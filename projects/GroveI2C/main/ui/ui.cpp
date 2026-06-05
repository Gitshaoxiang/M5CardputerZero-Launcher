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

static lv_obj_t *g_cells[128] = {};
static lv_timer_t *g_ui_timer = nullptr;
static lv_group_t *g_group = nullptr;
static std::atomic<bool> g_scanning{false};
static std::atomic<bool> g_scan_ready{false};
static std::mutex g_scan_mutex;
static std::array<bool, 128> g_found{};
static lv_obj_t *g_top_fx = nullptr;
static int g_top_fx_step = 0;
static int g_scan_elapsed_ms = 0;
static int g_screen_w = 320;
static int g_screen_h = 170;

static constexpr int kAddrCols = 16;
static constexpr int kGridPad = 3;
static constexpr int kGridGap = 2;
static constexpr int kGridCellW = 18;
static constexpr int kCellH = 11;
static constexpr int kScanIntervalMs = 2000;
 
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

static void clear_highlight()
{
    for (int i = 0; i < 128; ++i)
    {
        if (g_cells[i] == nullptr)
        {
            continue;
        }

        lv_obj_set_style_bg_color(g_cells[i], lv_color_hex(0x16283C), 0);
        lv_obj_set_style_border_color(g_cells[i], lv_color_hex(0x355574), 0);
        lv_obj_set_style_text_color(g_cells[i], lv_color_hex(0xB6CCE7), 0);
    }
}

static void apply_scan_result()
{
    std::array<bool, 128> found{};
    {
        std::lock_guard<std::mutex> lock(g_scan_mutex);
        found = g_found;
    }

    clear_highlight();

    for (int i = 0; i < 128; ++i)
    {
        if (!found[i] || g_cells[i] == nullptr)
        {
            continue;
        }

        lv_obj_set_style_bg_color(g_cells[i], lv_color_hex(0x38B56A), 0);
        lv_obj_set_style_border_color(g_cells[i], lv_color_hex(0x6DE59A), 0);
        lv_obj_set_style_text_color(g_cells[i], lv_color_hex(0xFFFFFF), 0);
    }
}

static void scan_worker()
{
    printf("[GroveI2C] scan: switch power on (gpio17=1)\n");
    system("gpioset -c gpiochip0 17=1 >/dev/null 2>&1");
    printf("[GroveI2C] scan: switch mux to I2C (gpio4=1)\n");
    system("gpioset -c gpiochip0 4=1 >/dev/null 2>&1");

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

static lv_obj_t *create_addr_cell(lv_obj_t *parent, int addr, int cell_w)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, cell_w, kCellH);
    lv_obj_set_style_radius(cell, 3, 0);
    lv_obj_set_style_border_width(cell, 1, 0);

    char text[4];
    std::snprintf(text, sizeof(text), "%02X", addr);
    lv_obj_t *label = lv_label_create(cell);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_center(label);

    return cell;
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

    const int header_top = 8;
    const int header_h = 28;
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, g_screen_w, header_h);
    lv_obj_set_pos(header, 0, header_top);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(header, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Grove I2C Scaner");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF1F7FF), 0);
    lv_obj_center(title);

    lv_obj_t *grid = lv_obj_create(screen);
    const int header_bottom = header_top + header_h;
    const int grid_top = header_bottom + 1;
    const int grid_bottom_margin = 2;
    int grid_h = g_screen_h - grid_top - grid_bottom_margin;
    if (grid_h < 90)
    {
        grid_h = 90;
    }
    const int available_w = g_screen_w - 20;
    const int ideal_w = kGridPad * 2 + kAddrCols * kGridCellW + (kAddrCols - 1) * kGridGap;
    const int grid_w = ideal_w < available_w ? ideal_w : available_w;
    lv_obj_set_size(grid, grid_w, grid_h);
    lv_obj_set_pos(grid, (g_screen_w - grid_w) / 2, grid_top);
    lv_obj_set_style_bg_color(grid, lv_color_hex(0x101E31), 0);
    lv_obj_set_style_border_color(grid, lv_color_hex(0x37557A), 0);
    lv_obj_set_style_border_width(grid, 1, 0);
    lv_obj_set_style_radius(grid, 6, 0);
    lv_obj_set_style_pad_all(grid, kGridPad, 0);
    lv_obj_set_style_pad_column(grid, kGridGap, 0);
    lv_obj_set_style_pad_row(grid, 2, 0);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(
        grid,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    int content_w = grid_w - (kGridPad * 2);
    int cell_w = (content_w - (kAddrCols - 1) * kGridGap) / kAddrCols;
    if (cell_w > kGridCellW)
    {
        cell_w = kGridCellW;
    }
    if (cell_w < 15)
    {
        cell_w = 15;
    }

    for (int addr = 0; addr < 128; ++addr)
    {
        g_cells[addr] = create_addr_cell(grid, addr, cell_w);
    }
    clear_highlight();

    g_group = lv_group_create();

    g_ui_timer = lv_timer_create(ui_tick_cb, 28, nullptr);
    (void)g_ui_timer;

    g_scan_elapsed_ms = 0;
    start_scan();
}

lv_group_t *ui_get_input_group()
{
    return g_group;
}
