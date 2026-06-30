#include "aircon_service.h"
#include "app_log.h"
#include "ir_service.h"
#include "lvgl/lvgl.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <pwd.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include <linux/input.h>

namespace {

enum class AppPage {
    Aircon,
    Copy,
    Nec,
};

enum class ReceiveMode {
    None,
    CopyRecord,
    NecMonitor,
};

struct StoredSignal {
    std::string name;
    std::string protocol;
    uint16_t address = 0;
    uint16_t command = 0;
    std::vector<uint8_t> data;
    std::vector<uint32_t> raw_timings;
};

struct ListRow {
    std::string title;
    std::string value;
    lv_color_t accent = lv_color_hex(0x78C8FF);
};

constexpr int kVisibleRows = 6;

static lv_group_t *g_group = nullptr;
static lv_obj_t *g_title = nullptr;
static lv_obj_t *g_subtitle = nullptr;
static lv_obj_t *g_notice = nullptr;
static lv_obj_t *g_copy_tab = nullptr;
static lv_obj_t *g_copy_tab_label = nullptr;
static lv_obj_t *g_copy_tab_cursor = nullptr;
static lv_obj_t *g_aircon_tab = nullptr;
static lv_obj_t *g_aircon_tab_label = nullptr;
static lv_obj_t *g_aircon_tab_cursor = nullptr;
static lv_obj_t *g_nec_tab = nullptr;
static lv_obj_t *g_nec_tab_label = nullptr;
static lv_obj_t *g_nec_tab_cursor = nullptr;
static lv_obj_t *g_left_card = nullptr;
static lv_obj_t *g_left_title = nullptr;
static lv_obj_t *g_left_body = nullptr;
static lv_obj_t *g_right_card = nullptr;
static lv_obj_t *g_right_title = nullptr;
static std::array<lv_obj_t *, kVisibleRows> g_rows {};
static std::array<lv_obj_t *, kVisibleRows> g_row_titles {};
static std::array<lv_obj_t *, kVisibleRows> g_row_values {};
static lv_obj_t *g_footer = nullptr;

static AppPage g_page = AppPage::Aircon;
static int g_copy_cursor = 0;
static int g_copy_scroll = 0;
static int g_aircon_cursor = 0;
static int g_aircon_scroll = 0;
static int g_nec_cursor = 0;
static int g_nec_scroll = 0;
static int g_selected_signal = -1;
static ReceiveMode g_receive_mode = ReceiveMode::None;

static std::size_t g_aircon_brand = 0;
static bool g_aircon_power = false;
static int g_aircon_temp = 24;
static aircon::Mode g_aircon_mode = aircon::Mode::Cool;
static aircon::Fan g_aircon_fan = aircon::Fan::Auto;
static bool g_aircon_swing = false;

static std::vector<StoredSignal> g_signals;
static irremote::ReceiverSession g_receiver;
static irremote::ReceiveSnapshot g_last_snapshot;
static irremote::DeviceInfo g_sender_info;
static irremote::DeviceInfo g_receiver_info;
static std::string g_notice_text = "Ready";
static lv_color_t g_notice_color = lv_color_hex(0x9BE3B1);
static std::string g_last_action = "No IR sent yet";

static const char *aircon_mode_name(aircon::Mode mode)
{
    switch (mode)
    {
    case aircon::Mode::Auto: return "Auto";
    case aircon::Mode::Cool: return "Cool";
    case aircon::Mode::Heat: return "Heat";
    case aircon::Mode::Dry: return "Dry";
    case aircon::Mode::Fan: return "Fan";
    }
    return "Cool";
}

static const char *aircon_fan_name(aircon::Fan fan)
{
    switch (fan)
    {
    case aircon::Fan::Auto: return "Auto";
    case aircon::Fan::Low: return "Low";
    case aircon::Fan::Medium: return "Medium";
    case aircon::Fan::High: return "High";
    }
    return "Auto";
}

static const char *receive_mode_name(ReceiveMode mode)
{
    switch (mode)
    {
    case ReceiveMode::None: return "idle";
    case ReceiveMode::CopyRecord: return "copy-record";
    case ReceiveMode::NecMonitor: return "nec-monitor";
    }
    return "idle";
}

static std::string trim_copy(std::string text)
{
    while (!text.empty() &&
           (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t'))
    {
        text.pop_back();
    }

    size_t pos = 0;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t'))
    {
        ++pos;
    }
    return text.substr(pos);
}

static std::string shorten_text(const std::string &text, std::size_t max_len)
{
    if (text.size() <= max_len)
    {
        return text;
    }
    if (max_len <= 3)
    {
        return text.substr(0, max_len);
    }
    return text.substr(0, max_len - 3) + "...";
}

static std::string get_home_dir()
{
    const char *home = getenv("HOME");
    if (home && home[0] != '\0')
    {
        return home;
    }

    passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir)
    {
        return pw->pw_dir;
    }
    return ".";
}

static std::string get_config_dir()
{
    return get_home_dir() + "/.config/m5cardputerzero/irremote";
}

static std::string get_state_path()
{
    return get_config_dir() + "/state.ini";
}

static std::string get_signal_path()
{
    return get_config_dir() + "/signals.db";
}

static void ensure_dir(const std::string &path)
{
    if (path.empty())
    {
        return;
    }

    std::string current;
    for (char ch : path)
    {
        current.push_back(ch);
        if (ch == '/' && current != "/")
        {
            mkdir(current.c_str(), 0755);
        }
    }
    mkdir(path.c_str(), 0755);
}

static std::vector<std::string> split_tab(const std::string &line)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= line.size())
    {
        size_t end = line.find('\t', start);
        parts.push_back(end == std::string::npos ? line.substr(start) : line.substr(start, end - start));
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }
    return parts;
}

static std::vector<uint8_t> parse_hex_bytes(const std::string &text)
{
    std::vector<uint8_t> bytes;
    std::string compact;
    for (char ch : text)
    {
        if (std::isxdigit(static_cast<unsigned char>(ch)))
        {
            compact.push_back(ch);
        }
    }

    if (compact.size() % 2 != 0)
    {
        return bytes;
    }

    for (size_t i = 0; i < compact.size(); i += 2)
    {
        char pair[3] = {compact[i], compact[i + 1], '\0'};
        char *end = nullptr;
        long value = std::strtol(pair, &end, 16);
        if (end == pair)
        {
            bytes.clear();
            return bytes;
        }
        bytes.push_back(static_cast<uint8_t>(value));
    }
    return bytes;
}

static uint16_t parse_u16(const std::string &text)
{
    char *end = nullptr;
    long value = std::strtol(text.c_str(), &end, 0);
    if (end == text.c_str())
    {
        return 0;
    }
    if (value < 0)
    {
        value = 0;
    }
    if (value > 0xffff)
    {
        value = 0xffff;
    }
    return static_cast<uint16_t>(value);
}

