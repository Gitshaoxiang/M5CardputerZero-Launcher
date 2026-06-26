#include "hal_pty.h"
#include "lvgl/lvgl.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <pwd.h>
#include <sstream>
#include <string>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include <linux/input.h>

namespace {

struct Profile {
    std::string name;
    std::string host;
    std::string port = "22";
    std::string user = "pi";
};

enum class UiMode {
    Browse,
    Edit,
    DeleteConfirm,
    Connecting,
    Session,
};

enum class SessionEntryMode {
    Command,
    Secret,
};

static constexpr uint32_t kUiTickMs = 80;
static constexpr size_t kVisibleRows = 4;
static constexpr size_t kMaxProfiles = 24;
static constexpr size_t kMaxLogBytes = 8192;
static constexpr int kPtyCols = 40;
static constexpr int kPtyRows = 16;

static lv_group_t *g_group = nullptr;
static lv_timer_t *g_tick_timer = nullptr;

static lv_obj_t *g_title = nullptr;
static lv_obj_t *g_header_meta = nullptr;
static lv_obj_t *g_status_chip = nullptr;
static lv_obj_t *g_status_text = nullptr;
static lv_obj_t *g_list_panel = nullptr;
static lv_obj_t *g_detail_panel = nullptr;
static lv_obj_t *g_footer = nullptr;
static lv_obj_t *g_empty_hint = nullptr;
static lv_obj_t *g_profile_count = nullptr;

static std::array<lv_obj_t *, kVisibleRows> g_row_cards{};
static std::array<lv_obj_t *, kVisibleRows> g_row_titles{};
static std::array<lv_obj_t *, kVisibleRows> g_row_subtitles{};

static lv_obj_t *g_detail_name = nullptr;
static lv_obj_t *g_detail_host = nullptr;
static lv_obj_t *g_detail_port = nullptr;
static lv_obj_t *g_detail_user = nullptr;
static lv_obj_t *g_detail_hint = nullptr;

static lv_obj_t *g_edit_panel = nullptr;
static std::array<lv_obj_t *, 4> g_edit_rows{};
static std::array<lv_obj_t *, 4> g_edit_labels{};
static std::array<lv_obj_t *, 4> g_edit_values{};
static lv_obj_t *g_edit_help = nullptr;

static lv_obj_t *g_overlay = nullptr;
static lv_obj_t *g_overlay_card = nullptr;
static lv_obj_t *g_overlay_title = nullptr;
static lv_obj_t *g_overlay_body = nullptr;
static lv_obj_t *g_overlay_spinner = nullptr;

static lv_obj_t *g_session_panel = nullptr;
static lv_obj_t *g_session_label = nullptr;
static lv_obj_t *g_session_stats_panel = nullptr;
static lv_obj_t *g_cpu_label = nullptr;
static lv_obj_t *g_mem_label = nullptr;
static lv_obj_t *g_cpu_bar = nullptr;
static lv_obj_t *g_mem_bar = nullptr;
static lv_obj_t *g_cpu_value = nullptr;
static lv_obj_t *g_mem_value = nullptr;

static int g_screen_w = 320;
static int g_screen_h = 170;

static UiMode g_mode = UiMode::Browse;
static std::vector<Profile> g_profiles;
static int g_selected = 0;
static int g_scroll_top = 0;
static int g_edit_index = -1;
static int g_edit_field = 0;
static Profile g_draft;
static std::string g_last_notice = "Ready";

static hal_pty_t g_pty = nullptr;
static bool g_session_running = false;
static uint32_t g_connect_start_ms = 0;
static bool g_connect_got_output = false;
static std::string g_session_log;
static std::string g_active_endpoint;
static SessionEntryMode g_session_entry_mode = SessionEntryMode::Command;
static int g_remote_cpu_percent = -1;
static int g_remote_mem_percent = -1;
static bool g_stats_loop_started = false;
static int g_metrics_fd = -1;
static pid_t g_metrics_pid = -1;
static std::string g_metrics_buffer;
static std::string g_control_path;
static uint32_t g_metrics_last_attempt_ms = 0;
static std::string g_metrics_state = "Idle";
static std::string g_local_input_preview;
static uint32_t g_local_input_tick = 0;
static bool g_cursor_phase_on = true;
static bool g_terminal_alt_screen_active = false;

static const char *kFieldNames[4] = {"Label", "Host/IP", "Port", "User"};
static const char *kStatsMarker = "__M5SSH_STATS__";

static std::string trim_copy(std::string text)
{
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t'))
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

static std::string get_home_dir()
{
    const char *home = getenv("HOME");
    if (home && home[0] != '\0')
    {
        return home;
    }

    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir)
    {
        return pw->pw_dir;
    }
    return ".";
}

static std::string get_config_dir()
{
    return get_home_dir() + "/.config/m5cardputerzero/sshclient";
}

static std::string get_profiles_path()
{
    return get_config_dir() + "/profiles.ini";
}

static std::string quote_sh_arg(const std::string &text)
{
    std::string out = "'";
    for (char ch : text)
    {
        if (ch == '\'')
        {
            out += "'\"'\"'";
        }
        else
        {
            out.push_back(ch);
        }
    }
    out += "'";
    return out;
}

static std::string make_control_path()
{
    char buf[128];
    snprintf(buf, sizeof(buf), "/tmp/m5ssh-%d-%u.sock", (int)getpid(), (unsigned)lv_tick_get());
    return buf;
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
        if (ch == '/')
        {
            if (!current.empty() && current != "/")
            {
                mkdir(current.c_str(), 0755);
            }
        }
    }
    mkdir(path.c_str(), 0755);
}

static std::string profile_field(const Profile &profile, int idx)
{
    switch (idx)
    {
    case 0: return profile.name;
    case 1: return profile.host;
    case 2: return profile.port;
    case 3: return profile.user;
    default: return "";
    }
}

static void set_profile_field(Profile &profile, int idx, const std::string &value)
{
    switch (idx)
    {
    case 0: profile.name = value; break;
    case 1: profile.host = value; break;
    case 2: profile.port = value; break;
    case 3: profile.user = value; break;
    default: break;
    }
}

static std::string make_profile_summary(const Profile &profile)
{
    return profile.user + "@" + profile.host + ":" + (profile.port.empty() ? "22" : profile.port);
}

static std::string make_profile_endpoint(const Profile &profile)
{
    if (profile.host.empty())
    {
        return "No host selected";
    }

    std::string text = profile.host;
    if (!profile.port.empty() && profile.port != "22")
    {
        text += ":";
        text += profile.port;
    }
    return text;
}

static std::string to_lower_ascii(std::string text)
{
    for (char &ch : text)
    {
        ch = (char)std::tolower((unsigned char)ch);
    }
    return text;
}

static bool contains_text_ci(const std::string &haystack, const char *needle)
{
    if (needle == nullptr || needle[0] == '\0')
    {
        return false;
    }
    return to_lower_ascii(haystack).find(to_lower_ascii(needle)) != std::string::npos;
}

static bool starts_with_text(const std::string &text, const char *prefix)
{
    if (prefix == nullptr)
    {
        return false;
    }
    size_t len = std::strlen(prefix);
    return text.size() >= len && text.compare(0, len, prefix) == 0;
}

static bool line_has_stats_marker(const std::string &text)
{
    return text.find(kStatsMarker) != std::string::npos ||
           text.find("777;M5SSH;cpu=%s;mem=%s") != std::string::npos;
}

static void load_profiles()
{
    g_profiles.clear();
    std::ifstream ifs(get_profiles_path());
    if (!ifs.is_open())
    {
        return;
    }

    Profile current;
    bool in_profile = false;
    std::string line;
    while (std::getline(ifs, line))
    {
        line = trim_copy(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
        {
            continue;
        }

        if (line == "[profile]")
        {
            if (in_profile && !current.host.empty())
            {
                g_profiles.push_back(current);
                current = Profile {};
            }
            in_profile = true;
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }

        std::string key = trim_copy(line.substr(0, eq));
        std::string value = trim_copy(line.substr(eq + 1));
        if (key == "name") current.name = value;
        else if (key == "host") current.host = value;
        else if (key == "port") current.port = value;
        else if (key == "user") current.user = value;
    }

    if (in_profile && !current.host.empty())
    {
        g_profiles.push_back(current);
    }

    if ((int)g_profiles.size() > g_selected)
    {
        g_selected = std::max(0, g_selected);
    }
}

static void save_profiles()
{
    ensure_dir(get_config_dir());
    std::ofstream ofs(get_profiles_path(), std::ios::trunc);
    if (!ofs.is_open())
    {
        g_last_notice = "Save failed";
        return;
    }

    for (const Profile &profile : g_profiles)
    {
        ofs << "[profile]\n";
        ofs << "name=" << profile.name << "\n";
        ofs << "host=" << profile.host << "\n";
        ofs << "port=" << (profile.port.empty() ? "22" : profile.port) << "\n";
        ofs << "user=" << (profile.user.empty() ? "pi" : profile.user) << "\n\n";
    }
    g_last_notice = "Saved locally";
}

static void clamp_selection()
{
    if (g_profiles.empty())
    {
        g_selected = 0;
        g_scroll_top = 0;
        return;
    }

    g_selected = std::max(0, std::min(g_selected, (int)g_profiles.size() - 1));
    if (g_selected < g_scroll_top)
    {
        g_scroll_top = g_selected;
    }
    if (g_selected >= g_scroll_top + (int)kVisibleRows)
    {
        g_scroll_top = g_selected - (int)kVisibleRows + 1;
    }
    g_scroll_top = std::max(0, std::min(g_scroll_top, std::max(0, (int)g_profiles.size() - (int)kVisibleRows)));
}

static void close_session()
{
    if (g_metrics_fd >= 0)
    {
        close(g_metrics_fd);
        g_metrics_fd = -1;
    }
    if (g_metrics_pid > 0)
    {
        kill(g_metrics_pid, SIGKILL);
        waitpid(g_metrics_pid, nullptr, 0);
        g_metrics_pid = -1;
    }
    g_metrics_buffer.clear();

    if (g_pty != nullptr)
    {
        hal_pty_close(g_pty);
        g_pty = nullptr;
    }
    g_session_running = false;
    g_connect_got_output = false;
    g_session_entry_mode = SessionEntryMode::Command;
    g_remote_cpu_percent = -1;
    g_remote_mem_percent = -1;
    g_stats_loop_started = false;
    g_active_endpoint.clear();
    g_metrics_last_attempt_ms = 0;
    g_metrics_state = "Idle";
    g_local_input_preview.clear();
    g_local_input_tick = 0;
    g_terminal_alt_screen_active = false;
    if (!g_control_path.empty())
    {
        unlink(g_control_path.c_str());
        g_control_path.clear();
    }
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

static lv_obj_t *create_card(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 14, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x0B1420), 0);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(0x0F1C2B), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x20344D), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_shadow_width(obj, 18, 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(0x09131F), 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    return obj;
}

