#include "lvgl/lvgl.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

void ui_init();
lv_group_t *ui_get_input_group();
void ui_debug_log(const char *line);
void ui_handle_key_item(uint32_t key_code, const char *utf8, int key_state);

static lv_indev_t *g_keyboard_indev = nullptr;
static volatile int g_app_quit_requested = 0;
static volatile int g_app_quit_enabled = 0;
static std::string g_keyboard_device;

static int g_key_fd = -1;
static bool g_shift_down = false;

static void app_request_quit()
{
    if (g_app_quit_enabled)
    {
        g_app_quit_requested = 1;
    }
}

static int app_should_quit()
{
    return g_app_quit_requested;
}

static const char *getenv_default(const char *name, const char *dflt)
{
    const char *value = getenv(name);
    return value ? value : dflt;
}

static void log_stdout(const char *line)
{
    printf("[GroveUART] %s\n", line ? line : "");
    fflush(stdout);
}

static bool path_exists(const char *path)
{
    struct stat st;
    return path != nullptr && stat(path, &st) == 0;
}

static bool contains_ci(const std::string &haystack, const char *needle)
{
    if (needle == nullptr || needle[0] == '\0')
    {
        return true;
    }

    std::string lowered = haystack;
    for (char &ch : lowered)
    {
        if (ch >= 'A' && ch <= 'Z')
        {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return lowered.find(needle) != std::string::npos;
}

static void add_unique(std::vector<std::string> &paths, const std::string &path)
{
    if (path.empty())
    {
        return;
    }

    for (const std::string &existing : paths)
    {
        if (existing == path)
        {
            return;
        }
    }
    paths.push_back(path);
}

static void add_input_dir(std::vector<std::string> &paths, const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (dir == nullptr)
    {
        return;
    }

    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (entry->d_name[0] == '.')
        {
            continue;
        }
        std::string name(entry->d_name);
        if (contains_ci(name, "event"))
        {
            add_unique(paths, std::string(dir_path) + "/" + name);
        }
    }
    closedir(dir);
}

static void add_proc_input_events(std::vector<std::string> &paths)
{
    FILE *fp = fopen("/proc/bus/input/devices", "r");
    if (fp == nullptr)
    {
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp) != nullptr)
    {
        const char *handlers = strstr(line, "Handlers=");
        if (handlers == nullptr)
        {
            continue;
        }

        const char *event = strstr(handlers, "event");
        while (event != nullptr)
        {
            int number = -1;
            if (sscanf(event, "event%d", &number) == 1 && number >= 0)
            {
                char path[64] = {};
                snprintf(path, sizeof(path), "/dev/input/event%d", number);
                add_unique(paths, path);
            }
            event = strstr(event + 5, "event");
        }
    }
    fclose(fp);
}

static bool test_bit(unsigned int bit, const unsigned long *bits, size_t word_count)
{
    const size_t word = bit / (sizeof(unsigned long) * 8);
    if (word >= word_count)
    {
        return false;
    }
    return (bits[word] & (1UL << (bit % (sizeof(unsigned long) * 8)))) != 0;
}

static int input_device_score(const std::string &path, char *name, size_t name_len)
{
    if (name != nullptr && name_len > 0)
    {
        name[0] = '\0';
    }

    int score = 0;
    if (contains_ci(path, "keyboard") || contains_ci(path, "keypad") ||
        contains_ci(path, "tca8418") || contains_ci(path, "m5"))
    {
        score += 30;
    }

    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        return score - 100;
    }

    char local_name[128] = {};
    if (ioctl(fd, EVIOCGNAME(sizeof(local_name)), local_name) >= 0)
    {
        if (name != nullptr && name_len > 0)
        {
            snprintf(name, name_len, "%s", local_name);
        }
        if (contains_ci(local_name, "tca8418")) score += 100;
        if (contains_ci(local_name, "keyboard") || contains_ci(local_name, "keypad") || contains_ci(local_name, "m5")) score += 50;
    }

    unsigned long ev_bits[(EV_MAX + sizeof(unsigned long) * 8) / (sizeof(unsigned long) * 8)] = {};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) >= 0 && test_bit(EV_KEY, ev_bits, sizeof(ev_bits) / sizeof(ev_bits[0])))
    {
        score += 20;
    }

    unsigned long key_bits[(KEY_MAX + sizeof(unsigned long) * 8) / (sizeof(unsigned long) * 8)] = {};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0)
    {
        if (test_bit(KEY_ENTER, key_bits, sizeof(key_bits) / sizeof(key_bits[0]))) score += 10;
        if (test_bit(KEY_BACKSPACE, key_bits, sizeof(key_bits) / sizeof(key_bits[0]))) score += 10;
        if (test_bit(KEY_A, key_bits, sizeof(key_bits) / sizeof(key_bits[0]))) score += 10;
    }

    close(fd);
    return score;
}