static std::vector<uint32_t> parse_raw_timings(const std::string &text)
{
    std::vector<uint32_t> values;
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find(',', start);
        std::string part = trim_copy(end == std::string::npos ? text.substr(start) : text.substr(start, end - start));
        if (!part.empty())
        {
            char *parse_end = nullptr;
            unsigned long parsed = std::strtoul(part.c_str(), &parse_end, 10);
            if (parse_end != part.c_str() && parsed > 0)
            {
                values.push_back(static_cast<uint32_t>(parsed));
            }
        }
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }
    return values;
}

static std::string format_raw_timings(const std::vector<uint32_t> &timings)
{
    std::ostringstream out;
    for (std::size_t i = 0; i < timings.size(); ++i)
    {
        if (i > 0)
        {
            out << ",";
        }
        out << timings[i];
    }
    return out.str();
}

static std::string preview_raw_timings(const std::vector<uint32_t> &timings)
{
    if (timings.empty())
    {
        return "--";
    }

    std::ostringstream out;
    const std::size_t limit = std::min<std::size_t>(timings.size(), 5);
    for (std::size_t i = 0; i < limit; ++i)
    {
        if (i > 0)
        {
            out << "/";
        }
        out << timings[i];
    }
    if (timings.size() > limit)
    {
        out << "...";
    }
    return out.str();
}

static std::string signal_protocol_name(const StoredSignal &signal)
{
    return signal.protocol.empty() ? "RAW" : signal.protocol;
}

static constexpr int kCopyItemRowStart = 1;

static bool is_copy_recording()
{
    return g_receive_mode == ReceiveMode::CopyRecord;
}

static bool is_nec_monitoring()
{
    return g_receive_mode == ReceiveMode::NecMonitor;
}

static void set_notice(const std::string &text, lv_color_t color)
{
    g_notice_text = text;
    g_notice_color = color;
    if (g_notice != nullptr)
    {
        lv_label_set_text(g_notice, g_notice_text.c_str());
        lv_obj_set_style_text_color(g_notice, g_notice_color, 0);
    }
}

static void set_page_notice()
{
    if (g_page == AppPage::Aircon)
    {
        set_notice("8 Edit", lv_color_hex(0x78C8FF));
        return;
    }
    if (g_page == AppPage::Copy)
    {
        set_notice(is_copy_recording() ? "Recording" : "Enter New", lv_color_hex(0x78C8FF));
        return;
    }
    set_notice(is_nec_monitoring() ? "Live NEC" : "NEC Page", lv_color_hex(0x78C8FF));
}

static std::string describe_device(const irremote::DeviceInfo &info)
{
    std::ostringstream out;
    out << "path=" << (info.lirc_path.empty() ? "-" : info.lirc_path)
        << " rc=" << (info.rc_name.empty() ? "-" : info.rc_name)
        << " driver=" << (info.driver_name.empty() ? "-" : info.driver_name)
        << " name=" << (info.device_name.empty() ? "-" : info.device_name)
        << " available=" << (info.available ? "yes" : "no");
    if (!info.error_message.empty())
    {
        out << " error=" << info.error_message;
    }
    return out.str();
}

static void save_signals()
{
    ensure_dir(get_config_dir());
    std::ofstream out(get_signal_path(), std::ios::trunc);
    if (!out.is_open())
    {
        set_notice("Save fail", lv_color_hex(0xFFB37A));
        return;
    }

    for (const StoredSignal &signal : g_signals)
    {
        out << "RAW" << '\t'
            << signal.name << '\t'
            << signal_protocol_name(signal) << '\t'
            << irremote::format_nec_address(signal.address) << '\t'
            << irremote::format_nec_command(signal.command) << '\t'
            << irremote::format_hex_bytes(signal.data) << '\t'
            << format_raw_timings(signal.raw_timings) << '\n';
    }
}

static void normalize_selection()
{
    if (g_signals.empty())
    {
        g_selected_signal = -1;
        g_copy_cursor = 0;
        return;
    }

    if (g_selected_signal < 0)
    {
        g_selected_signal = 0;
    }
    if (g_selected_signal >= static_cast<int>(g_signals.size()))
    {
        g_selected_signal = static_cast<int>(g_signals.size()) - 1;
    }

    if (g_copy_cursor >= kCopyItemRowStart)
    {
        int signal_index = g_copy_cursor - kCopyItemRowStart;
        if (signal_index >= static_cast<int>(g_signals.size()))
        {
            g_copy_cursor = kCopyItemRowStart + static_cast<int>(g_signals.size()) - 1;
        }
    }
}

static void load_signals()
{
    g_signals.clear();
    std::ifstream in(get_signal_path());
    if (!in.is_open())
    {
        normalize_selection();
        return;
    }

    std::string line;
    while (std::getline(in, line))
    {
        line = trim_copy(line);
        if (line.empty())
        {
            continue;
        }

        std::vector<std::string> parts = split_tab(line);
        StoredSignal signal;

        if (parts.size() >= 7 && parts[0] == "RAW")
        {
            signal.name = trim_copy(parts[1]);
            signal.protocol = trim_copy(parts[2]);
            signal.address = parse_u16(parts[3]);
            signal.command = parse_u16(parts[4]);
            signal.data = parse_hex_bytes(parts[5]);
            signal.raw_timings = parse_raw_timings(parts[6]);
        }
        else if (parts.size() >= 5)
        {
            signal.name = trim_copy(parts[0]);
            signal.protocol = trim_copy(parts[1]);
            signal.address = parse_u16(parts[2]);
            signal.command = parse_u16(parts[3]);
            signal.data = parse_hex_bytes(parts[4]);
        }

        if (signal.name.empty())
        {
            continue;
        }
        if (signal.data.size() == 4)
        {
            irremote::parse_nec_bytes(signal.data, &signal.address, &signal.command, &signal.protocol);
        }
        if (signal.protocol.empty())
        {
            signal.protocol = "RAW";
        }
        g_signals.push_back(signal);
    }

    normalize_selection();
}

static void save_state()
{
    ensure_dir(get_config_dir());
    std::ofstream out(get_state_path(), std::ios::trunc);
    if (!out.is_open())
    {
        return;
    }

    const char *page_name = "copy";
    if (g_page == AppPage::Aircon)
    {
        page_name = "aircon";
    }
    else if (g_page == AppPage::Nec)
    {
        page_name = "nec";
    }

    out << "page=" << page_name << '\n';
    out << "copy_cursor=" << g_copy_cursor << '\n';
    out << "selected_signal=" << g_selected_signal << '\n';
    out << "aircon_cursor=" << g_aircon_cursor << '\n';
    out << "nec_cursor=" << g_nec_cursor << '\n';
    out << "aircon_brand=" << g_aircon_brand << '\n';
    out << "aircon_power=" << (g_aircon_power ? 1 : 0) << '\n';
    out << "aircon_temp=" << g_aircon_temp << '\n';
    out << "aircon_mode=" << static_cast<int>(g_aircon_mode) << '\n';
    out << "aircon_fan=" << static_cast<int>(g_aircon_fan) << '\n';
    out << "aircon_swing=" << (g_aircon_swing ? 1 : 0) << '\n';
}

