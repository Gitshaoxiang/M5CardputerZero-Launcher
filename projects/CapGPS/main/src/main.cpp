#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if !LV_USE_SDL
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#endif

void ui_init();
lv_group_t *ui_get_input_group();
void ui_shutdown();

static lv_indev_t *g_keyboard_indev = nullptr;
static volatile int g_app_quit_requested = 0;
static volatile int g_app_quit_enabled = 0;
static const char *getenv_default(const char *name, const char *dflt);

#if !LV_USE_SDL
static pthread_t g_key_thread;
static volatile int g_key_thread_started = 0;
#endif

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

#if !LV_USE_SDL
static void *app_key_monitor_thread(void *arg)
{
    const char *device = arg ? static_cast<const char *>(arg)
                             : "/dev/input/by-path/platform-3f804000.i2c-event";
    int fd = open(device, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        fprintf(stderr, "Failed to open key input %s: %s\n", device, strerror(errno));
        return nullptr;
    }

    while (!app_should_quit())
    {
        struct input_event event {};
        ssize_t got = read(fd, &event, sizeof(event));
        if (got < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        if (got != sizeof(event) || event.type != EV_KEY || event.value == 0)
        {
            continue;
        }

        if (event.code == KEY_HOME || event.code == KEY_ESC)
        {
            app_request_quit();
        }
    }

    close(fd);
    return nullptr;
}

static void app_start_key_monitor()
{
    if (g_key_thread_started)
    {
        return;
    }

    const char *keyboard_device = getenv_default(
        "LV_LINUX_KEYBOARD_DEVICE",
        "/dev/input/by-path/platform-3f804000.i2c-event");
    if (pthread_create(&g_key_thread, nullptr, app_key_monitor_thread,
                       const_cast<char *>(keyboard_device)) == 0)
    {
        pthread_detach(g_key_thread);
        g_key_thread_started = 1;
    }
}
#endif

static const char *getenv_default(const char *name, const char *dflt)
{
    return getenv(name) ?: dflt;
}

static int get_st7789v_fbdev(char *dev_path, size_t buf_size)
{
    if (dev_path == NULL || buf_size == 0)
    {
        return -1;
    }

    FILE *fp = fopen("/proc/fb", "r");
    if (fp == NULL)
    {
        perror("Failed to open /proc/fb");
        return -1;
    }

    char line[256];
    int fb_num = -1;
    while (fgets(line, sizeof(line), fp) != NULL)
    {
        if (strstr(line, "fb_st7789v") != NULL)
        {
            if (sscanf(line, "%d", &fb_num) == 1)
            {
                break;
            }
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

#if LV_USE_EVDEV
static void lv_linux_indev_init(void)
{
    const char *mouse_device = getenv_default("LV_LINUX_MOUSE_DEVICE", NULL);
    if (mouse_device)
    {
        lv_evdev_create(LV_INDEV_TYPE_POINTER, mouse_device);
    }

    const char *keyboard_device = getenv_default(
        "LV_LINUX_KEYBOARD_DEVICE",
        "/dev/input/by-path/platform-3f804000.i2c-event");
    if (keyboard_device)
    {
        g_keyboard_indev = lv_evdev_create(LV_INDEV_TYPE_KEYPAD, keyboard_device);
    }
}
#endif

#if LV_USE_LINUX_FBDEV
static void lv_linux_disp_init(void)
{
    const char *device = NULL;
    char fbdev[64] = {0};
    device = getenv_default("LV_LINUX_FBDEV_DEVICE", NULL);
    if ((device == NULL) && (get_st7789v_fbdev(fbdev, sizeof(fbdev)) == 0))
    {
        device = fbdev;
    }

    lv_display_t *disp = lv_linux_fbdev_create();
    if (disp == NULL)
    {
        printf("Failed to create fbdev display!\n");
        return;
    }

    lv_linux_fbdev_set_file(disp, device);
}

#if !LV_USE_EVDEV && !LV_USE_LIBINPUT
static void lv_linux_indev_init(void)
{
}
#endif

#elif LV_USE_LINUX_DRM
static void lv_linux_disp_init(void)
{
    const char *device = getenv_default("LV_LINUX_DRM_CARD", "/dev/dri/card0");
    lv_display_t *disp = lv_linux_drm_create();
    lv_linux_drm_set_file(disp, device, -1);
}

#elif LV_USE_SDL
static void lv_linux_disp_init(void)
{
    const int width = atoi(getenv("LV_SDL_VIDEO_WIDTH") ?: "320");
    const int height = atoi(getenv("LV_SDL_VIDEO_HEIGHT") ?: "170");
    lv_sdl_window_create(width, height);
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
    lv_init();

    lv_linux_disp_init();
    lv_linux_indev_init();

    ui_init();

#if !LV_USE_SDL
    app_start_key_monitor();
#endif

    lv_obj_t *screen = lv_screen_active();
    lv_obj_add_event_cb(screen, app_key_event_cb, LV_EVENT_KEY, nullptr);

    lv_group_t *group = ui_get_input_group();
    if (group != nullptr && g_keyboard_indev != nullptr)
    {
        lv_group_add_obj(group, screen);
        lv_group_set_default(group);
        lv_indev_set_group(g_keyboard_indev, group);
    }

    int startup_ticks = 0;
    while (!app_should_quit())
    {
        lv_timer_handler();
        if (!g_app_quit_enabled && ++startup_ticks >= 5)
        {
            g_app_quit_enabled = 1;
        }
        usleep(1000);
    }

    ui_shutdown();
    return 0;
}