static std::string detect_keyboard_device()
{
    const char *forced = getenv("LV_LINUX_KEYBOARD_DEVICE");
    if (forced != nullptr && forced[0] != '\0')
    {
        return forced;
    }

    std::vector<std::string> candidates;
    add_unique(candidates, "/dev/input/by-path/platform-3f804000.i2c-event");
    add_input_dir(candidates, "/dev/input/by-path");
    add_input_dir(candidates, "/dev/input/by-id");
    add_proc_input_events(candidates);
    for (int i = 0; i < 32; ++i)
    {
        char path[64] = {};
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        if (path_exists(path))
        {
            add_unique(candidates, path);
        }
    }

    std::string best;
    int best_score = -10000;
    for (const std::string &candidate : candidates)
    {
        char name[128] = {};
        int score = input_device_score(candidate, name, sizeof(name));
        if (score > best_score)
        {
            best_score = score;
            best = candidate;
        }
    }

    if (best.empty())
    {
        best = "/dev/input/by-path/platform-3f804000.i2c-event";
    }

    return best;
}

static int get_st7789v_fbdev(char *dev_path, size_t buf_size)
{
    if (dev_path == nullptr || buf_size == 0)
    {
        return -1;
    }

    FILE *fp = fopen("/proc/fb", "r");
    if (fp == nullptr)
    {
        perror("Failed to open /proc/fb");
        return -1;
    }

    char line[256];
    int fb_num = -1;
    while (fgets(line, sizeof(line), fp) != nullptr)
    {
        if (strstr(line, "fb_st7789v") != nullptr && sscanf(line, "%d", &fb_num) == 1)
        {
            break;
        }
    }

    fclose(fp);
    if (fb_num < 0)
    {
        fprintf(stderr, "fb_st7789v not found in /proc/fb\n");
        return -1;
    }

    snprintf(dev_path, buf_size, "/dev/fb%d", fb_num);
    return 0;
}

static char shifted_digit(uint32_t code)
{
    switch (code)
    {
    case KEY_1: return '!';
    case KEY_2: return '@';
    case KEY_3: return '#';
    case KEY_4: return '$';
    case KEY_5: return '%';
    case KEY_6: return '^';
    case KEY_7: return '&';
    case KEY_8: return '*';
    case KEY_9: return '(';
    case KEY_0: return ')';
    default: return 0;
    }
}

static char letter_key_to_char(uint32_t code)
{
    switch (code)
    {
    case KEY_A: return 'a';
    case KEY_B: return 'b';
    case KEY_C: return 'c';
    case KEY_D: return 'd';
    case KEY_E: return 'e';
    case KEY_F: return 'f';
    case KEY_G: return 'g';
    case KEY_H: return 'h';
    case KEY_I: return 'i';
    case KEY_J: return 'j';
    case KEY_K: return 'k';
    case KEY_L: return 'l';
    case KEY_M: return 'm';
    case KEY_N: return 'n';
    case KEY_O: return 'o';
    case KEY_P: return 'p';
    case KEY_Q: return 'q';
    case KEY_R: return 'r';
    case KEY_S: return 's';
    case KEY_T: return 't';
    case KEY_U: return 'u';
    case KEY_V: return 'v';
    case KEY_W: return 'w';
    case KEY_X: return 'x';
    case KEY_Y: return 'y';
    case KEY_Z: return 'z';
    default: return 0;
    }
}

