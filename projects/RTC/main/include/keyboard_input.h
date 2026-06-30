#ifndef RTC_KEYBOARD_INPUT_H
#define RTC_KEYBOARD_INPUT_H

#include <pthread.h>
#include <stdint.h>
#include <sys/queue.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KBD_MOD_SHIFT  (1u << 0)
#define KBD_MOD_CTRL   (1u << 1)
#define KBD_MOD_ALT    (1u << 2)
#define KBD_MOD_LOGO   (1u << 3)
#define KBD_MOD_CAPS   (1u << 4)
#define KBD_MOD_NUM    (1u << 5)

#define KBD_KEY_RELEASED  0
#define KBD_KEY_PRESSED   1
#define KBD_KEY_REPEATED  2

struct key_item {
    uint32_t key_code;
    uint32_t keysym;
    uint32_t codepoint;
    uint32_t mods;
    int key_state;
    char sym_name[65];
    char utf8[16];
    char flage;
    STAILQ_ENTRY(key_item) entries;
};

STAILQ_HEAD(keyboard_queue_t, key_item);

extern struct keyboard_queue_t keyboard_queue;
extern pthread_mutex_t keyboard_mutex;
extern volatile int LVGL_RUN_FLAGE;

void *keyboard_read_thread(void *argv);

#ifdef __cplusplus
}
#endif

#endif