static void load_state()
{
    std::ifstream in(get_state_path());
    if (!in.is_open())
    {
        return;
    }

    std::string line;
    while (std::getline(in, line))
    {
        size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }

        std::string key = trim_copy(line.substr(0, eq));
        std::string value = trim_copy(line.substr(eq + 1));
        if (key == "page")
        {
            g_page = value == "aircon" ? AppPage::Aircon : (value == "nec" ? AppPage::Nec : AppPage::Copy);
        }
        else if (key == "copy_cursor")
        {
            g_copy_cursor = std::max(0, std::atoi(value.c_str()));
        }
        else if (key == "selected_signal")
        {
            g_selected_signal = std::atoi(value.c_str());
        }
        else if (key == "aircon_cursor")
        {
            g_aircon_cursor = std::max(0, std::atoi(value.c_str()));
        }
        else if (key == "nec_cursor")
        {
            g_nec_cursor = std::max(0, std::atoi(value.c_str()));
        }
        else if (key == "aircon_brand")
        {
            g_aircon_brand = static_cast<std::size_t>(
                std::max(0, std::min(static_cast<int>(aircon::brand_count() - 1), std::atoi(value.c_str()))));
        }
        else if (key == "aircon_power")
        {
            g_aircon_power = std::atoi(value.c_str()) != 0;
        }
        else if (key == "aircon_temp")
        {
            g_aircon_temp = std::max(16, std::min(30, std::atoi(value.c_str())));
        }
        else if (key == "aircon_mode")
        {
            g_aircon_mode = static_cast<aircon::Mode>(std::max(0, std::min(4, std::atoi(value.c_str()))));
        }
        else if (key == "aircon_fan")
        {
            g_aircon_fan = static_cast<aircon::Fan>(std::max(0, std::min(3, std::atoi(value.c_str()))));
        }
        else if (key == "aircon_swing")
        {
            g_aircon_swing = std::atoi(value.c_str()) != 0;
        }
    }
}

static const StoredSignal *current_signal()
{
    if (g_signals.empty() || g_selected_signal < 0 || g_selected_signal >= static_cast<int>(g_signals.size()))
    {
        return nullptr;
    }
    return &g_signals[static_cast<std::size_t>(g_selected_signal)];
}

static int current_row_count()
{
    if (g_page == AppPage::Copy)
    {
        return std::max(1, kCopyItemRowStart + static_cast<int>(g_signals.size()));
    }
    if (g_page == AppPage::Aircon)
    {
        return 8;
    }
    return 6;
}

static int &page_cursor()
{
    if (g_page == AppPage::Copy)
    {
        return g_copy_cursor;
    }
    if (g_page == AppPage::Aircon)
    {
        return g_aircon_cursor;
    }
    return g_nec_cursor;
}

static int &page_scroll()
{
    if (g_page == AppPage::Copy)
    {
        return g_copy_scroll;
    }
    if (g_page == AppPage::Aircon)
    {
        return g_aircon_scroll;
    }
    return g_nec_scroll;
}

static void sync_scroll()
{
    int rows = current_row_count();
    int &cursor = page_cursor();
    int &scroll = page_scroll();
    if (rows <= 0)
    {
        cursor = 0;
        scroll = 0;
        return;
    }

    cursor = std::max(0, std::min(cursor, rows - 1));
    if (cursor < scroll)
    {
        scroll = cursor;
    }
    if (cursor >= scroll + kVisibleRows)
    {
        scroll = cursor - kVisibleRows + 1;
    }
    scroll = std::max(0, std::min(scroll, std::max(0, rows - kVisibleRows)));

    if (g_page == AppPage::Copy && cursor >= kCopyItemRowStart && !g_signals.empty())
    {
        g_selected_signal = std::min(cursor - kCopyItemRowStart, static_cast<int>(g_signals.size()) - 1);
    }
}

static std::string next_signal_name()
{
    int index = static_cast<int>(g_signals.size()) + 1;
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "Signal %02d", index);
    return buffer;
}

static StoredSignal *current_signal_mut()
{
    if (g_signals.empty() || g_selected_signal < 0 || g_selected_signal >= static_cast<int>(g_signals.size()))
    {
        return nullptr;
    }
    return &g_signals[static_cast<std::size_t>(g_selected_signal)];
}

static bool live_snapshot_has_nec()
{
    return g_last_snapshot.data.size() == 4 &&
           !g_last_snapshot.protocol.empty() &&
           g_last_snapshot.protocol != "RAW";
}

static void record_last_action(const std::string &label, const irremote::SendResult &result)
{
    if (result.protocol == "RAW" || result.data.empty())
    {
        g_last_action = label + "  raw " + std::to_string(result.raw_timings.size()) + " ticks";
    }
    else
    {
        g_last_action = label + "  " + irremote::format_nec_address(result.address) + "/" +
                        irremote::format_nec_command(result.command);
    }
}

static bool send_signal_raw(const StoredSignal &signal, const std::string &label)
{
    applog::info("UI replay request label=" + label +
                 " protocol=" + signal.protocol +
                 " raw_ticks=" + std::to_string(signal.raw_timings.size()) +
                 " data=" + irremote::format_hex_bytes(signal.data));
    irremote::SendResult result;
    if (!signal.raw_timings.empty())
    {
        result = irremote::send_raw_timings(signal.raw_timings);
    }
    else if (!signal.data.empty())
    {
        result = irremote::send_nec_bytes(signal.data);
    }
    else
    {
        set_notice("No data", lv_color_hex(0xFFB37A));
        return false;
    }

    if (!result.success)
    {
        applog::warn("UI replay failed label=" + label +
                     " path=" + result.device_path +
                     " message=" + result.message);
        set_notice(result.message.empty() ? "Send fail" : result.message, lv_color_hex(0xFFB37A));
        return false;
    }

    applog::info("UI replay ok label=" + label +
                 " path=" + result.device_path +
                 " message=" + result.message);
    record_last_action(label, result);
    set_notice("Sent", lv_color_hex(0x9BE3B1));
    save_state();
    return true;
}

static bool send_signal_nec(const StoredSignal &signal, const std::string &label)
{
    if (signal.data.size() != 4)
    {
        set_notice("No NEC", lv_color_hex(0xFFD57A));
        return false;
    }

    applog::info("UI NEC send request label=" + label +
                 " data=" + irremote::format_hex_bytes(signal.data));
    irremote::SendResult result = irremote::send_nec_bytes(signal.data);
    if (!result.success)
    {
        applog::warn("UI NEC send failed label=" + label +
                     " path=" + result.device_path +
                     " message=" + result.message);
        set_notice(result.message.empty() ? "Send fail" : result.message, lv_color_hex(0xFFB37A));
        return false;
    }

    applog::info("UI NEC send ok label=" + label +
                 " path=" + result.device_path +
                 " message=" + result.message);
    record_last_action(label, result);
    set_notice("NEC sent", lv_color_hex(0x9BE3B1));
    save_state();
    return true;
}