static char tca_symbol_code_to_char(uint32_t code)
{
    switch (code)
    {
    case 183: return '!';
    case 184: return '@';
    case 185: return '#';
    case 186: return '$';
    case 187: return '%';
    case 188: return '^';
    case 189: return '&';
    case 190: return '*';
    case 191: return '(';
    case 192: return ')';
    case 193: return '~';
    case 194: return '`';
    case 195: return '+';
    case 196: return '-';
    case 197: return '/';
    case 198: return '\\';
    case 199: return '{';
    case 200: return '}';
    case 201: return '[';
    case 202: return ']';
    case 209: return '=';
    case 210: return ':';
    case 211: return ';';
    case 212: return '_';
    case 213: return '?';
    case 214: return '<';
    case 215: return '>';
    case 216: return '\'';
    case 217: return '"';
    case 231: return ',';
    case 232: return '.';
    case 233: return '|';
    default: return 0;
    }
}

static void raw_key_to_utf8(uint32_t code, char *out, size_t out_len)
{
    if (out == nullptr || out_len == 0)
    {
        return;
    }
    out[0] = '\0';

    char ch = tca_symbol_code_to_char(code);
    if (ch == 0)
    {
        ch = letter_key_to_char(code);
    }
    if (ch >= 'a' && ch <= 'z' && g_shift_down)
    {
        ch = static_cast<char>(ch - 'a' + 'A');
    }
    else if (ch == 0 && code >= KEY_1 && code <= KEY_0)
    {
        ch = g_shift_down ? shifted_digit(code) : (code == KEY_0 ? '0' : static_cast<char>('1' + (code - KEY_1)));
    }
    else if (ch == 0)
    {
        switch (code)
        {
        case KEY_SPACE: ch = ' '; break;
        case KEY_TAB: ch = '\t'; break;
        case KEY_MINUS: ch = g_shift_down ? '_' : '-'; break;
        case KEY_EQUAL: ch = g_shift_down ? '+' : '='; break;
        case KEY_LEFTBRACE: ch = g_shift_down ? '{' : '['; break;
        case KEY_RIGHTBRACE: ch = g_shift_down ? '}' : ']'; break;
        case KEY_BACKSLASH: ch = g_shift_down ? '|' : '\\'; break;
        case KEY_SEMICOLON: ch = g_shift_down ? ':' : ';'; break;
        case KEY_APOSTROPHE: ch = g_shift_down ? '"' : '\''; break;
        case KEY_GRAVE: ch = g_shift_down ? '~' : '`'; break;
        case KEY_COMMA: ch = g_shift_down ? '<' : ','; break;
        case KEY_DOT: ch = g_shift_down ? '>' : '.'; break;
        case KEY_SLASH: ch = g_shift_down ? '?' : '/'; break;
        default: break;
        }
    }

    if (ch != 0)
    {
        out[0] = ch;
        out[1] = '\0';
    }
}

static bool env_enabled(const char *name, bool fallback)
{
    const char *value = getenv(name);
    if (value == nullptr || value[0] == '\0')
    {
        return fallback;
    }
    return strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 &&
           strcmp(value, "False") != 0 &&
           strcmp(value, "off") != 0 &&
           strcmp(value, "OFF") != 0;
}

static void open_raw_keyboard()
{
    if (g_key_fd >= 0)
    {
        return;
    }

    g_key_fd = open(g_keyboard_device.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (g_key_fd < 0)
    {
        char line[220] = {};
        snprintf(line, sizeof(line), "KBD raw open failed %s: %s", g_keyboard_device.c_str(), strerror(errno));
        log_stdout(line);
        return;
    }

    bool grab = env_enabled("GROVEUART_KEYBOARD_GRAB", true);
    if (grab && ioctl(g_key_fd, EVIOCGRAB, 1) < 0)
    {
        char line[220] = {};
        snprintf(line, sizeof(line), "KBD raw grab failed: %s", strerror(errno));
        log_stdout(line);
    }

    char line[220] = {};
    snprintf(line, sizeof(line), "KBD raw polling %s%s", g_keyboard_device.c_str(), grab ? " grabbed" : " shared");
    log_stdout(line);
}

static void handle_raw_event(uint32_t code, int value)
{
    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT)
    {
        g_shift_down = value != 0;
    }

    char utf8[8] = {};
    if (value != 0)
    {
        raw_key_to_utf8(code, utf8, sizeof(utf8));
    }

    if (value != 0)
    {
        if (code == KEY_ESC || code == KEY_HOME)
        {
            log_stdout("SYS exit key");
            app_request_quit();
        }
        else
        {
            ui_handle_key_item(code, utf8, value == 2 ? 2 : 1);
        }
    }
}