static void set_status(const char *text, lv_color_t color)
{
    if (g_status_text)
    {
        lv_label_set_text(g_status_text, text);
        lv_obj_set_style_text_color(g_status_text, color, 0);
    }
}

static void refresh_session_entry_ui()
{
    if (g_session_entry_mode == SessionEntryMode::Secret)
    {
        set_status("Password", lv_color_hex(0xD5963F));
    }
    else
    {
        const char *text = "Session ended";
        lv_color_t color = lv_color_hex(0xF2B56A);
        if (g_session_running)
        {
            if (g_remote_cpu_percent >= 0 || g_remote_mem_percent >= 0)
            {
                text = "Live+stats";
                color = lv_color_hex(0x89D6B0);
            }
            else if (g_metrics_state == "Error")
            {
                text = "Stats err";
                color = lv_color_hex(0xFFB38A);
            }
            else if (g_metrics_state == "Starting")
            {
                text = "Syncing";
                color = lv_color_hex(0x7FCBFF);
            }
            else
            {
                text = "Live session";
                color = lv_color_hex(0x89D6B0);
            }
        }
        set_status(text, color);
    }
}

static void set_session_entry_mode(SessionEntryMode mode)
{
    g_session_entry_mode = mode;
    refresh_session_entry_ui();
}

static void refresh_header_meta()
{
    if (g_header_meta == nullptr)
    {
        return;
    }

    std::string meta = "Local profiles";
    if (g_mode == UiMode::Connecting || g_mode == UiMode::Session)
    {
        meta = g_active_endpoint.empty() ? "SSH session" : g_active_endpoint;
    }
    else if (g_mode == UiMode::Edit)
    {
        meta = g_draft.host.empty() ? "Editing new host" : make_profile_endpoint(g_draft);
    }

    lv_label_set_text(g_header_meta, meta.c_str());
}

static void refresh_footer()
{
    if (g_footer == nullptr)
    {
        return;
    }

    switch (g_mode)
    {
    case UiMode::Browse:
        lv_label_set_text(g_footer, "ENT connect  N new  E edit  D del  ESC quit");
        break;
    case UiMode::Edit:
        lv_label_set_text(g_footer, "UP/DN field  Type  BACK del  ENT save  ESC back");
        break;
    case UiMode::DeleteConfirm:
        lv_label_set_text(g_footer, "Y delete  N cancel");
        break;
    case UiMode::Connecting:
        lv_label_set_text(g_footer, "Connecting... ESC cancel");
        break;
    case UiMode::Session:
        lv_label_set_text(g_footer, "");
        break;
    }
}