static bool send_live_nec()
{
    if (!live_snapshot_has_nec())
    {
        set_notice("No NEC", lv_color_hex(0xFFD57A));
        return false;
    }

    StoredSignal signal;
    signal.name = "Live NEC";
    signal.protocol = g_last_snapshot.protocol;
    signal.address = g_last_snapshot.address;
    signal.command = g_last_snapshot.command;
    signal.data = g_last_snapshot.data;
    signal.raw_timings = g_last_snapshot.raw_timings;
    return send_signal_nec(signal, signal.name);
}

static aircon::State current_aircon_state()
{
    aircon::State state;
    state.brand = aircon::brand_from_index(g_aircon_brand);
    state.power = g_aircon_power;
    state.temp_c = g_aircon_temp;
    state.mode = g_aircon_mode;
    state.fan = g_aircon_fan;
    state.swing = g_aircon_swing;
    return aircon::clamp_state(state);
}

static bool send_aircon_action(aircon::Action action,
                               const aircon::State &before,
                               const aircon::State &after,
                               const std::string &label)
{
    aircon::BuildResult built = aircon::build_command(before, after, action);
    if (!built.success || built.raw_timings.empty())
    {
        applog::warn("UI aircon build failed brand=" + std::string(aircon::brand_name(g_aircon_brand)) +
                     " label=" + label +
                     " message=" + built.message);
        set_notice(built.message.empty() ? "Build fail" : built.message, lv_color_hex(0xFFB37A));
        return false;
    }

    applog::info("UI aircon send request brand=" + std::string(aircon::brand_name(g_aircon_brand)) +
                 " label=" + label +
                 " timings=" + std::to_string(built.raw_timings.size()));
    irremote::SendResult result = irremote::send_raw_timings(built.raw_timings);
    if (!result.success)
    {
        applog::warn("UI aircon send failed brand=" + std::string(aircon::brand_name(g_aircon_brand)) +
                     " label=" + label +
                     " path=" + result.device_path +
                     " message=" + result.message);
        set_notice(result.message.empty() ? "Send fail" : result.message, lv_color_hex(0xFFB37A));
        return false;
    }

    applog::info("UI aircon send ok brand=" + std::string(aircon::brand_name(g_aircon_brand)) +
                 " label=" + label +
                 " path=" + result.device_path +
                 " message=" + result.message);
    record_last_action(label, result);
    set_notice("AC sent", lv_color_hex(0x9BE3B1));
    save_state();
    return true;
}

static void stop_receiver_mode()
{
    if (g_receive_mode != ReceiveMode::None || g_receiver.snapshot().opened)
    {
        applog::info("UI receiver stop mode=" + std::string(receive_mode_name(g_receive_mode)));
        g_receiver.stop();
    }
    g_receive_mode = ReceiveMode::None;
}

static bool start_receiver_mode(ReceiveMode mode)
{
    if (g_receive_mode == mode && g_receiver.snapshot().opened)
    {
        return true;
    }

    stop_receiver_mode();

    const bool raw_only = mode == ReceiveMode::CopyRecord;
    if (!g_receiver.start(0, false, raw_only))
    {
        g_last_snapshot = g_receiver.snapshot();
        applog::warn("UI receiver start failed mode=" + std::string(receive_mode_name(mode)) +
                     " message=" + g_last_snapshot.message);
        set_notice(g_last_snapshot.message.empty() ? "RX fail" : g_last_snapshot.message,
                   lv_color_hex(0xFFB37A));
        return false;
    }

    g_receive_mode = mode;
    g_last_snapshot = g_receiver.snapshot();
    applog::info("UI receiver start mode=" + std::string(receive_mode_name(mode)) +
                 " path=" + g_last_snapshot.device_path);
    return true;
}

static bool start_copy_record()
{
    const StoredSignal *signal = current_signal();
    if (signal == nullptr)
    {
        set_notice("Press N", lv_color_hex(0xFFD57A));
        return false;
    }

    if (start_receiver_mode(ReceiveMode::CopyRecord))
    {
        set_notice("Recording", lv_color_hex(0xFFD57A));
        return true;
    }
    return false;
}

static void stop_copy_record(const std::string &message)
{
    stop_receiver_mode();
    set_notice(message, lv_color_hex(0xFFD57A));
}

static void ensure_nec_monitor()
{
    if (g_page != AppPage::Nec)
    {
        if (is_nec_monitoring())
        {
            stop_receiver_mode();
        }
        return;
    }

    if (start_receiver_mode(ReceiveMode::NecMonitor))
    {
        set_notice(live_snapshot_has_nec() ? "NEC ready" : "Listening", lv_color_hex(0x78C8FF));
    }
}

static void cycle_selected_signal()
{
    if (g_signals.empty())
    {
        g_selected_signal = -1;
        return;
    }
    g_selected_signal = (g_selected_signal + 1 + static_cast<int>(g_signals.size())) % static_cast<int>(g_signals.size());
}

static void delete_selected_signal()
{
    if (is_copy_recording())
    {
        stop_copy_record("Recording stopped");
    }

    if (g_selected_signal < 0 || g_selected_signal >= static_cast<int>(g_signals.size()))
    {
        set_notice("No item", lv_color_hex(0xFFD57A));
        return;
    }

    std::string name = g_signals[static_cast<std::size_t>(g_selected_signal)].name;
    g_signals.erase(g_signals.begin() + g_selected_signal);
    if (g_selected_signal >= static_cast<int>(g_signals.size()))
    {
        g_selected_signal = static_cast<int>(g_signals.size()) - 1;
    }
    normalize_selection();
    save_signals();
    set_notice("Deleted", lv_color_hex(0xFFD57A));
}

static void create_copy_item(bool auto_record)
{
    StoredSignal signal;
    signal.name = next_signal_name();
    signal.protocol = "RAW";
    g_signals.push_back(signal);
    g_selected_signal = static_cast<int>(g_signals.size()) - 1;
    g_copy_cursor = kCopyItemRowStart + g_selected_signal;
    save_signals();
    save_state();
    set_notice("Created", lv_color_hex(0x82E3B5));

    if (auto_record)
    {
        start_copy_record();
    }
}

static void replay_selected_signal()
{
    const StoredSignal *signal = current_signal();
    if (signal == nullptr)
    {
        set_notice("No item", lv_color_hex(0xFFD57A));
        return;
    }
    if (signal->raw_timings.empty())
    {
        set_notice("No raw", lv_color_hex(0xFFD57A));
        return;
    }
    send_signal_raw(*signal, signal->name);
}

