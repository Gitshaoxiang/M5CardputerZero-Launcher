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

static lv_obj_t *g_border_runner_main = nullptr;
static lv_obj_t *g_border_runner_carry = nullptr;
static lv_obj_t *g_border_frame = nullptr;
static lv_obj_t *g_cells[128] = {};
static lv_timer_t *g_ui_timer = nullptr;
static lv_group_t *g_group = nullptr;
static std::atomic<bool> g_scanning{false};
static std::atomic<bool> g_scan_ready{false};
static std::mutex g_scan_mutex;
static std::array<bool, 128> g_found{};
static int g_border_step = 0;
static int g_screen_w = 320;
static int g_screen_h = 170;

static constexpr int kAddrCols = 16;
static constexpr int kGridPad = 6;
static constexpr int kGridGap = 2;
static constexpr int kCellH = 12;

static void set_runner_rect(int step, int segment, int thickness)
{
    if (g_border_runner_main == nullptr || g_border_runner_carry == nullptr)
    {
        return;
    }

    const int margin = 3;
    const int inner_w = g_screen_w - margin * 2;
    const int inner_h = g_screen_h - margin * 2;
    const int top_len = inner_w - 1;
    const int right_len = inner_h - 1;
    const int bottom_len = inner_w - 1;
    const int left_len = inner_h - 1;
    const int perimeter = top_len + right_len + bottom_len + left_len;

    int p = step % perimeter;
    if (p < 0)
    {
        p += perimeter;
    }

    int edge = 0;
    int edge_start = 0;
    int edge_len = top_len;
    if (p < top_len)
    {
        edge = 0;
        edge_start = 0;
        edge_len = top_len;
    }
    else if (p < top_len + right_len)
    {
        edge = 1;
        edge_start = top_len;
        edge_len = right_len;
    }
    else if (p < top_len + right_len + bottom_len)
    {
        edge = 2;
        edge_start = top_len + right_len;
        edge_len = bottom_len;
    }
    else
    {
        edge = 3;
        edge_start = top_len + right_len + bottom_len;
        edge_len = left_len;
    }

    int dist_on_edge = p - edge_start;
    int main_len = dist_on_edge + 1;
    if (main_len > segment)
    {
        main_len = segment;
    }
    int carry_len = segment - main_len;
    if (carry_len < 0)
    {
        carry_len = 0;
    }

    int x = margin;
    int y = margin;
    int w = thickness;
    int h = thickness;

    if (edge == 0)
    {
        int head_x = margin + dist_on_edge;
        x = head_x - main_len + 1;
        y = margin;
        w = main_len;
        h = thickness;
        lv_obj_set_style_bg_grad_dir(g_border_runner_main, LV_GRAD_DIR_HOR, 0);
        lv_obj_set_style_bg_color(g_border_runner_main, lv_color_hex(0x3F8FC1), 0);
        lv_obj_set_style_bg_grad_color(g_border_runner_main, lv_color_hex(0xA9EBFF), 0);
    }
    else if (edge == 1)
    {
        int head_y = margin + dist_on_edge;
        x = margin + inner_w - 1;
        y = head_y - main_len + 1;
        w = thickness;
        h = main_len;
        lv_obj_set_style_bg_grad_dir(g_border_runner_main, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_color(g_border_runner_main, lv_color_hex(0x3F8FC1), 0);
        lv_obj_set_style_bg_grad_color(g_border_runner_main, lv_color_hex(0xA9EBFF), 0);
    }
    else if (edge == 2)
    {
        int head_x = margin + inner_w - 1 - dist_on_edge;
        x = head_x;
        y = margin + inner_h - 1;
        w = main_len;
        h = thickness;
        lv_obj_set_style_bg_grad_dir(g_border_runner_main, LV_GRAD_DIR_HOR, 0);
        lv_obj_set_style_bg_color(g_border_runner_main, lv_color_hex(0xA9EBFF), 0);
        lv_obj_set_style_bg_grad_color(g_border_runner_main, lv_color_hex(0x3F8FC1), 0);
    }
    else
    {
        int head_y = margin + inner_h - 1 - dist_on_edge;
        x = margin;
        y = head_y;
        w = thickness;
        h = main_len;
        lv_obj_set_style_bg_grad_dir(g_border_runner_main, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_color(g_border_runner_main, lv_color_hex(0xA9EBFF), 0);
        lv_obj_set_style_bg_grad_color(g_border_runner_main, lv_color_hex(0x3F8FC1), 0);
    }

    lv_obj_set_pos(g_border_runner_main, x, y);
    lv_obj_set_size(g_border_runner_main, w > 1 ? w : 2, h > 1 ? h : 2);

    if (carry_len <= 0)
    {
        lv_obj_add_flag(g_border_runner_carry, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(g_border_runner_carry, LV_OBJ_FLAG_HIDDEN);
    int cx = margin;
    int cy = margin;
    int cw = thickness;
    int ch = thickness;

    if (edge == 0)
    {
        cx = margin;
        cy = margin + inner_h - carry_len;
        cw = thickness;
        ch = carry_len;
        lv_obj_set_style_bg_grad_dir(g_border_runner_carry, LV_GRAD_DIR_VER, 0);
    }
    else if (edge == 1)
    {
        cx = margin + inner_w - carry_len;
        cy = margin;
        cw = carry_len;
        ch = thickness;
        lv_obj_set_style_bg_grad_dir(g_border_runner_carry, LV_GRAD_DIR_HOR, 0);
    }
    else if (edge == 2)
    {
        cx = margin + inner_w - 1;
        cy = margin + inner_h - carry_len;
        cw = thickness;
        ch = carry_len;
        lv_obj_set_style_bg_grad_dir(g_border_runner_carry, LV_GRAD_DIR_VER, 0);
    }
    else
    {
        cx = margin + inner_w - carry_len;
        cy = margin + inner_h - 1;
        cw = carry_len;
        ch = thickness;
        lv_obj_set_style_bg_grad_dir(g_border_runner_carry, LV_GRAD_DIR_HOR, 0);
    }

    lv_obj_set_style_bg_color(g_border_runner_carry, lv_color_hex(0x316F98), 0);
    lv_obj_set_style_bg_grad_color(g_border_runner_carry, lv_color_hex(0x4A9DCC), 0);
    lv_obj_set_pos(g_border_runner_carry, cx, cy);
    lv_obj_set_size(g_border_runner_carry, cw > 1 ? cw : 2, ch > 1 ? ch : 2);
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
    system("gpioset -c gpiochip0 17=1 >/dev/null 2>&1");
    system("gpioset -c gpiochip0 4=1 >/dev/null 2>&1");
    std::string output = run_command_capture("i2cdetect -y 1 2>&1");

    auto found = parse_i2cdetect_output(output);
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

    g_scan_ready.store(false);
    g_scanning.store(true);
    clear_highlight();

    std::thread worker(scan_worker);
    worker.detach();
}

static void ui_tick_cb(lv_timer_t *timer)
{
    (void)timer;

    const int margin = 3;
    const int inner_w = g_screen_w - margin * 2;
    const int inner_h = g_screen_h - margin * 2;
    const int perimeter = inner_w * 2 + inner_h * 2 - 4;
    const int speed = 4;
    g_border_step = (g_border_step + speed) % perimeter;

    set_runner_rect(g_border_step, 56, 6);

    if (g_scan_ready.exchange(false))
    {
        apply_scan_result();
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

    g_border_frame = lv_obj_create(screen);
    lv_obj_remove_style_all(g_border_frame);
    lv_obj_set_size(g_border_frame, g_screen_w - 6, g_screen_h - 6);
    lv_obj_set_pos(g_border_frame, 3, 3);
    lv_obj_set_style_border_width(g_border_frame, 3, 0);
    lv_obj_set_style_border_color(g_border_frame, lv_color_hex(0x3F678A), 0);
    lv_obj_set_style_bg_opa(g_border_frame, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_border_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_border_frame, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Grove I2C Scaner");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF1F7FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *grid = lv_obj_create(screen);
    lv_obj_set_size(grid, g_screen_w - 22, g_screen_h - 50);
    lv_obj_align(grid, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(grid, lv_color_hex(0x101E31), 0);
    lv_obj_set_style_border_color(grid, lv_color_hex(0x37557A), 0);
    lv_obj_set_style_border_width(grid, 1, 0);
    lv_obj_set_style_radius(grid, 6, 0);
    lv_obj_set_style_pad_all(grid, kGridPad, 0);
    lv_obj_set_style_pad_column(grid, kGridGap, 0);
    lv_obj_set_style_pad_row(grid, 3, 0);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    int grid_w = static_cast<int>(lv_obj_get_width(grid));
    int content_w = grid_w - (kGridPad * 2);
    int cell_w = (content_w - (kAddrCols - 1) * kGridGap) / kAddrCols;
    if (cell_w < 14)
    {
        cell_w = 14;
    }

    for (int addr = 0; addr < 128; ++addr)
    {
        g_cells[addr] = create_addr_cell(grid, addr, cell_w);
    }
    clear_highlight();

    g_border_runner_main = lv_obj_create(screen);
    lv_obj_remove_style_all(g_border_runner_main);
    lv_obj_set_size(g_border_runner_main, 56, 6);
    lv_obj_set_pos(g_border_runner_main, 3, 3);
    lv_obj_set_style_radius(g_border_runner_main, 3, 0);
    lv_obj_set_style_bg_opa(g_border_runner_main, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(g_border_runner_main, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_color(g_border_runner_main, lv_color_hex(0x3F8FC1), 0);
    lv_obj_set_style_bg_grad_color(g_border_runner_main, lv_color_hex(0xA9EBFF), 0);
    lv_obj_set_style_shadow_width(g_border_runner_main, 10, 0);
    lv_obj_set_style_shadow_opa(g_border_runner_main, LV_OPA_60, 0);
    lv_obj_set_style_shadow_color(g_border_runner_main, lv_color_hex(0x66C8F1), 0);
    lv_obj_clear_flag(g_border_runner_main, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_border_runner_main, LV_SCROLLBAR_MODE_OFF);
    lv_obj_move_foreground(g_border_runner_main);

    g_border_runner_carry = lv_obj_create(screen);
    lv_obj_remove_style_all(g_border_runner_carry);
    lv_obj_set_size(g_border_runner_carry, 4, 4);
    lv_obj_set_pos(g_border_runner_carry, 3, 3);
    lv_obj_set_style_radius(g_border_runner_carry, 2, 0);
    lv_obj_set_style_bg_opa(g_border_runner_carry, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(g_border_runner_carry, lv_color_hex(0x316F98), 0);
    lv_obj_set_style_bg_grad_color(g_border_runner_carry, lv_color_hex(0x4A9DCC), 0);
    lv_obj_set_style_shadow_width(g_border_runner_carry, 6, 0);
    lv_obj_set_style_shadow_opa(g_border_runner_carry, LV_OPA_30, 0);
    lv_obj_set_style_shadow_color(g_border_runner_carry, lv_color_hex(0x4A9DCC), 0);
    lv_obj_clear_flag(g_border_runner_carry, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_border_runner_carry, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_border_runner_carry, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_border_runner_carry);

    g_group = lv_group_create();

    g_ui_timer = lv_timer_create(ui_tick_cb, 28, nullptr);
    (void)g_ui_timer;

    start_scan();
}

lv_group_t *ui_get_input_group()
{
    return g_group;
}