static void process_raw_keys()
{
    if (g_key_fd < 0)
    {
        return;
    }

    int count = 0;
    while (count < 32)
    {
        struct input_event event {};
        ssize_t got = read(g_key_fd, &event, sizeof(event));
        if (got == sizeof(event))
        {
            if (event.type == EV_KEY)
            {
                handle_raw_event(event.code, event.value);
                count++;
            }
            continue;
        }

        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            break;
        }
        if (got < 0 && errno == EINTR)
        {
            continue;
        }
        if (got < 0)
        {
            char line[160] = {};
            snprintf(line, sizeof(line), "KBD raw read failed: %s", strerror(errno));
            log_stdout(line);
        }
        break;
    }
}

#if !LV_USE_SDL
static void lv_linux_indev_init(void)
{
    log_stdout("KBD init raw polling path");
#if LV_USE_EVDEV
    const char *mouse_device = getenv_default("LV_LINUX_MOUSE_DEVICE", nullptr);
    if (mouse_device)
    {
        lv_evdev_create(LV_INDEV_TYPE_POINTER, mouse_device);
    }
#endif

    g_keyboard_device = detect_keyboard_device();
    open_raw_keyboard();
    g_keyboard_indev = nullptr;
}
#endif

#if LV_USE_LINUX_FBDEV
static void lv_linux_disp_init(void)
{
    const char *device = nullptr;
    char fbdev[64] = {};
    device = getenv_default("LV_LINUX_FBDEV_DEVICE", nullptr);
    if ((device == nullptr) && (get_st7789v_fbdev(fbdev, sizeof(fbdev)) == 0))
    {
        device = fbdev;
    }

    lv_display_t *disp = lv_linux_fbdev_create();
    if (disp == nullptr)
    {
        printf("Failed to create fbdev display!\n");
        return;
    }

    lv_linux_fbdev_set_file(disp, device);
}

#elif LV_USE_LINUX_DRM
static void lv_linux_disp_init(void)
{
    const char *device = getenv_default("LV_LINUX_DRM_CARD", "/dev/dri/card0");
    lv_display_t *disp = lv_linux_drm_create();
    lv_linux_drm_set_file(disp, device, -1);
}

#elif LV_USE_SDL
#include "lvgl/src/drivers/sdl/lv_sdl_keyboard.h"
#include "lvgl/src/drivers/sdl/lv_sdl_mouse.h"
#include "lvgl/src/drivers/sdl/lv_sdl_window.h"

static void lv_linux_disp_init(void)
{
    const int width = atoi(getenv("LV_SDL_VIDEO_WIDTH") ?: "320");
    const int height = atoi(getenv("LV_SDL_VIDEO_HEIGHT") ?: "170");
    lv_sdl_window_create(width, height);
}

static void app_key_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_KEY)
    {
        return;
    }

    uint32_t key = lv_event_get_key(event);
    if (key == LV_KEY_HOME || key == LV_KEY_ESC)
    {
        app_request_quit();
    }
}

static void lv_linux_indev_init(void)
{
    lv_sdl_mouse_create();
    g_keyboard_indev = lv_sdl_keyboard_create();
}

#else
#error Unsupported configuration
#endif

int main(void)
{
    log_stdout("main start");
    lv_init();

    log_stdout("init display");
    lv_linux_disp_init();

    log_stdout("init ui");
    ui_init();

    log_stdout("init input");
    lv_linux_indev_init();
    log_stdout("enter loop");

#if LV_USE_SDL
    lv_obj_add_event_cb(lv_screen_active(), app_key_event_cb, LV_EVENT_KEY, nullptr);
#endif

    lv_group_t *group = ui_get_input_group();
    if (group != nullptr && g_keyboard_indev != nullptr)
    {
        lv_group_set_default(group);
        lv_indev_set_group(g_keyboard_indev, group);
    }

    int startup_ticks = 0;
    while (!app_should_quit())
    {
        lv_timer_handler();
        process_raw_keys();
        if (!g_app_quit_enabled && ++startup_ticks >= 5)
        {
            g_app_quit_enabled = 1;
        }
        usleep(1000);
    }

    if (g_key_fd >= 0)
    {
        ioctl(g_key_fd, EVIOCGRAB, 0);
        close(g_key_fd);
        g_key_fd = -1;
    }
    log_stdout("exit");
    return 0;
}