static void activate_current_row()
{
    if (g_page == AppPage::Copy)
    {
        if (g_copy_cursor == 0)
        {
            if (is_copy_recording())
            {
                stop_copy_record("Recording stopped");
            }
            else
            {
                create_copy_item(true);
            }
        }
        else
        {
            g_selected_signal = std::min(g_copy_cursor - kCopyItemRowStart, static_cast<int>(g_signals.size()) - 1);
            replay_selected_signal();
        }
        save_state();
        return;
    }

    if (g_page == AppPage::Aircon)
    {
        const aircon::State before = current_aircon_state();
        switch (g_aircon_cursor)
        {
        case 0:
            set_notice("8 Brand", lv_color_hex(0xFFD57A));
            break;
        case 1:
        {
            aircon::State after = before;
            after.power = !after.power;
            if (send_aircon_action(aircon::Action::PowerToggle, before, after, "Power"))
            {
                g_aircon_power = after.power;
            }
            break;
        }
        case 2:
        {
            aircon::State after = before;
            after.power = true;
            after.temp_c = std::min(30, after.temp_c + 1);
            if (send_aircon_action(aircon::Action::TempUp, before, after, "Temp +"))
            {
                g_aircon_power = after.power;
                g_aircon_temp = after.temp_c;
            }
            break;
        }
        case 3:
        {
            aircon::State after = before;
            after.power = true;
            after.temp_c = std::max(16, after.temp_c - 1);
            if (send_aircon_action(aircon::Action::TempDown, before, after, "Temp -"))
            {
                g_aircon_power = after.power;
                g_aircon_temp = after.temp_c;
            }
            break;
        }
        case 4:
        {
            aircon::State after = before;
            after.power = true;
            after.mode = g_aircon_mode;
            if (send_aircon_action(aircon::Action::ApplyMode, before, after,
                                   std::string("Mode ") + aircon_mode_name(g_aircon_mode)))
            {
                g_aircon_power = after.power;
            }
            break;
        }
        case 5:
        {
            aircon::State after = before;
            after.power = true;
            after.fan = g_aircon_fan;
            if (send_aircon_action(aircon::Action::ApplyFan, before, after,
                                   std::string("Fan ") + aircon_fan_name(g_aircon_fan)))
            {
                g_aircon_power = after.power;
            }
            break;
        }
        case 6:
        {
            aircon::State after = before;
            after.power = true;
            after.swing = g_aircon_swing;
            if (send_aircon_action(aircon::Action::ToggleSwing, before, after,
                                   std::string("Swing ") + (g_aircon_swing ? "On" : "Off")))
            {
                g_aircon_power = after.power;
            }
            break;
        }
        case 7:
        {
            aircon::State after = before;
            if (send_aircon_action(aircon::Action::SendState, before, after, "State"))
            {
                g_aircon_power = after.power;
            }
            break;
        }
        }

        save_state();
        return;
    }

    if (g_nec_cursor == 0)
    {
        if (is_nec_monitoring())
        {
            stop_receiver_mode();
            set_notice("Mon off", lv_color_hex(0xFFD57A));
        }
        else
        {
            ensure_nec_monitor();
        }
    }
    else if (g_nec_cursor == 5)
    {
        send_live_nec();
    }

    save_state();
}

static void adjust_current_row()
{
    if (g_page == AppPage::Copy)
    {
        if (g_copy_cursor >= kCopyItemRowStart && !g_signals.empty())
        {
            g_selected_signal = std::min(g_copy_cursor - kCopyItemRowStart, static_cast<int>(g_signals.size()) - 1);
            delete_selected_signal();
        }
        else
        {
            set_notice("Enter New", lv_color_hex(0xFFD57A));
        }
        save_state();
        return;
    }

    if (g_page == AppPage::Aircon)
    {
        switch (g_aircon_cursor)
        {
        case 0:
            g_aircon_brand = (g_aircon_brand + 1) % aircon::brand_count();
            set_notice(shorten_text(aircon::brand_name(g_aircon_brand), 10), lv_color_hex(0x78C8FF));
            break;
        case 4:
            g_aircon_mode = static_cast<aircon::Mode>((static_cast<int>(g_aircon_mode) + 1) % 5);
            set_notice(std::string("Mode ") + aircon_mode_name(g_aircon_mode), lv_color_hex(0x78C8FF));
            break;
        case 5:
            g_aircon_fan = static_cast<aircon::Fan>((static_cast<int>(g_aircon_fan) + 1) % 4);
            set_notice(std::string("Fan ") + aircon_fan_name(g_aircon_fan), lv_color_hex(0x78C8FF));
            break;
        case 6:
            g_aircon_swing = !g_aircon_swing;
            set_notice(g_aircon_swing ? "Swing On" : "Swing Off", lv_color_hex(0x78C8FF));
            break;
        default:
            set_notice("8 Edit", lv_color_hex(0xFFD57A));
            break;
        }

        save_state();
        return;
    }

    if (g_nec_cursor == 0)
    {
        if (is_nec_monitoring())
        {
            stop_receiver_mode();
            set_notice("Mon off", lv_color_hex(0xFFD57A));
        }
        else
        {
            ensure_nec_monitor();
        }
    }
    else
    {
        set_notice("Live NEC", lv_color_hex(0x78C8FF));
    }
    save_state();
}

static ListRow build_copy_row(int index)
{
    ListRow row;
    if (index == 0)
    {
        row.title = "New";
        row.value = is_copy_recording() ? "Listening..." : "Create & learn";
        row.accent = lv_color_hex(is_copy_recording() ? 0xFFD57A : 0x82E3B5);
    }
    else
    {
        int signal_index = index - kCopyItemRowStart;
        if (signal_index >= 0 && signal_index < static_cast<int>(g_signals.size()))
        {
            const StoredSignal &signal = g_signals[static_cast<std::size_t>(signal_index)];
            row.title = shorten_text(signal.name, 20);
            row.value = is_copy_recording() && signal_index == g_selected_signal ? "Listening..." : "";
            row.accent = signal_index == g_selected_signal ? lv_color_hex(0x82E3B5) : lv_color_hex(0x78C8FF);
        }
        else
        {
            row.title = "No Copies";
            row.value = "";
            row.accent = lv_color_hex(0x78C8FF);
        }
    }
    return row;
}

static ListRow build_aircon_row(int index)
{
    ListRow row;
    switch (index)
    {
    case 0:
        row.title = "Brand";
        row.value = shorten_text(aircon::brand_name(g_aircon_brand), 11);
        row.accent = lv_color_hex(0x82E3B5);
        break;
    case 1:
        row.title = "Power";
        row.value = g_aircon_power ? "On > Off" : "Off > On";
        row.accent = lv_color_hex(g_aircon_power ? 0x82E3B5 : 0xFF9F76);
        break;
    case 2:
        row.title = "Temp +";
        row.value = "";
        row.accent = lv_color_hex(0xFFD57A);
        break;
    case 3:
        row.title = "Temp -";
        row.value = "";
        row.accent = lv_color_hex(0x78C8FF);
        break;
    case 4:
        row.title = "Mode";
        row.value = aircon_mode_name(g_aircon_mode);
        row.accent = lv_color_hex(0x82E3B5);
        break;
    case 5:
        row.title = "Fan";
        row.value = aircon_fan_name(g_aircon_fan);
        row.accent = lv_color_hex(0x78C8FF);
        break;
    case 6:
        row.title = "Swing";
        row.value = g_aircon_swing ? "On / Auto" : "Off";
        row.accent = lv_color_hex(0xFFD57A);
        break;
    case 7:
        row.title = "Send";
        row.value = std::string(g_aircon_power ? "On " : "Off ") + std::to_string(g_aircon_temp) + "C";
        row.accent = lv_color_hex(0x82E3B5);
        break;
    }
    return row;
}