static void refresh_session_stats()
{
    if (g_session_stats_panel == nullptr)
    {
        return;
    }

    bool visible = g_mode == UiMode::Session;
    if (visible)
    {
        lv_obj_clear_flag(g_session_stats_panel, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(g_session_stats_panel, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    auto clamp_pct = [](int value) {
        return std::max(0, std::min(100, value));
    };

    if (g_remote_cpu_percent >= 0)
    {
        int cpu = clamp_pct(g_remote_cpu_percent);
        lv_bar_set_value(g_cpu_bar, cpu, LV_ANIM_OFF);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", cpu);
        lv_label_set_text(g_cpu_value, buf);
    }
    else
    {
        lv_bar_set_value(g_cpu_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(g_cpu_value, g_metrics_state == "Live" ? "--" : "..");
    }

    if (g_remote_mem_percent >= 0)
    {
        int mem = clamp_pct(g_remote_mem_percent);
        lv_bar_set_value(g_mem_bar, mem, LV_ANIM_OFF);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", mem);
        lv_label_set_text(g_mem_value, buf);
    }
    else
    {
        lv_bar_set_value(g_mem_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(g_mem_value, g_metrics_state == "Live" ? "--" : "..");
    }
}

static void refresh_list()
{
    clamp_selection();

    char count_buf[32];
    snprintf(count_buf, sizeof(count_buf), "%d saved", (int)g_profiles.size());
    lv_label_set_text(g_profile_count, count_buf);

    bool empty = g_profiles.empty();
    if (g_empty_hint)
    {
        if (empty)
        {
            lv_obj_clear_flag(g_empty_hint, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(g_empty_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }

    for (size_t i = 0; i < kVisibleRows; ++i)
    {
        int idx = g_scroll_top + (int)i;
        lv_obj_t *card = g_row_cards[i];
        if (idx >= (int)g_profiles.size())
        {
            lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        lv_obj_clear_flag(card, LV_OBJ_FLAG_HIDDEN);
        const Profile &profile = g_profiles[(size_t)idx];
        bool active = idx == g_selected;
        lv_obj_set_style_bg_color(card, active ? lv_color_hex(0x123557) : lv_color_hex(0x0E1825), 0);
        lv_obj_set_style_border_color(card, active ? lv_color_hex(0x4BA8FF) : lv_color_hex(0x1C2B3D), 0);
        lv_obj_set_style_shadow_opa(card, active ? LV_OPA_20 : LV_OPA_0, 0);

        lv_label_set_text(g_row_titles[i], profile.name.empty() ? "(unnamed)" : profile.name.c_str());
        lv_obj_set_style_text_color(g_row_titles[i], active ? lv_color_hex(0xF5FAFF) : lv_color_hex(0xD8E8F7), 0);

        std::string sub = make_profile_summary(profile);
        lv_label_set_text(g_row_subtitles[i], sub.c_str());
        lv_obj_set_style_text_color(g_row_subtitles[i], active ? lv_color_hex(0x9ED0FF) : lv_color_hex(0x7993AA), 0);
    }
}

static void refresh_detail()
{
    if (g_profiles.empty())
    {
        lv_label_set_text(g_detail_name, "No profiles");
        lv_label_set_text(g_detail_host, "Add one with N");
        lv_label_set_text(g_detail_port, "--");
        lv_label_set_text(g_detail_user, "--");
        lv_label_set_text(g_detail_hint, "");
        return;
    }

    const Profile &profile = g_profiles[(size_t)g_selected];
    lv_label_set_text(g_detail_name, profile.name.empty() ? "(unnamed)" : profile.name.c_str());

    std::string host = profile.host;
    std::string port = ":" + (profile.port.empty() ? std::string("22") : profile.port);
    std::string user = profile.user.empty() ? std::string("pi") : profile.user;

    lv_label_set_text(g_detail_host, host.c_str());
    lv_label_set_text(g_detail_port, port.c_str());
    lv_label_set_text(g_detail_user, user.c_str());
    lv_label_set_text(g_detail_hint, "");
}

static void refresh_edit_form()
{
    for (int i = 0; i < 4; ++i)
    {
        bool active = i == g_edit_field;
        lv_obj_set_style_bg_color(g_edit_rows[(size_t)i], active ? lv_color_hex(0x123557) : lv_color_hex(0x0E1825), 0);
        lv_obj_set_style_border_color(g_edit_rows[(size_t)i], active ? lv_color_hex(0x4BA8FF) : lv_color_hex(0x1C2B3D), 0);
        lv_obj_set_style_border_width(g_edit_rows[(size_t)i], active ? 1 : 0, 0);
        lv_obj_set_style_text_color(g_edit_labels[(size_t)i], active ? lv_color_hex(0xA8D7FF) : lv_color_hex(0x7D98B0), 0);

        std::string value = profile_field(g_draft, i);
        if (active)
        {
            value.push_back('_');
        }
        lv_label_set_text(g_edit_values[(size_t)i], value.c_str());
        lv_obj_set_style_text_color(g_edit_values[(size_t)i], active ? lv_color_hex(0xF7FBFF) : lv_color_hex(0xD7E6F5), 0);
    }

    lv_label_set_text(g_edit_help, g_edit_index >= 0 ? "Editing existing profile" : "Creating a new profile");
}

static void hide_all_primary_views()
{
    lv_obj_add_flag(g_list_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_detail_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_edit_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_session_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_session_stats_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_footer, LV_OBJ_FLAG_HIDDEN);
}

static void apply_mode()
{
    hide_all_primary_views();
    refresh_footer();
    refresh_header_meta();

    switch (g_mode)
    {
    case UiMode::Browse:
        lv_obj_clear_flag(g_list_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_detail_panel, LV_OBJ_FLAG_HIDDEN);
        refresh_list();
        refresh_detail();
        set_status("Profiles", lv_color_hex(0x89D6B0));
        break;
    case UiMode::Edit:
        lv_obj_clear_flag(g_edit_panel, LV_OBJ_FLAG_HIDDEN);
        refresh_edit_form();
        set_status("Editing", lv_color_hex(0x7FCBFF));
        break;
    case UiMode::DeleteConfirm:
        lv_obj_clear_flag(g_list_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_detail_panel, LV_OBJ_FLAG_HIDDEN);
        refresh_list();
        refresh_detail();
        lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_overlay_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_overlay_title, "Delete profile?");
        lv_label_set_text(g_overlay_body, g_profiles.empty() ? "Nothing to delete" : g_profiles[(size_t)g_selected].name.c_str());
        set_status("Confirm", lv_color_hex(0xFFD27A));
        break;
    case UiMode::Connecting:
        lv_obj_clear_flag(g_list_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_detail_panel, LV_OBJ_FLAG_HIDDEN);
        refresh_list();
        refresh_detail();
        lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_overlay_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_overlay_title, "Connecting");
        if (!g_profiles.empty())
        {
            lv_label_set_text(g_overlay_body, make_profile_summary(g_profiles[(size_t)g_selected]).c_str());
        }
        set_status("Opening SSH", lv_color_hex(0x7FCBFF));
        break;
    case UiMode::Session:
        lv_obj_clear_flag(g_session_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_footer, LV_OBJ_FLAG_HIDDEN);
        refresh_session_entry_ui();
        refresh_session_stats();
        break;
    }
}

static std::string escape_terminal_markup(const std::string &text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (char ch : text)
    {
        if (ch == '#')
        {
            out += "\\#";
        }
        else if (ch == '\t')
        {
            out += "    ";
        }
        else
        {
            out.push_back(ch);
        }
    }
    return out;
}

static std::string format_local_preview(const std::string &text)
{
    std::string out;
    out.reserve(text.size() * 2 + 8);
    for (char ch : text)
    {
        if (ch == ' ')
        {
            out += ".";
        }
        else if (ch == '\t')
        {
            out += "->";
        }
        else if (ch == '#')
        {
            out += "\\#";
        }
        else
        {
            out.push_back(ch);
        }
    }
    return out;
}

static bool looks_like_shell_prompt(const std::string &line)
{
    if (line.empty())
    {
        return false;
    }

    std::string trimmed = trim_copy(line);
    if (trimmed.empty())
    {
        return false;
    }
    if (contains_text_ci(trimmed, "password") || contains_text_ci(trimmed, "passphrase"))
    {
        return false;
    }

    char tail = trimmed.back();
    if ((tail == '$' || tail == '#' || tail == '%' || tail == '>') && trimmed.size() <= 96)
    {
        return true;
    }

    if (trimmed.size() >= 2)
    {
        std::string tail2 = trimmed.substr(trimmed.size() - 2);
        if ((tail2 == "$ " || tail2 == "# " || tail2 == "% " || tail2 == "> ") && trimmed.size() <= 96)
        {
            return true;
        }
    }
    return false;
}

static void append_terminal_line(std::string &out, const std::string &line)
{
    if (line_has_stats_marker(line))
    {
        return;
    }

    const char *color = nullptr;
    if (starts_with_text(line, "$ ssh "))
    {
        color = "56B3FF";
    }
    else if (starts_with_text(line, "[session ended"))
    {
        color = "E6A65C";
    }
    else if (contains_text_ci(line, "password:") || contains_text_ci(line, "passphrase"))
    {
        color = "D5963F";
    }
    else if (contains_text_ci(line, "permission denied") ||
             contains_text_ci(line, "connection refused") ||
             contains_text_ci(line, "timed out") ||
             contains_text_ci(line, "no route to host"))
    {
        color = "FF8D7A";
    }
    else if (contains_text_ci(line, "warning:") || contains_text_ci(line, "host key"))
    {
        color = "FFD27A";
    }
    else if (looks_like_shell_prompt(line))
    {
        color = "89D6B0";
    }

    std::string escaped = escape_terminal_markup(line);
    if (color != nullptr && !escaped.empty())
    {
        out += "#";
        out += color;
        out += " ";
        out += escaped;
        out += "#";
    }
    else
    {
        out += escaped;
    }
}

enum class TermColor : uint8_t {
    Default = 0,
    Gray,
    Red,
    Green,
    Yellow,
    Blue,
    Magenta,
    Cyan,
    White,
};

struct TerminalCell {
    char ch = ' ';
    TermColor color = TermColor::Default;
};

struct TerminalRow {
    std::array<TerminalCell, kPtyCols> cells {};
};

struct TerminalScreenState {
    std::array<TerminalRow, kPtyRows> rows {};
    std::array<TerminalRow, kPtyRows> saved_rows {};
    int cursor_row = 0;
    int cursor_col = 0;
    int saved_cursor_row = 0;
    int saved_cursor_col = 0;
    bool saved_screen_valid = false;
    bool alt_screen = false;
    bool cursor_visible = true;
    TermColor current_color = TermColor::Default;
    bool bold = false;
    int latest_cpu = -1;
    int latest_mem = -1;
};

static const char *term_color_to_hex(TermColor color)
{
    switch (color)
    {
    case TermColor::Gray: return "7E8A96";
    case TermColor::Red: return "FF8D7A";
    case TermColor::Green: return "89D6B0";
    case TermColor::Yellow: return "FFD27A";
    case TermColor::Blue: return "56B3FF";
    case TermColor::Magenta: return "C59BFF";
    case TermColor::Cyan: return "6FD9D2";
    case TermColor::White: return "EAF4FF";
    case TermColor::Default:
    default: return nullptr;
    }
}

static TermColor ansi_basic_to_term_color(int code, bool bold)
{
    switch (code)
    {
    case 30: return bold ? TermColor::White : TermColor::Gray;
    case 31:
    case 91: return TermColor::Red;
    case 32:
    case 92: return TermColor::Green;
    case 33:
    case 93: return TermColor::Yellow;
    case 34:
    case 94: return TermColor::Blue;
    case 35:
    case 95: return TermColor::Magenta;
    case 36:
    case 96: return TermColor::Cyan;
    case 37:
    case 97: return TermColor::White;
    default: return TermColor::Default;
    }
}

static TermColor rgb_to_term_color(int r, int g, int b)
{
    if (r > 220 && g > 220 && b > 220) return TermColor::White;
    if (r < 90 && g < 90 && b < 90) return TermColor::Gray;
    if (r >= g + 40 && r >= b + 40) return TermColor::Red;
    if (g >= r + 30 && g >= b + 20) return TermColor::Green;
    if (b >= r + 30 && b >= g + 20) return TermColor::Blue;
    if (r > 180 && g > 150 && b < 140) return TermColor::Yellow;
    if (r > 160 && b > 160) return TermColor::Magenta;
    if (g > 140 && b > 140) return TermColor::Cyan;
    return TermColor::White;
}

static TermColor ansi_256_to_term_color(int code)
{
    static const TermColor kTable[16] = {
        TermColor::Gray,    TermColor::Red,     TermColor::Green,  TermColor::Yellow,
        TermColor::Blue,    TermColor::Magenta, TermColor::Cyan,   TermColor::White,
        TermColor::Gray,    TermColor::Red,     TermColor::Green,  TermColor::Yellow,
        TermColor::Blue,    TermColor::Magenta, TermColor::Cyan,   TermColor::White,
    };

    if (code >= 0 && code < 16)
    {
        return kTable[code];
    }
    if (code >= 232)
    {
        int shade = 8 + (code - 232) * 10;
        return rgb_to_term_color(shade, shade, shade);
    }
    if (code >= 16 && code <= 231)
    {
        int n = code - 16;
        int r = n / 36;
        int g = (n / 6) % 6;
        int b = n % 6;
        auto level = [](int v) {
            return v == 0 ? 0 : 55 + v * 40;
        };
        return rgb_to_term_color(level(r), level(g), level(b));
    }
    return TermColor::Default;
}

static std::vector<int> parse_csi_params(const std::string &params)
{
    std::vector<int> values;
    std::stringstream ss(params);
    std::string token;
    while (std::getline(ss, token, ';'))
    {
        values.push_back(token.empty() ? -1 : std::atoi(token.c_str()));
    }
    if (values.empty())
    {
        values.push_back(-1);
    }
    return values;
}

static void apply_ansi_sgr_codes(const std::string &params, TermColor &current_color, bool &bold)
{
    std::vector<int> values = parse_csi_params(params);
    for (size_t i = 0; i < values.size(); ++i)
    {
        int code = values[i] < 0 ? 0 : values[i];
        if (code == 0)
        {
            current_color = TermColor::Default;
            bold = false;
        }
        else if (code == 1)
        {
            bold = true;
        }
        else if (code == 22)
        {
            bold = false;
        }
        else if (code == 39)
        {
            current_color = TermColor::Default;
        }
        else if (code == 38 && i + 1 < values.size())
        {
            if (values[i + 1] == 5 && i + 2 < values.size())
            {
                current_color = ansi_256_to_term_color(std::max(0, values[i + 2]));
                i += 2;
            }
            else if (values[i + 1] == 2 && i + 4 < values.size())
            {
                current_color = rgb_to_term_color(std::max(0, values[i + 2]),
                                                  std::max(0, values[i + 3]),
                                                  std::max(0, values[i + 4]));
                i += 4;
            }
        }
        else
        {
            TermColor next = ansi_basic_to_term_color(code, bold);
            if (next != TermColor::Default || code == 30 || code == 37 || code == 90 || code == 97)
            {
                current_color = next;
            }
        }
    }
}

static bool parse_remote_stats_osc(const std::string &payload, int &cpu_percent, int &mem_percent)
{
    if (!starts_with_text(payload, "777;M5SSH;"))
    {
        return false;
    }

    int cpu = -1;
    int mem = -1;
    size_t pos = std::strlen("777;M5SSH;");
    while (pos < payload.size())
    {
        size_t next = payload.find(';', pos);
        std::string part = payload.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        size_t eq = part.find('=');
        if (eq != std::string::npos)
        {
            std::string key = part.substr(0, eq);
            int value = std::atoi(part.substr(eq + 1).c_str());
            if (key == "cpu") cpu = value;
            else if (key == "mem") mem = value;
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }

    if (cpu >= 0) cpu_percent = std::max(0, std::min(100, cpu));
    if (mem >= 0) mem_percent = std::max(0, std::min(100, mem));
    return cpu >= 0 || mem >= 0;
}

static std::string build_remote_stats_command()
{
    return
        "while :; do "
        "set -- $(awk '/^cpu /{print $2,$3,$4,$5,$6,$7,$8}' /proc/stat 2>/dev/null); "
        "idle=$(($4+$5)); total=$(($1+$2+$3+$4+$5+$6+$7)); "
        "if [ -n \"${__m5_prev_total:-}\" ]; then diff_total=$((total-__m5_prev_total)); diff_idle=$((idle-__m5_prev_idle)); "
        "if [ $diff_total -gt 0 ]; then cpu=$(((100*(diff_total-diff_idle))/diff_total)); else cpu=0; fi; "
        "else cpu=0; fi; "
        "__m5_prev_total=$total; __m5_prev_idle=$idle; "
        "mem=$(awk '/MemTotal:/ {t=$2} /MemAvailable:/ {a=$2} END { if (t > 0) printf \"%d\", ((t-a)*100)/t; else printf \"0\" }' /proc/meminfo 2>/dev/null); "
        "printf \"" "__M5SSH_STATS__" " cpu=%s mem=%s\\n\" \"$cpu\" \"$mem\"; "
        "sleep 3; "
        "done";
}

static bool parse_remote_stats_line(const std::string &line, int &cpu_percent, int &mem_percent)
{
    if (!starts_with_text(line, kStatsMarker))
    {
        return false;
    }

    int cpu = -1;
    int mem = -1;
    std::stringstream ss(line);
    std::string part;
    while (ss >> part)
    {
        size_t eq = part.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        std::string key = part.substr(0, eq);
        int value = std::atoi(part.substr(eq + 1).c_str());
        if (key == "cpu") cpu = value;
        else if (key == "mem") mem = value;
    }

    if (cpu >= 0) cpu_percent = std::max(0, std::min(100, cpu));
    if (mem >= 0) mem_percent = std::max(0, std::min(100, mem));
    return cpu >= 0 || mem >= 0;
}

static void poll_metrics_channel()
{
    if (g_metrics_fd < 0)
    {
        return;
    }

    char buf[256];
    while (true)
    {
        ssize_t n = read(g_metrics_fd, buf, sizeof(buf));
        if (n > 0)
        {
            g_metrics_buffer.append(buf, (size_t)n);
            size_t pos = 0;
            while (true)
            {
                size_t end = g_metrics_buffer.find('\n', pos);
                if (end == std::string::npos)
                {
                    g_metrics_buffer.erase(0, pos);
                    break;
                }

                std::string line = trim_copy(g_metrics_buffer.substr(pos, end - pos));
                int cpu = -1;
                int mem = -1;
                if (parse_remote_stats_line(line, cpu, mem))
                {
                    if (cpu >= 0) g_remote_cpu_percent = cpu;
                    if (mem >= 0) g_remote_mem_percent = mem;
                    g_metrics_state = "Live";
                    refresh_session_entry_ui();
                    refresh_session_stats();
                }
                else if (!line.empty() && !starts_with_text(line, "Warning:"))
                {
                    g_metrics_state = "Error";
                    refresh_session_entry_ui();
                    refresh_session_stats();
                }
                pos = end + 1;
            }
            continue;
        }

        if (n == 0)
        {
            if (g_metrics_state != "Live")
            {
                g_metrics_state = "Error";
                refresh_session_entry_ui();
                refresh_session_stats();
            }
            close(g_metrics_fd);
            g_metrics_fd = -1;
            break;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            break;
        }

        close(g_metrics_fd);
        g_metrics_fd = -1;
        g_metrics_state = "Error";
        refresh_session_entry_ui();
        refresh_session_stats();
        break;
    }

    if (g_metrics_pid > 0)
    {
        int status = 0;
        pid_t r = waitpid(g_metrics_pid, &status, WNOHANG);
        if (r == g_metrics_pid)
        {
            g_metrics_pid = -1;
            if (g_metrics_fd >= 0)
            {
                close(g_metrics_fd);
                g_metrics_fd = -1;
            }
            if (g_metrics_state != "Live")
            {
                g_metrics_state = "Error";
                refresh_session_entry_ui();
                refresh_session_stats();
            }
            g_stats_loop_started = false;
        }
    }
}

static void start_metrics_channel()
{
    if (g_metrics_fd >= 0 || g_metrics_pid > 0 || !g_session_running || g_control_path.empty())
    {
        return;
    }

    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0)
    {
        return;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return;
    }

    if (pid == 0)
    {
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);

        const Profile &profile = g_profiles[(size_t)g_selected];
        std::string port = profile.port.empty() ? "22" : profile.port;
        std::string remote = profile.user + "@" + profile.host;
        std::string control = "ControlPath=" + g_control_path;
        std::string remote_cmd = "sh -lc " + quote_sh_arg(build_remote_stats_command());

        std::vector<const char *> argv;
        argv.push_back("ssh");
        argv.push_back("-T");
        argv.push_back("-o");
        argv.push_back("BatchMode=yes");
        argv.push_back("-o");
        argv.push_back("StrictHostKeyChecking=no");
        argv.push_back("-o");
        argv.push_back("UserKnownHostsFile=/dev/null");
        argv.push_back("-o");
        argv.push_back(control.c_str());
        argv.push_back("-p");
        argv.push_back(port.c_str());
        argv.push_back(remote.c_str());
        argv.push_back(remote_cmd.c_str());
        argv.push_back(nullptr);
        execvp("ssh", (char *const *)argv.data());
        _exit(127);
    }

    close(pipe_fds[1]);
    int flags = fcntl(pipe_fds[0], F_GETFL);
    fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);
    g_metrics_fd = pipe_fds[0];
    g_metrics_pid = pid;
    g_stats_loop_started = true;
    g_metrics_state = "Starting";
    g_metrics_buffer.clear();
    refresh_session_entry_ui();
    refresh_session_stats();
}

static void clear_terminal_row(TerminalRow &row)
{
    for (TerminalCell &cell : row.cells)
    {
        cell.ch = ' ';
        cell.color = TermColor::Default;
    }
}

static void clear_terminal_screen(std::array<TerminalRow, kPtyRows> &rows)
{
    for (TerminalRow &row : rows)
    {
        clear_terminal_row(row);
    }
}

static void init_terminal_screen(TerminalScreenState &state)
{
    clear_terminal_screen(state.rows);
    clear_terminal_screen(state.saved_rows);
}

static void scroll_terminal_up(TerminalScreenState &state)
{
    for (int row = 1; row < kPtyRows; ++row)
    {
        state.rows[(size_t)(row - 1)] = state.rows[(size_t)row];
    }
    clear_terminal_row(state.rows[(size_t)(kPtyRows - 1)]);
}

static void terminal_line_feed(TerminalScreenState &state)
{
    if (state.cursor_row >= kPtyRows - 1)
    {
        scroll_terminal_up(state);
    }
    else
    {
        ++state.cursor_row;
    }
}

static void terminal_carriage_return(TerminalScreenState &state)
{
    state.cursor_col = 0;
}

static void terminal_write_char(TerminalScreenState &state, char ch)
{
    if (ch == '\t')
    {
        int next = ((state.cursor_col / 4) + 1) * 4;
        while (state.cursor_col < next)
        {
            terminal_write_char(state, ' ');
        }
        return;
    }

    if (state.cursor_row < 0) state.cursor_row = 0;
    if (state.cursor_row >= kPtyRows) state.cursor_row = kPtyRows - 1;
    if (state.cursor_col < 0) state.cursor_col = 0;
    if (state.cursor_col >= kPtyCols) state.cursor_col = kPtyCols - 1;

    TerminalCell &cell = state.rows[(size_t)state.cursor_row].cells[(size_t)state.cursor_col];
    cell.ch = ch;
    cell.color = state.current_color;

    ++state.cursor_col;
    if (state.cursor_col >= kPtyCols)
    {
        state.cursor_col = 0;
        terminal_line_feed(state);
    }
}

static void terminal_erase_in_line(TerminalScreenState &state, int mode)
{
    int from = 0;
    int to = kPtyCols - 1;
    if (mode == 0)
    {
        from = state.cursor_col;
    }
    else if (mode == 1)
    {
        to = state.cursor_col;
    }

    from = std::max(0, std::min(from, kPtyCols - 1));
    to = std::max(0, std::min(to, kPtyCols - 1));
    if (from > to)
    {
        return;
    }
    for (int col = from; col <= to; ++col)
    {
        TerminalCell &cell = state.rows[(size_t)state.cursor_row].cells[(size_t)col];
        cell.ch = ' ';
        cell.color = TermColor::Default;
    }
}

static void terminal_erase_in_display(TerminalScreenState &state, int mode)
{
    if (mode == 2)
    {
        clear_terminal_screen(state.rows);
        state.cursor_row = 0;
        state.cursor_col = 0;
        return;
    }

    if (mode == 0)
    {
        terminal_erase_in_line(state, 0);
        for (int row = state.cursor_row + 1; row < kPtyRows; ++row)
        {
            clear_terminal_row(state.rows[(size_t)row]);
        }
    }
    else if (mode == 1)
    {
        terminal_erase_in_line(state, 1);
        for (int row = 0; row < state.cursor_row; ++row)
        {
            clear_terminal_row(state.rows[(size_t)row]);
        }
    }
}

static void terminal_set_cursor(TerminalScreenState &state, int row, int col)
{
    state.cursor_row = std::max(0, std::min(kPtyRows - 1, row));
    state.cursor_col = std::max(0, std::min(kPtyCols - 1, col));
}

static bool terminal_row_is_blank(const TerminalRow &row)
{
    for (const TerminalCell &cell : row.cells)
    {
        if (cell.ch != ' ')
        {
            return false;
        }
    }
    return true;
}

static std::string terminal_row_plain_text(const TerminalRow &row)
{
    std::string line;
    line.reserve(kPtyCols);
    int last = -1;
    for (int col = 0; col < kPtyCols; ++col)
    {
        if (row.cells[(size_t)col].ch != ' ')
        {
            last = col;
        }
    }
    if (last < 0)
    {
        return "";
    }
    for (int col = 0; col <= last; ++col)
    {
        line.push_back(row.cells[(size_t)col].ch);
    }
    return line;
}

static std::string render_terminal_row_markup(const TerminalRow &row, int row_index,
                                              int cursor_row, int cursor_col, bool show_cursor)
{
    int last_non_blank = -1;
    for (int col = 0; col < kPtyCols; ++col)
    {
        bool cursor_here = show_cursor && row_index == cursor_row && col == cursor_col;
        char ch = row.cells[(size_t)col].ch;
        if (ch != ' ' || cursor_here)
        {
            last_non_blank = col;
        }
    }

    if (last_non_blank < 0)
    {
        return "";
    }

    std::string out;
    out.reserve((size_t)(last_non_blank + 1) * 3);
    TermColor open_color = TermColor::Default;
    for (int col = 0; col <= last_non_blank; ++col)
    {
        const TerminalCell &cell = row.cells[(size_t)col];
        bool cursor_here = show_cursor && row_index == cursor_row && col == cursor_col;
        char ch = cell.ch;
        TermColor color = cell.color;

        if (cursor_here)
        {
            color = TermColor::White;
            if (ch == ' ')
            {
                ch = '_';
            }
        }

        if (color != open_color)
        {
            if (open_color != TermColor::Default)
            {
                out += "#";
            }
            if (color != TermColor::Default)
            {
                out += "#";
                out += term_color_to_hex(color);
                out += " ";
            }
            open_color = color;
        }

        if (ch == '#')
        {
            out += "\\#";
        }
        else
        {
            out.push_back(ch);
        }
    }

    if (open_color != TermColor::Default)
    {
        out += "#";
    }
    return out;
}

static void handle_csi_sequence(TerminalScreenState &state, const std::string &params, char command)
{
    bool private_mode = !params.empty() && params[0] == '?';
    std::string body = private_mode ? params.substr(1) : params;
    std::vector<int> values = parse_csi_params(body);
    auto param_or = [&](size_t index, int fallback) {
        if (index >= values.size() || values[index] < 0)
        {
            return fallback;
        }
        return values[index];
    };

    if (private_mode)
    {
        int mode = param_or(0, 0);
        if (command == 'h' && (mode == 1049 || mode == 47 || mode == 1047))
        {
            state.saved_rows = state.rows;
            state.saved_cursor_row = state.cursor_row;
            state.saved_cursor_col = state.cursor_col;
            state.saved_screen_valid = true;
            state.alt_screen = true;
            state.cursor_visible = true;
            clear_terminal_screen(state.rows);
            state.cursor_row = 0;
            state.cursor_col = 0;
        }
        else if (command == 'l' && (mode == 1049 || mode == 47 || mode == 1047))
        {
            if (state.saved_screen_valid)
            {
                state.rows = state.saved_rows;
                state.cursor_row = state.saved_cursor_row;
                state.cursor_col = state.saved_cursor_col;
            }
            state.alt_screen = false;
        }
        else if (mode == 25)
        {
            state.cursor_visible = command == 'h';
        }
        return;
    }

    switch (command)
    {
    case 'A':
        terminal_set_cursor(state, state.cursor_row - param_or(0, 1), state.cursor_col);
        break;
    case 'B':
        terminal_set_cursor(state, state.cursor_row + param_or(0, 1), state.cursor_col);
        break;
    case 'C':
        terminal_set_cursor(state, state.cursor_row, state.cursor_col + param_or(0, 1));
        break;
    case 'D':
        terminal_set_cursor(state, state.cursor_row, state.cursor_col - param_or(0, 1));
        break;
    case 'G':
        terminal_set_cursor(state, state.cursor_row, param_or(0, 1) - 1);
        break;
    case 'H':
    case 'f':
        terminal_set_cursor(state, param_or(0, 1) - 1, param_or(1, 1) - 1);
        break;
    case 'd':
        terminal_set_cursor(state, param_or(0, 1) - 1, state.cursor_col);
        break;
    case 'J':
        terminal_erase_in_display(state, param_or(0, 0));
        break;
    case 'K':
        terminal_erase_in_line(state, param_or(0, 0));
        break;
    case 'm':
        apply_ansi_sgr_codes(body, state.current_color, state.bold);
        break;
    case 's':
        state.saved_cursor_row = state.cursor_row;
        state.saved_cursor_col = state.cursor_col;
        break;
    case 'u':
        terminal_set_cursor(state, state.saved_cursor_row, state.saved_cursor_col);
        break;
    default:
        break;
    }
}

static std::string render_terminal_markup(const std::string &raw)
{
    enum class EscState { Normal, Esc, Csi, Osc };

    TerminalScreenState state {};
    init_terminal_screen(state);
    EscState esc_state = EscState::Normal;
    std::string csi;
    std::string osc;

    for (size_t i = 0; i < raw.size(); ++i)
    {
        unsigned char ch = (unsigned char)raw[i];
        switch (esc_state)
        {
        case EscState::Normal:
            if (ch == 0x1B)
            {
                esc_state = EscState::Esc;
            }
            else if (ch == '\r')
            {
                terminal_carriage_return(state);
            }
            else if (ch == '\n')
            {
                terminal_line_feed(state);
            }
            else if (ch == '\b' || ch == 0x7F)
            {
                if (state.cursor_col > 0)
                {
                    --state.cursor_col;
                }
            }
            else if (ch == '\t' || (ch >= 0x20 && ch < 0x7F))
            {
                terminal_write_char(state, (char)ch);
            }
            break;
        case EscState::Esc:
            if (ch == '[')
            {
                csi.clear();
                esc_state = EscState::Csi;
            }
            else if (ch == ']')
            {
                osc.clear();
                esc_state = EscState::Osc;
            }
            else if (ch == '7')
            {
                state.saved_cursor_row = state.cursor_row;
                state.saved_cursor_col = state.cursor_col;
                esc_state = EscState::Normal;
            }
            else if (ch == '8')
            {
                terminal_set_cursor(state, state.saved_cursor_row, state.saved_cursor_col);
                esc_state = EscState::Normal;
            }
            else
            {
                esc_state = EscState::Normal;
            }
            break;
        case EscState::Csi:
            if (ch >= 0x40 && ch <= 0x7E)
            {
                handle_csi_sequence(state, csi, (char)ch);
                esc_state = EscState::Normal;
            }
            else
            {
                csi.push_back((char)ch);
            }
            break;
        case EscState::Osc:
            if (ch == '\a')
            {
                parse_remote_stats_osc(osc, state.latest_cpu, state.latest_mem);
                esc_state = EscState::Normal;
            }
            else
            {
                osc.push_back((char)ch);
            }
            break;
        }
    }

    if (state.latest_cpu >= 0)
    {
        g_remote_cpu_percent = state.latest_cpu;
    }
    if (state.latest_mem >= 0)
    {
        g_remote_mem_percent = state.latest_mem;
    }
    if (state.latest_cpu >= 0 || state.latest_mem >= 0)
    {
        g_metrics_state = "Live";
        refresh_session_entry_ui();
    }
    g_terminal_alt_screen_active = state.alt_screen;
    refresh_session_stats();

    bool show_cursor = state.cursor_visible &&
                       g_cursor_phase_on &&
                       g_mode == UiMode::Session &&
                       g_session_entry_mode != SessionEntryMode::Secret;
    int last_row = 0;
    for (int row = 0; row < kPtyRows; ++row)
    {
        if (!terminal_row_is_blank(state.rows[(size_t)row]))
        {
            last_row = row;
        }
    }
    if (show_cursor)
    {
        last_row = std::max(last_row, state.cursor_row);
    }
    if (state.alt_screen)
    {
        last_row = kPtyRows - 1;
    }

    std::string out;
    for (int row = 0; row <= last_row; ++row)
    {
        bool has_color = false;
        for (const TerminalCell &cell : state.rows[(size_t)row].cells)
        {
            if (cell.color != TermColor::Default)
            {
                has_color = true;
                break;
            }
        }

        std::string line;
        if (has_color || (show_cursor && row == state.cursor_row))
        {
            line = render_terminal_row_markup(state.rows[(size_t)row], row, state.cursor_row, state.cursor_col, show_cursor);
        }
        else
        {
            line = terminal_row_plain_text(state.rows[(size_t)row]);
            if (!line.empty())
            {
                std::string plain;
                append_terminal_line(plain, line);
                line = plain;
            }
        }

        out += line;
        if (row != last_row)
        {
            out.push_back('\n');
        }
    }

    return out.empty() ? std::string("#6E8298 Waiting for remote output...#") : out;
}

static void refresh_terminal_view()
{
    if (g_session_label == nullptr)
    {
        return;
    }

    std::string rendered = render_terminal_markup(g_session_log);
    lv_label_set_text(g_session_label, rendered.c_str());
    lv_obj_update_layout(g_session_panel);
    lv_obj_scroll_to_y(g_session_panel, g_terminal_alt_screen_active ? 0 : LV_COORD_MAX, LV_ANIM_OFF);
}

static void append_session_log(const std::string &chunk)
{
    if (chunk.empty())
    {
        return;
    }

    g_session_log += chunk;
    if (g_session_log.size() > kMaxLogBytes)
    {
        g_session_log.erase(0, g_session_log.size() - kMaxLogBytes);
    }
    refresh_terminal_view();
}

static std::string sanitize_pty_bytes(const char *data, size_t len)
{
    enum class EscState { Normal, Esc, Csi, Osc };
    EscState state = EscState::Normal;
    std::string out;
    out.reserve(len);

    for (size_t i = 0; i < len; ++i)
    {
        unsigned char ch = (unsigned char)data[i];
        switch (state)
        {
        case EscState::Normal:
            if (ch == 0x1B)
            {
                state = EscState::Esc;
            }
            else if (ch == '\r')
            {
            }
            else if (ch == '\b' || ch == 0x7F)
            {
                if (!out.empty()) out.pop_back();
            }
            else if (ch == '\n' || ch == '\t' || (ch >= 0x20 && ch < 0x7F))
            {
                out.push_back((char)ch);
            }
            break;
        case EscState::Esc:
            if (ch == '[')
            {
                state = EscState::Csi;
            }
            else if (ch == ']')
            {
                state = EscState::Osc;
            }
            else
            {
                state = EscState::Normal;
            }
            break;
        case EscState::Csi:
            if ((ch >= '@' && ch <= '~') || ch == 'm')
            {
                state = EscState::Normal;
            }
            break;
        case EscState::Osc:
            if (ch == '\a')
            {
                state = EscState::Normal;
            }
            break;
        }
    }
    return out;
}

static void start_edit(bool is_new)
{
    if (is_new)
    {
        g_edit_index = -1;
        g_draft = Profile {};
        g_draft.name = "New Server";
        g_draft.port = "22";
        g_draft.user = "pi";
    }
    else if (!g_profiles.empty())
    {
        g_edit_index = g_selected;
        g_draft = g_profiles[(size_t)g_selected];
    }
    g_edit_field = 0;
    g_mode = UiMode::Edit;
    apply_mode();
}

static void save_draft()
{
    g_draft.name = trim_copy(g_draft.name);
    g_draft.host = trim_copy(g_draft.host);
    g_draft.port = trim_copy(g_draft.port);
    g_draft.user = trim_copy(g_draft.user);

    if (g_draft.host.empty())
    {
        g_last_notice = "Host/IP is required";
        return;
    }
    if (g_draft.name.empty())
    {
        g_draft.name = g_draft.host;
    }
    if (g_draft.port.empty())
    {
        g_draft.port = "22";
    }
    if (g_draft.user.empty())
    {
        g_draft.user = "pi";
    }

    if (g_edit_index >= 0 && g_edit_index < (int)g_profiles.size())
    {
        g_profiles[(size_t)g_edit_index] = g_draft;
        g_selected = g_edit_index;
    }
    else if (g_profiles.size() < kMaxProfiles)
    {
        g_profiles.push_back(g_draft);
        g_selected = (int)g_profiles.size() - 1;
    }
    else
    {
        g_last_notice = "Profile limit reached";
        return;
    }

    save_profiles();
    g_mode = UiMode::Browse;
    apply_mode();
}

static void delete_selected_profile()
{
    if (g_profiles.empty())
    {
        g_mode = UiMode::Browse;
        apply_mode();
        return;
    }

    g_profiles.erase(g_profiles.begin() + g_selected);
    if (g_selected >= (int)g_profiles.size())
    {
        g_selected = std::max(0, (int)g_profiles.size() - 1);
    }
    save_profiles();
    g_last_notice = "Profile removed";
    g_mode = UiMode::Browse;
    apply_mode();
}

static void start_connect()
{
    if (g_profiles.empty())
    {
        return;
    }

    close_session();
    g_session_log.clear();
    refresh_terminal_view();

    const Profile &profile = g_profiles[(size_t)g_selected];
    g_active_endpoint = make_profile_endpoint(profile);
    g_remote_cpu_percent = -1;
    g_remote_mem_percent = -1;
    g_stats_loop_started = false;
    g_metrics_last_attempt_ms = 0;
    g_metrics_state = "Idle";
    g_control_path = make_control_path();
    std::vector<std::string> tokens;
    tokens.push_back("ssh");
    tokens.push_back("-tt");
    tokens.push_back("-o");
    tokens.push_back("StrictHostKeyChecking=no");
    tokens.push_back("-o");
    tokens.push_back("UserKnownHostsFile=/dev/null");
    tokens.push_back("-o");
    tokens.push_back("ControlMaster=auto");
    tokens.push_back("-o");
    tokens.push_back("ControlPersist=no");
    tokens.push_back("-o");
    tokens.push_back("ControlPath=" + g_control_path);
    tokens.push_back("-o");
    tokens.push_back("ConnectTimeout=10");
    tokens.push_back("-p");
    tokens.push_back(profile.port.empty() ? "22" : profile.port);
    tokens.push_back(profile.user + "@" + profile.host);

    std::vector<const char *> argv;
    for (const std::string &token : tokens)
    {
        argv.push_back(token.c_str());
    }
    argv.push_back(nullptr);

    g_pty = hal_pty_open("ssh", argv.data(), kPtyCols, kPtyRows);
    if (g_pty == nullptr)
    {
        g_last_notice = "Unable to start ssh";
        apply_mode();
        return;
    }

    g_session_running = true;
    g_connect_start_ms = lv_tick_get();
    g_connect_got_output = false;
    set_session_entry_mode(SessionEntryMode::Command);
    append_session_log("$ ssh " + make_profile_summary(profile) + "\n");
    g_mode = UiMode::Connecting;
    apply_mode();
}

static void enter_session_view()
{
    g_mode = UiMode::Session;
    lv_group_remove_all_objs(g_group);
    lv_group_add_obj(g_group, g_session_panel);
    lv_group_focus_obj(g_session_panel);
    lv_group_set_editing(g_group, true);
    apply_mode();
}

static void return_to_browse()
{
    lv_group_remove_all_objs(g_group);
    g_mode = UiMode::Browse;
    apply_mode();
}

static void write_session_bytes(const char *data, size_t len)
{
    if (data == nullptr || len == 0 || g_pty == nullptr || !g_session_running)
    {
        return;
    }
    hal_pty_write(g_pty, data, len);
}

static void maybe_start_remote_stats_loop(const std::string &sanitized_chunk)
{
    if (g_pty == nullptr || !g_session_running || g_stats_loop_started)
    {
        return;
    }
    if (g_session_entry_mode == SessionEntryMode::Secret)
    {
        return;
    }

    bool ready = false;
    size_t pos = 0;
    while (pos <= sanitized_chunk.size())
    {
        size_t end = sanitized_chunk.find('\n', pos);
        std::string line = end == std::string::npos ? sanitized_chunk.substr(pos) : sanitized_chunk.substr(pos, end - pos);
        if (looks_like_shell_prompt(line))
        {
            ready = true;
            break;
        }
        if (end == std::string::npos)
        {
            break;
        }
        pos = end + 1;
    }

    if (!ready)
    {
        if (g_mode == UiMode::Session && g_connect_got_output && lv_tick_elaps(g_connect_start_ms) > 2500U)
        {
            ready = true;
        }
    }

    if (!ready)
    {
        return;
    }

    uint32_t now = lv_tick_get();
    if (g_metrics_last_attempt_ms != 0 && lv_tick_elaps(g_metrics_last_attempt_ms) < 2000U)
    {
        return;
    }
    g_metrics_last_attempt_ms = now;
    start_metrics_channel();
}

static void update_session_state_from_output(const std::string &chunk)
{
    if (chunk.empty())
    {
        return;
    }

    std::string lowered = to_lower_ascii(chunk);
    if (lowered.find("password:") != std::string::npos ||
        lowered.find("passphrase") != std::string::npos)
    {
        set_session_entry_mode(SessionEntryMode::Secret);
        return;
    }

    if (g_session_entry_mode == SessionEntryMode::Secret)
    {
        if (lowered.find("permission denied") != std::string::npos)
        {
            return;
        }
        set_session_entry_mode(SessionEntryMode::Command);
    }
}

static void poll_pty()
{
    if (g_pty == nullptr)
    {
        return;
    }

    char buf[512];
    while (true)
    {
        int n = hal_pty_read(g_pty, buf, sizeof(buf));
        if (n > 0)
        {
            g_connect_got_output = true;
            std::string cleaned = sanitize_pty_bytes(buf, (size_t)n);
            if (!cleaned.empty() && !g_local_input_preview.empty() && g_session_entry_mode != SessionEntryMode::Secret)
            {
                g_local_input_preview.clear();
                g_local_input_tick = 0;
            }
            append_session_log(std::string(buf, (size_t)n));
            update_session_state_from_output(cleaned);
            maybe_start_remote_stats_loop(cleaned);
            continue;
        }
        break;
    }

    int exit_status = 0;
    if (hal_pty_check_child(g_pty, &exit_status) == 1)
    {
        close_session();
        char line[64];
        snprintf(line, sizeof(line), "\n[session ended: %d]\n", exit_status);
        append_session_log(line);
        if (g_mode == UiMode::Connecting)
        {
            g_last_notice = "Connect failed";
            return_to_browse();
        }
        else if (g_mode == UiMode::Session)
        {
            apply_mode();
        }
        return;
    }

    if (g_mode == UiMode::Connecting)
    {
        uint32_t elapsed = lv_tick_elaps(g_connect_start_ms);
        if (elapsed > 1200U || g_connect_got_output)
        {
            enter_session_view();
        }
    }
}

static void show_overlay_message(const char *title, const char *body)
{
    lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_overlay_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(g_overlay_title, title);
    lv_label_set_text(g_overlay_body, body);
}

static void ui_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    poll_pty();
    maybe_start_remote_stats_loop("");
    poll_metrics_channel();

    bool cursor_phase = ((lv_tick_get() / 500U) % 2U) == 0U;
    if (cursor_phase != g_cursor_phase_on)
    {
        g_cursor_phase_on = cursor_phase;
        if (g_mode == UiMode::Session)
        {
            refresh_terminal_view();
        }
    }
}

static void handle_browse_key(uint32_t key, const char *utf8)
{
    (void)utf8;
    switch (key)
    {
    case KEY_UP:
        if (!g_profiles.empty() && g_selected > 0)
        {
            --g_selected;
            apply_mode();
        }
        break;
    case KEY_DOWN:
        if (!g_profiles.empty() && g_selected + 1 < (int)g_profiles.size())
        {
            ++g_selected;
            apply_mode();
        }
        break;
    case KEY_ENTER:
    case KEY_KPENTER:
        start_connect();
        break;
    case KEY_N:
        start_edit(true);
        break;
    case KEY_E:
        start_edit(false);
        break;
    case KEY_D:
        g_mode = UiMode::DeleteConfirm;
        apply_mode();
        break;
    case KEY_ESC:
        lv_deinit();
        _exit(0);
        break;
    default:
        break;
    }
}

static void handle_edit_key(uint32_t key, const char *utf8)
{
    if (key == KEY_UP)
    {
        g_edit_field = std::max(0, g_edit_field - 1);
        apply_mode();
        return;
    }
    if (key == KEY_DOWN || key == KEY_TAB)
    {
        g_edit_field = std::min(3, g_edit_field + 1);
        apply_mode();
        return;
    }
    if (key == KEY_ESC)
    {
        return_to_browse();
        return;
    }
    if (key == KEY_ENTER || key == KEY_KPENTER)
    {
        save_draft();
        return;
    }
    if (key == KEY_BACKSPACE)
    {
        std::string value = profile_field(g_draft, g_edit_field);
        if (!value.empty())
        {
            value.pop_back();
            set_profile_field(g_draft, g_edit_field, value);
            apply_mode();
        }
        return;
    }
    if (key == KEY_DELETE)
    {
        set_profile_field(g_draft, g_edit_field, "");
        apply_mode();
        return;
    }
    if (utf8 && utf8[0] >= 0x20)
    {
        std::string value = profile_field(g_draft, g_edit_field);
        value += utf8;
        set_profile_field(g_draft, g_edit_field, value);
        apply_mode();
    }
}

static void handle_delete_key(uint32_t key)
{
    if (key == KEY_ESC || key == KEY_N)
    {
        return_to_browse();
    }
    else if (key == KEY_Y)
    {
        delete_selected_profile();
    }
}

static void handle_session_key(uint32_t key, const char *utf8)
{
    if (key == KEY_ESC)
    {
        close_session();
        g_last_notice = "Session closed";
        return_to_browse();
        return;
    }

    if (g_pty == nullptr || !g_session_running)
    {
        return;
    }

    if (key == KEY_ENTER || key == KEY_KPENTER)
    {
        static const char newline[] = "\n";
        write_session_bytes(newline, 1);
        g_local_input_preview.clear();
        g_local_input_tick = lv_tick_get();
        refresh_terminal_view();
    }
    else if (key == KEY_BACKSPACE)
    {
        static const char backspace[] = "\x7f";
        write_session_bytes(backspace, 1);
        if (g_session_entry_mode != SessionEntryMode::Secret && !g_local_input_preview.empty())
        {
            g_local_input_preview.pop_back();
        }
        g_local_input_tick = lv_tick_get();
        refresh_terminal_view();
    }
    else if (key == KEY_DELETE)
    {
        static const char backspace[] = "\x7f";
        write_session_bytes(backspace, 1);
        if (g_session_entry_mode != SessionEntryMode::Secret && !g_local_input_preview.empty())
        {
            g_local_input_preview.pop_back();
        }
        g_local_input_tick = lv_tick_get();
        refresh_terminal_view();
    }
    else if (key == KEY_LEFT)
    {
        static const char left_seq[] = "\x1b[D";
        write_session_bytes(left_seq, sizeof(left_seq) - 1);
        g_local_input_tick = lv_tick_get();
        refresh_terminal_view();
    }
    else if (key == KEY_RIGHT)
    {
        static const char right_seq[] = "\x1b[C";
        write_session_bytes(right_seq, sizeof(right_seq) - 1);
        g_local_input_tick = lv_tick_get();
        refresh_terminal_view();
    }
    else if (key == KEY_UP)
    {
        static const char up_seq[] = "\x1b[A";
        write_session_bytes(up_seq, sizeof(up_seq) - 1);
        g_local_input_preview.clear();
        g_local_input_tick = lv_tick_get();
        refresh_terminal_view();
    }
    else if (key == KEY_DOWN)
    {
        static const char down_seq[] = "\x1b[B";
        write_session_bytes(down_seq, sizeof(down_seq) - 1);
        g_local_input_preview.clear();
        g_local_input_tick = lv_tick_get();
        refresh_terminal_view();
    }
    else if (key == KEY_PAGEUP || key == KEY_PREVIOUS)
    {
        static const char page_up_seq[] = "\x1b[5~";
        write_session_bytes(page_up_seq, sizeof(page_up_seq) - 1);
        g_local_input_preview.clear();
        g_local_input_tick = lv_tick_get();
        refresh_terminal_view();
    }
    else if (key == KEY_PAGEDOWN || key == KEY_NEXT)
    {
        static const char page_down_seq[] = "\x1b[6~";
        write_session_bytes(page_down_seq, sizeof(page_down_seq) - 1);
        g_local_input_preview.clear();
        g_local_input_tick = lv_tick_get();
        refresh_terminal_view();
    }
    else if (key == KEY_TAB)
    {
        static const char tab_char[] = "\t";
        write_session_bytes(tab_char, 1);
        if (g_session_entry_mode != SessionEntryMode::Secret)
        {
            g_local_input_preview.push_back('\t');
        }
        g_local_input_tick = lv_tick_get();
        refresh_terminal_view();
    }
    else if (utf8 != nullptr && utf8[0] != '\0')
    {
        write_session_bytes(utf8, std::strlen(utf8));
        if (g_session_entry_mode != SessionEntryMode::Secret &&
            (unsigned char)utf8[0] >= 0x20)
        {
            g_local_input_preview += utf8;
        }
        g_local_input_tick = lv_tick_get();
        refresh_terminal_view();
    }
}

} // namespace

void ui_init()
{
    load_profiles();

    lv_obj_t *screen = lv_screen_active();
    lv_display_t *disp = lv_display_get_default();
    if (disp != nullptr)
    {
        g_screen_w = lv_display_get_horizontal_resolution(disp);
        g_screen_h = lv_display_get_vertical_resolution(disp);
    }

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x050A12), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x0A1725), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *glow = lv_obj_create(screen);
    lv_obj_remove_style_all(glow);
    lv_obj_set_size(glow, 140, 140);
    lv_obj_set_pos(glow, -50, 12);
    lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(glow, lv_color_hex(0x1E84FF), 0);
    lv_obj_set_style_bg_opa(glow, 18, 0);
    lv_obj_set_style_shadow_width(glow, 28, 0);
    lv_obj_set_style_shadow_color(glow, lv_color_hex(0x1E84FF), 0);
    lv_obj_set_style_shadow_opa(glow, 20, 0);

    lv_obj_t *header = create_card(screen, 8, 6, g_screen_w - 16, 24);
    g_title = create_label(header, "SSH Client", &lv_font_montserrat_14, lv_color_hex(0xF6FAFF));
    lv_obj_set_pos(g_title, 10, 2);

    g_header_meta = create_label(header, "Local profiles", &lv_font_montserrat_12, lv_color_hex(0x8EB4D8));
    lv_obj_set_pos(g_header_meta, 94, 4);
    lv_obj_set_width(g_header_meta, 126);
    lv_label_set_long_mode(g_header_meta, LV_LABEL_LONG_CLIP);

    g_status_chip = lv_obj_create(header);
    lv_obj_remove_style_all(g_status_chip);
    lv_obj_set_size(g_status_chip, 80, 16);
    lv_obj_set_pos(g_status_chip, g_screen_w - 104, 4);
    lv_obj_set_style_radius(g_status_chip, 8, 0);
    lv_obj_set_style_bg_color(g_status_chip, lv_color_hex(0x102336), 0);
    lv_obj_set_style_bg_opa(g_status_chip, LV_OPA_COVER, 0);
    g_status_text = create_label(g_status_chip, "Profiles", &lv_font_montserrat_12, lv_color_hex(0x89D6B0));
    lv_obj_center(g_status_text);

    g_list_panel = create_card(screen, 8, 34, 304, 100);
    lv_obj_t *list_label = create_label(g_list_panel, "Saved Hosts", &lv_font_montserrat_12, lv_color_hex(0x8FAFD0));
    lv_obj_set_pos(list_label, 10, 6);
    g_profile_count = create_label(g_list_panel, "0 saved", &lv_font_montserrat_12, lv_color_hex(0x5FD0A1));
    lv_obj_set_pos(g_profile_count, 236, 6);

    for (size_t i = 0; i < kVisibleRows; ++i)
    {
        g_row_cards[i] = create_card(g_list_panel, 8, 22 + (lv_coord_t)i * 19, 288, 19);
        lv_obj_set_style_radius(g_row_cards[i], 7, 0);
        lv_obj_set_style_shadow_width(g_row_cards[i], 0, 0);
        g_row_titles[i] = create_label(g_row_cards[i], "", &lv_font_montserrat_12, lv_color_hex(0xF5FAFF));
        lv_obj_set_pos(g_row_titles[i], 8, 0);
        lv_obj_set_width(g_row_titles[i], 94);
        lv_label_set_long_mode(g_row_titles[i], LV_LABEL_LONG_CLIP);
        g_row_subtitles[i] = create_label(g_row_cards[i], "", &lv_font_montserrat_12, lv_color_hex(0x89A5BE));
        lv_obj_set_pos(g_row_subtitles[i], 108, 0);
        lv_obj_set_width(g_row_subtitles[i], 170);
        lv_label_set_long_mode(g_row_subtitles[i], LV_LABEL_LONG_CLIP);
    }

    g_empty_hint = create_label(g_list_panel, "Press N to add your first host", &lv_font_montserrat_12, lv_color_hex(0x6E8298));
    lv_obj_set_pos(g_empty_hint, 10, 58);
    lv_obj_set_width(g_empty_hint, 280);
    lv_label_set_long_mode(g_empty_hint, LV_LABEL_LONG_WRAP);

    g_detail_panel = create_card(screen, 8, 136, 304, 16);
    g_detail_name = create_label(g_detail_panel, "No profiles", &lv_font_montserrat_14, lv_color_hex(0xF7FBFF));
    lv_obj_set_pos(g_detail_name, 10, 1);
    lv_obj_set_width(g_detail_name, 84);
    lv_label_set_long_mode(g_detail_name, LV_LABEL_LONG_CLIP);

    g_detail_host = create_label(g_detail_panel, "Host --", &lv_font_montserrat_12, lv_color_hex(0xAAD2FF));
    lv_obj_set_pos(g_detail_host, 96, 1);
    lv_obj_set_width(g_detail_host, 126);
    lv_label_set_long_mode(g_detail_host, LV_LABEL_LONG_CLIP);

    g_detail_port = create_label(g_detail_panel, "Port --", &lv_font_montserrat_12, lv_color_hex(0xAAD2FF));
    lv_obj_set_pos(g_detail_port, 224, 1);
    lv_obj_set_width(g_detail_port, 30);
    g_detail_user = create_label(g_detail_panel, "User --", &lv_font_montserrat_12, lv_color_hex(0xAAD2FF));
    lv_obj_set_pos(g_detail_user, 258, 1);
    lv_obj_set_width(g_detail_user, 36);

    g_detail_hint = create_label(g_detail_panel, "Profiles are stored locally", &lv_font_montserrat_12, lv_color_hex(0x7389A2));
    lv_obj_add_flag(g_detail_hint, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_long_mode(g_detail_hint, LV_LABEL_LONG_CLIP);

    g_edit_panel = create_card(screen, 8, 34, 304, 110);
    create_label(g_edit_panel, "Edit SSH Profile", &lv_font_montserrat_14, lv_color_hex(0xF7FBFF));
    lv_obj_set_pos(lv_obj_get_child(g_edit_panel, 0), 10, 6);

    for (int i = 0; i < 4; ++i)
    {
        g_edit_rows[(size_t)i] = create_card(g_edit_panel, 10, 22 + i * 18, 284, 15);
        lv_obj_set_style_radius(g_edit_rows[(size_t)i], 8, 0);
        lv_obj_set_style_shadow_width(g_edit_rows[(size_t)i], 0, 0);
        g_edit_labels[(size_t)i] = create_label(g_edit_rows[(size_t)i], kFieldNames[i], &lv_font_montserrat_12, lv_color_hex(0x7D98B0));
        lv_obj_set_pos(g_edit_labels[(size_t)i], 8, 0);
        g_edit_values[(size_t)i] = create_label(g_edit_rows[(size_t)i], "", &lv_font_montserrat_12, lv_color_hex(0xF7FBFF));
        lv_obj_set_pos(g_edit_values[(size_t)i], 72, 0);
        lv_obj_set_width(g_edit_values[(size_t)i], 200);
        lv_label_set_long_mode(g_edit_values[(size_t)i], LV_LABEL_LONG_CLIP);
    }
    g_edit_help = create_label(g_edit_panel, "Creating a new profile", &lv_font_montserrat_12, lv_color_hex(0x7389A2));
    lv_obj_set_pos(g_edit_help, 10, 96);

    g_session_panel = create_card(screen, 8, 34, 304, 104);
    lv_obj_set_style_bg_color(g_session_panel, lv_color_hex(0x04080D), 0);
    lv_obj_set_style_bg_grad_color(g_session_panel, lv_color_hex(0x060B12), 0);
    lv_obj_set_style_border_color(g_session_panel, lv_color_hex(0x182636), 0);
    lv_obj_set_style_shadow_width(g_session_panel, 0, 0);
    lv_obj_set_style_radius(g_session_panel, 10, 0);
    lv_obj_add_flag(g_session_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_session_panel, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_scroll_dir(g_session_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_session_panel, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_pad_left(g_session_panel, 10, 0);
    lv_obj_set_style_pad_right(g_session_panel, 10, 0);
    lv_obj_set_style_pad_top(g_session_panel, 8, 0);
    lv_obj_set_style_pad_bottom(g_session_panel, 8, 0);

    g_session_label = create_label(g_session_panel, "", &lv_font_montserrat_12, lv_color_hex(0xDDF0FF));
    lv_obj_set_width(g_session_label, 284);
    lv_label_set_long_mode(g_session_label, LV_LABEL_LONG_WRAP);
    lv_label_set_recolor(g_session_label, true);

    g_session_stats_panel = create_card(screen, 8, 142, 304, 20);
    lv_obj_set_style_radius(g_session_stats_panel, 8, 0);
    lv_obj_set_style_shadow_width(g_session_stats_panel, 0, 0);
    lv_obj_set_style_bg_color(g_session_stats_panel, lv_color_hex(0x09121B), 0);
    lv_obj_set_style_bg_grad_color(g_session_stats_panel, lv_color_hex(0x0C1722), 0);
    lv_obj_set_style_border_color(g_session_stats_panel, lv_color_hex(0x18293A), 0);

    g_cpu_label = create_label(g_session_stats_panel, "CPU", &lv_font_montserrat_12, lv_color_hex(0x8FD3FF));
    lv_obj_set_pos(g_cpu_label, 8, 2);
    g_cpu_bar = lv_bar_create(g_session_stats_panel);
    lv_obj_set_pos(g_cpu_bar, 34, 5);
    lv_obj_set_size(g_cpu_bar, 96, 8);
    lv_bar_set_range(g_cpu_bar, 0, 100);
    lv_obj_set_style_bg_color(g_cpu_bar, lv_color_hex(0x152534), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_cpu_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_cpu_bar, lv_color_hex(0x56B3FF), LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_cpu_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(g_cpu_bar, 5, LV_PART_INDICATOR);
    g_cpu_value = create_label(g_session_stats_panel, "--", &lv_font_montserrat_12, lv_color_hex(0xCFE8FF));
    lv_obj_set_pos(g_cpu_value, 134, 2);
    lv_obj_set_width(g_cpu_value, 28);

    g_mem_label = create_label(g_session_stats_panel, "MEM", &lv_font_montserrat_12, lv_color_hex(0x9AE6A8));
    lv_obj_set_pos(g_mem_label, 164, 2);
    g_mem_bar = lv_bar_create(g_session_stats_panel);
    lv_obj_set_pos(g_mem_bar, 194, 5);
    lv_obj_set_size(g_mem_bar, 76, 8);
    lv_bar_set_range(g_mem_bar, 0, 100);
    lv_obj_set_style_bg_color(g_mem_bar, lv_color_hex(0x152534), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_mem_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_mem_bar, lv_color_hex(0x89D6B0), LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_mem_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(g_mem_bar, 5, LV_PART_INDICATOR);
    g_mem_value = create_label(g_session_stats_panel, "--", &lv_font_montserrat_12, lv_color_hex(0xD8F6DE));
    lv_obj_set_pos(g_mem_value, 274, 2);
    lv_obj_set_width(g_mem_value, 28);

    g_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(g_overlay);
    lv_obj_set_size(g_overlay, g_screen_w, g_screen_h);
    lv_obj_set_style_bg_opa(g_overlay, 140, 0);
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(0x01060B), 0);
    g_overlay_card = create_card(g_overlay, 48, 48, 224, 72);
    g_overlay_title = create_label(g_overlay_card, "Connecting", &lv_font_montserrat_16, lv_color_hex(0xF7FBFF));
    lv_obj_set_pos(g_overlay_title, 16, 10);
    g_overlay_body = create_label(g_overlay_card, "user@host:22", &lv_font_montserrat_12, lv_color_hex(0x9CCFFF));
    lv_obj_set_pos(g_overlay_body, 16, 38);
    lv_obj_set_width(g_overlay_body, 150);
    lv_label_set_long_mode(g_overlay_body, LV_LABEL_LONG_CLIP);
    g_overlay_spinner = lv_spinner_create(g_overlay_card);
    lv_obj_set_size(g_overlay_spinner, 26, 26);
    lv_obj_set_pos(g_overlay_spinner, 176, 22);
    lv_spinner_set_anim_params(g_overlay_spinner, 900, 80);
    lv_obj_set_style_arc_color(g_overlay_spinner, lv_color_hex(0x1A2D40), 0);
    lv_obj_set_style_arc_color(g_overlay_spinner, lv_color_hex(0x56B3FF), LV_PART_INDICATOR);

    g_footer = create_label(screen, "", &lv_font_montserrat_12, lv_color_hex(0x7E97AE));
    lv_obj_set_pos(g_footer, 10, 150);
    lv_obj_set_width(g_footer, 300);
    lv_label_set_long_mode(g_footer, LV_LABEL_LONG_CLIP);

    g_group = lv_group_create();
    g_tick_timer = lv_timer_create(ui_tick_cb, kUiTickMs, nullptr);
    (void)g_tick_timer;

    refresh_session_entry_ui();
    refresh_session_stats();
    refresh_terminal_view();
    apply_mode();
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

    switch (g_mode)
    {
    case UiMode::Browse:
        handle_browse_key(key_code, utf8);
        break;
    case UiMode::Edit:
        handle_edit_key(key_code, utf8);
        break;
    case UiMode::DeleteConfirm:
        handle_delete_key(key_code);
        break;
    case UiMode::Connecting:
        if (key_code == KEY_ESC)
        {
            close_session();
            g_last_notice = "Connect cancelled";
            return_to_browse();
        }
        break;
    case UiMode::Session:
        handle_session_key(key_code, utf8);
        break;
    }
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