static ListRow build_nec_row(int index)
{
    ListRow row;
    switch (index)
    {
    case 0:
        row.title = "Monitor";
        row.value = is_nec_monitoring() ? "Live" : "Paused";
        row.accent = lv_color_hex(0x82E3B5);
        break;
    case 1:
        row.title = "Decode";
        row.value = live_snapshot_has_nec() ? "NEC ready" :
                    (g_last_snapshot.signal_seen ? "Raw only" : "Waiting");
        row.accent = live_snapshot_has_nec() ? lv_color_hex(0x82E3B5) : lv_color_hex(0xFFD57A);
        break;
    case 2:
        row.title = "Proto";
        row.value = g_last_snapshot.protocol.empty() ? "--" : g_last_snapshot.protocol;
        row.accent = lv_color_hex(0x78C8FF);
        break;
    case 3:
        row.title = "Address";
        row.value = live_snapshot_has_nec() ? irremote::format_nec_address(g_last_snapshot.address) : "--";
        row.accent = lv_color_hex(0x78C8FF);
        break;
    case 4:
        row.title = "Command";
        row.value = live_snapshot_has_nec() ? irremote::format_nec_command(g_last_snapshot.command) : "--";
        row.accent = lv_color_hex(0x78C8FF);
        break;
    case 5:
        row.title = "Send NEC";
        row.value = live_snapshot_has_nec() ? shorten_text(irremote::format_hex_bytes(g_last_snapshot.data), 11) : "N/A";
        row.accent = live_snapshot_has_nec() ? lv_color_hex(0x82E3B5) : lv_color_hex(0xFF9F76);
        break;
    }
    return row;
}

static std::string left_title_text()
{
    if (g_page == AppPage::Copy)
    {
        return "Copy";
    }
    if (g_page == AppPage::Aircon)
    {
        return "A/C";
    }
    return "NEC";
}

static std::string build_left_body()
{
    std::ostringstream out;
    if (g_page == AppPage::Copy)
    {
        out << "";
        return out.str();
    }

    if (g_page == AppPage::Aircon)
    {
        out << "TX:" << (g_sender_info.available ? "ok" : "--") << "\n";
        out << "BR:" << shorten_text(aircon::brand_name(g_aircon_brand), 9) << "\n";
        out << "PWR:" << (g_aircon_power ? "On" : "Off") << "\n";
        out << "TMP:" << g_aircon_temp << "C\n";
        out << "MOD:" << aircon_mode_name(g_aircon_mode) << "\n";
        out << "FAN:" << aircon_fan_name(g_aircon_fan) << "\n";
        out << "SW:" << (g_aircon_swing ? "On" : "Off") << "\n";
        out << shorten_text(g_last_action, 12);
        return out.str();
    }

    out << "RX:" << (g_receiver_info.available ? "ok" : "--") << "\n";
    out << "MON:" << (is_nec_monitoring() ? "on" : "off") << "\n";
    out << "PT:" << shorten_text(g_last_snapshot.protocol.empty() ? "--" : g_last_snapshot.protocol, 8) << "\n";
    out << "AD:" << (live_snapshot_has_nec() ? irremote::format_nec_address(g_last_snapshot.address) : "--") << "\n";
    out << "CM:" << (live_snapshot_has_nec() ? irremote::format_nec_command(g_last_snapshot.command) : "--") << "\n";
    out << "RAW:" << g_last_snapshot.raw_timings.size() << "t\n";
    out << shorten_text(preview_raw_timings(g_last_snapshot.raw_timings), 12) << "\n";
    out << shorten_text(g_last_action, 12);
    return out.str();
}

static void refresh_ui()
{
    normalize_selection();
    sync_scroll();

    lv_label_set_text(g_title,
                      g_page == AppPage::Aircon ? "A/C" :
                      (g_page == AppPage::Copy ? "COPY" : "NEC"));
    if (g_page == AppPage::Aircon)
    {
        lv_label_set_text(g_subtitle, "");
    }
    else if (g_page == AppPage::Copy)
    {
        lv_label_set_text(g_subtitle, "");
    }
    else
    {
        lv_label_set_text(g_subtitle, "");
    }
    lv_label_set_text(g_notice, g_notice_text.c_str());
    lv_obj_set_style_text_color(g_notice, g_notice_color, 0);

    const bool copy_active = g_page == AppPage::Copy;
    const bool aircon_active = g_page == AppPage::Aircon;
    const bool nec_active = g_page == AppPage::Nec;

    lv_obj_set_style_text_color(g_copy_tab_label, copy_active ? lv_color_hex(0xF4FAFF) : lv_color_hex(0x92A7BE), 0);
    lv_obj_set_style_text_color(g_aircon_tab_label, aircon_active ? lv_color_hex(0xF4FAFF) : lv_color_hex(0x92A7BE), 0);
    lv_obj_set_style_text_color(g_nec_tab_label, nec_active ? lv_color_hex(0xF4FAFF) : lv_color_hex(0x92A7BE), 0);
    lv_obj_set_style_bg_opa(g_aircon_tab_cursor, aircon_active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(g_aircon_tab_cursor, lv_color_hex(0x82E3B5), 0);
    lv_obj_set_style_bg_opa(g_copy_tab_cursor, copy_active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(g_copy_tab_cursor, lv_color_hex(0x78C8FF), 0);
    lv_obj_set_style_bg_opa(g_nec_tab_cursor, nec_active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(g_nec_tab_cursor, lv_color_hex(0xFFD57A), 0);

    std::string left_title = left_title_text();
    std::string left_body = build_left_body();
    lv_label_set_text(g_left_title, left_title.c_str());
    lv_label_set_text(g_left_body, left_body.c_str());
    lv_label_set_text(g_right_title,
                      g_page == AppPage::Aircon ? "Actions" :
                      (g_page == AppPage::Copy ? "Copy List" : "Decode"));

    if (copy_active)
    {
        lv_obj_add_flag(g_left_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(g_right_card, 8, 34);
        lv_obj_set_size(g_right_card, 304, 112);
    }
    else
    {
        lv_obj_clear_flag(g_left_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(g_right_card, 104, 34);
        lv_obj_set_size(g_right_card, 208, 112);
    }

    const int rows = current_row_count();
    const int scroll = page_scroll();
    const int cursor = page_cursor();
    for (int i = 0; i < kVisibleRows; ++i)
    {
        const bool wide_copy_row = copy_active;
        lv_obj_set_pos(g_rows[static_cast<std::size_t>(i)], 8, 22 + i * 15);
        lv_obj_set_size(g_rows[static_cast<std::size_t>(i)], wide_copy_row ? 288 : 192, 15);
        lv_obj_set_width(g_row_titles[static_cast<std::size_t>(i)], wide_copy_row ? 180 : 90);
        lv_obj_set_pos(g_row_values[static_cast<std::size_t>(i)], wide_copy_row ? 190 : 100, 2);
        lv_obj_set_width(g_row_values[static_cast<std::size_t>(i)], wide_copy_row ? 88 : 86);

        int row_index = scroll + i;
        if (row_index < rows)
        {
            ListRow row = g_page == AppPage::Copy ? build_copy_row(row_index) :
                          (g_page == AppPage::Aircon ? build_aircon_row(row_index) : build_nec_row(row_index));
            bool selected = row_index == cursor;
            lv_obj_clear_flag(g_rows[static_cast<std::size_t>(i)], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(g_row_titles[static_cast<std::size_t>(i)], row.title.c_str());
            lv_label_set_text(g_row_values[static_cast<std::size_t>(i)], row.value.c_str());
            lv_obj_set_style_bg_color(g_rows[static_cast<std::size_t>(i)],
                                      selected ? lv_color_hex(0x183A55) : lv_color_hex(0x0F1824),
                                      0);
            lv_obj_set_style_border_color(g_rows[static_cast<std::size_t>(i)],
                                          selected ? row.accent : lv_color_hex(0x243B53),
                                          0);
            lv_obj_set_style_shadow_width(g_rows[static_cast<std::size_t>(i)], selected ? 10 : 0, 0);
            lv_obj_set_style_shadow_color(g_rows[static_cast<std::size_t>(i)], row.accent, 0);
            lv_obj_set_style_text_color(g_row_titles[static_cast<std::size_t>(i)],
                                        selected ? lv_color_hex(0xF5FAFF) : lv_color_hex(0xD6E4F2),
                                        0);
            lv_obj_set_style_text_color(g_row_values[static_cast<std::size_t>(i)],
                                        selected ? row.accent : lv_color_hex(0x8EA7BE),
                                        0);
        }
        else
        {
            lv_obj_add_flag(g_rows[static_cast<std::size_t>(i)], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (copy_active)
    {
        lv_label_set_text(g_footer, "4/6 Pg  5/7 Row  8 Del  N New  Ent Run");
    }
    else
    {
        lv_label_set_text(g_footer, "4/6 Pg  5/7 Row  8 Edit  N New  Ent");
    }
}

static void on_poll_timer(lv_timer_t *timer)
{
    (void)timer;
    if (g_receive_mode == ReceiveMode::None)
    {
        return;
    }

    const std::string last_message = g_last_snapshot.message;
    const bool last_seen = g_last_snapshot.signal_seen;
    g_last_snapshot = g_receiver.poll();

    if (g_receive_mode == ReceiveMode::CopyRecord)
    {
        if (g_last_snapshot.data_changed && !g_last_snapshot.raw_timings.empty())
        {
            StoredSignal *signal = current_signal_mut();
            if (signal == nullptr)
            {
                stop_copy_record("Target item missing");
            }
            else
            {
                signal->protocol = "RAW";
                signal->address = 0;
                signal->command = 0;
                signal->data.clear();
                signal->raw_timings = g_last_snapshot.raw_timings;
                save_signals();
                save_state();
                applog::info("UI recorded signal name=" + signal->name +
                             " protocol=" + signal->protocol +
                             " raw_ticks=" + std::to_string(signal->raw_timings.size()) +
                             " raw_preview=" + preview_raw_timings(signal->raw_timings));
                stop_receiver_mode();
                set_notice("Recorded", lv_color_hex(0x82E3B5));
            }
        }
        else if (!g_last_snapshot.message.empty() && g_last_snapshot.message != last_message)
        {
            applog::debug("UI copy poll message=" + g_last_snapshot.message);
            set_notice("Listening", lv_color_hex(0xFFD57A));
        }
    }
    else if (g_receive_mode == ReceiveMode::NecMonitor)
    {
        if (g_last_snapshot.data_changed)
        {
            applog::info("UI live decode update protocol=" + g_last_snapshot.protocol +
                         " address=" + irremote::format_nec_address(g_last_snapshot.address) +
                         " command=" + irremote::format_nec_command(g_last_snapshot.command) +
                         " raw_ticks=" + std::to_string(g_last_snapshot.raw_timings.size()) +
                         " data=" + irremote::format_hex_bytes(g_last_snapshot.data));
            set_notice(live_snapshot_has_nec() ? "NEC ready" : "Raw seen",
                       live_snapshot_has_nec() ? lv_color_hex(0x82E3B5) : lv_color_hex(0xFFD57A));
        }
        else if (!g_last_snapshot.message.empty() && g_last_snapshot.message != last_message)
        {
            applog::debug("UI nec poll message=" + g_last_snapshot.message);
        }
    }

    if (g_page == AppPage::Copy || g_page == AppPage::Nec ||
        g_last_snapshot.data_changed || g_last_snapshot.signal_seen != last_seen)
    {
        refresh_ui();
    }
}

static lv_obj_t *make_card(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 14, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x0D1621), 0);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(0x132535), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x22384C), 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *make_text(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, const char *text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

static void switch_page(int delta)
{
    const int page_count = 3;
    int index = static_cast<int>(g_page);
    index = (index + delta + page_count) % page_count;
    if (g_page == AppPage::Copy && is_copy_recording())
    {
        stop_copy_record("Recording stopped");
    }
    if (g_page == AppPage::Nec && is_nec_monitoring())
    {
        stop_receiver_mode();
    }
    g_page = static_cast<AppPage>(index);
    if (g_page == AppPage::Nec)
    {
        ensure_nec_monitor();
    }
    else
    {
        set_page_notice();
    }
    sync_scroll();
    save_state();
    refresh_ui();
}

static void move_cursor(int delta)
{
    int &cursor = page_cursor();
    cursor += delta;
    sync_scroll();
    save_state();
    refresh_ui();
}

static bool is_pressed_state(int key_state)
{
    return key_state == 1 || key_state == 2;
}

static bool matches_digit(uint32_t key_code, const char *utf8, char digit)
{
    if (utf8 != nullptr && utf8[0] == digit && utf8[1] == '\0')
    {
        return true;
    }

    switch (digit)
    {
    case '4': return key_code == KEY_4 || key_code == KEY_LEFT;
    case '5': return key_code == KEY_5 || key_code == KEY_UP;
    case '6': return key_code == KEY_6 || key_code == KEY_RIGHT;
    case '7': return key_code == KEY_7 || key_code == KEY_DOWN;
    case '8': return key_code == KEY_8 || key_code == KEY_TAB || key_code == KEY_SPACE;
    default: return false;
    }
}

}  // namespace

void ui_init()
{
    applog::begin_session("IrRemote startup");
    g_group = lv_group_create();

    load_state();
    load_signals();
    g_sender_info = irremote::probe_sender();
    g_receiver_info = irremote::probe_receiver();
    applog::info("Sender probe: " + describe_device(g_sender_info));
    applog::info("Receiver probe: " + describe_device(g_receiver_info));
    applog::info("Runtime log: " + applog::log_path());
    normalize_selection();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x071018), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x0B2030), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);

    lv_obj_t *header = make_card(screen, 8, 8, 304, 20);
    g_title = make_text(header, 10, 3, "A/C", lv_color_hex(0xF4FAFF));
    lv_obj_set_style_text_font(g_title, &lv_font_montserrat_14, 0);
    g_subtitle = make_text(header, 0, 0, "", lv_color_hex(0x8EA7BE));
    g_notice = make_text(header, 64, 4, "Ready", lv_color_hex(0x9BE3B1));
    lv_obj_set_width(g_notice, 104);
    lv_label_set_long_mode(g_notice, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(g_notice, &lv_font_montserrat_12, 0);

    g_aircon_tab = make_card(screen, 176, 8, 26, 18);
    lv_obj_set_style_bg_opa(g_aircon_tab, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_aircon_tab, 0, 0);
    lv_obj_set_style_radius(g_aircon_tab, 0, 0);
    g_aircon_tab_label = make_text(g_aircon_tab, 2, 1, "AC", lv_color_hex(0xF4FAFF));
    lv_obj_set_style_text_font(g_aircon_tab_label, &lv_font_montserrat_12, 0);
    g_aircon_tab_cursor = lv_obj_create(g_aircon_tab);
    lv_obj_remove_style_all(g_aircon_tab_cursor);
    lv_obj_set_pos(g_aircon_tab_cursor, 1, 15);
    lv_obj_set_size(g_aircon_tab_cursor, 22, 2);
    g_copy_tab = make_card(screen, 217, 8, 38, 18);
    lv_obj_set_style_bg_opa(g_copy_tab, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_copy_tab, 0, 0);
    lv_obj_set_style_radius(g_copy_tab, 0, 0);
    g_copy_tab_label = make_text(g_copy_tab, 0, 1, "COPY", lv_color_hex(0xF4FAFF));
    lv_obj_set_style_text_font(g_copy_tab_label, &lv_font_montserrat_12, 0);
    g_copy_tab_cursor = lv_obj_create(g_copy_tab);
    lv_obj_remove_style_all(g_copy_tab_cursor);
    lv_obj_set_pos(g_copy_tab_cursor, 0, 15);
    lv_obj_set_size(g_copy_tab_cursor, 34, 2);
    g_nec_tab = make_card(screen, 269, 8, 30, 18);
    lv_obj_set_style_bg_opa(g_nec_tab, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_nec_tab, 0, 0);
    lv_obj_set_style_radius(g_nec_tab, 0, 0);
    g_nec_tab_label = make_text(g_nec_tab, 1, 1, "NEC", lv_color_hex(0xF4FAFF));
    lv_obj_set_style_text_font(g_nec_tab_label, &lv_font_montserrat_12, 0);
    g_nec_tab_cursor = lv_obj_create(g_nec_tab);
    lv_obj_remove_style_all(g_nec_tab_cursor);
    lv_obj_set_pos(g_nec_tab_cursor, 0, 15);
    lv_obj_set_size(g_nec_tab_cursor, 26, 2);

    g_left_card = make_card(screen, 8, 34, 92, 112);
    g_left_title = make_text(g_left_card, 8, 6, "A/C", lv_color_hex(0xF4FAFF));
    lv_obj_set_style_text_font(g_left_title, &lv_font_montserrat_14, 0);
    g_left_body = make_text(g_left_card, 8, 22, "", lv_color_hex(0xA9BED2));
    lv_obj_set_width(g_left_body, 78);
    lv_label_set_long_mode(g_left_body, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(g_left_body, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_line_space(g_left_body, 0, 0);

    g_right_card = make_card(screen, 104, 34, 208, 112);
    g_right_title = make_text(g_right_card, 8, 6, "Actions", lv_color_hex(0xF4FAFF));
    lv_obj_set_style_text_font(g_right_title, &lv_font_montserrat_14, 0);
    for (int i = 0; i < kVisibleRows; ++i)
    {
        lv_obj_t *row = make_card(g_right_card, 8, 22 + i * 15, 192, 15);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x0F1824), 0);
        lv_obj_set_style_bg_grad_color(row, lv_color_hex(0x132535), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x243B53), 0);
        g_rows[static_cast<std::size_t>(i)] = row;
        g_row_titles[static_cast<std::size_t>(i)] = make_text(row, 7, 2, "", lv_color_hex(0xD6E4F2));
        lv_obj_set_width(g_row_titles[static_cast<std::size_t>(i)], 90);
        lv_label_set_long_mode(g_row_titles[static_cast<std::size_t>(i)], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(g_row_titles[static_cast<std::size_t>(i)], &lv_font_montserrat_12, 0);
        g_row_values[static_cast<std::size_t>(i)] = make_text(row, 100, 2, "", lv_color_hex(0x8EA7BE));
        lv_obj_set_width(g_row_values[static_cast<std::size_t>(i)], 86);
        lv_label_set_long_mode(g_row_values[static_cast<std::size_t>(i)], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(g_row_values[static_cast<std::size_t>(i)], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(g_row_values[static_cast<std::size_t>(i)], &lv_font_montserrat_12, 0);
    }

    lv_obj_t *footer_card = make_card(screen, 8, 148, 304, 14);
    g_footer = make_text(footer_card, 8, 1, "4/6 Pg  5/7 Row  8 Edit  Ent Send", lv_color_hex(0x8EA7BE));
    lv_obj_set_style_text_font(g_footer, &lv_font_montserrat_12, 0);

    lv_timer_create(on_poll_timer, 120, nullptr);
    if (g_page == AppPage::Nec)
    {
        ensure_nec_monitor();
    }
    else
    {
        set_page_notice();
    }
    refresh_ui();
}

lv_group_t *ui_get_input_group()
{
    return g_group;
}

void ui_handle_key_item(uint32_t key_code, const char *utf8, int key_state)
{
    if (!is_pressed_state(key_state))
    {
        return;
    }

    if (g_page == AppPage::Copy && utf8 != nullptr &&
        ((utf8[0] == 'n' || utf8[0] == 'N') && utf8[1] == '\0'))
    {
        create_copy_item(true);
        refresh_ui();
        return;
    }

    if (matches_digit(key_code, utf8, '4'))
    {
        switch_page(-1);
        return;
    }
    if (matches_digit(key_code, utf8, '6'))
    {
        switch_page(1);
        return;
    }
    if (matches_digit(key_code, utf8, '5'))
    {
        move_cursor(-1);
        return;
    }
    if (matches_digit(key_code, utf8, '7'))
    {
        move_cursor(1);
        return;
    }
    if (matches_digit(key_code, utf8, '8'))
    {
        adjust_current_row();
        refresh_ui();
        return;
    }
    if (key_code == KEY_ENTER || key_code == KEY_KPENTER || (utf8 != nullptr && utf8[0] == '\r'))
    {
        activate_current_row();
        refresh_ui();
    }
}

void ui_handle_lvgl_key(uint32_t key)
{
    switch (key)
    {
    case LV_KEY_LEFT:
        switch_page(-1);
        break;
    case LV_KEY_RIGHT:
        switch_page(1);
        break;
    case LV_KEY_UP:
        move_cursor(-1);
        break;
    case LV_KEY_DOWN:
        move_cursor(1);
        break;
    case LV_KEY_ENTER:
        activate_current_row();
        break;
    default:
        break;
    }
    refresh_ui();
}
