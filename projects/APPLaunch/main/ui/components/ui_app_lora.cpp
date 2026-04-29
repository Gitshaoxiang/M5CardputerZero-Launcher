#include "ui_app_lora.hpp"
#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "RadioLib.h"

// LoRa UI控件和状态变量
static lv_obj_t* lora_panel        = NULL;
static lv_obj_t* lora_status_label = NULL;
static lv_obj_t* lora_msg_label    = NULL;
static lv_timer_t* lora_timer      = NULL;
static bool lora_hw_inited         = false;

// go_back_home 回调（由 APPLaunch 设置）
static void (*go_back_home_cb)(void) = NULL;

// 示例：硬件初始化（可移植 main.cpp 相关代码）
static void lora_hw_init()
{
    if (lora_hw_inited) return;
    // TODO: 移植 main.cpp lora_init_hardware 相关内容
    lora_hw_inited = true;
}

// 示例：收发消息（可移植 main.cpp 相关代码）
static void lora_send_demo()
{
    // TODO: 移植 main.cpp lora_send_demo_packet 相关内容
    if (lora_msg_label) lv_label_set_text(lora_msg_label, "已发送: Hello LoRa");
}

static void lora_update_status(const char* text)
{
    if (lora_status_label) lv_label_set_text(lora_status_label, text);
}

static void lora_on_timer(lv_timer_t* timer)
{
    // TODO: 移植 main.cpp lora_app_task/lora_poll_irq_and_update_ui 相关内容
    lora_update_status("LoRa 状态: 正常");
}

static void lora_on_back(lv_event_t* e)
{
    if (go_back_home_cb) go_back_home_cb();
}

void ui_app_lora_create(lv_obj_t* parent)
{
    if (lora_panel) return;
    lora_panel = lv_obj_create(parent);
    lv_obj_set_size(lora_panel, 320, 240);
    lv_obj_center(lora_panel);
    lv_obj_set_style_bg_color(lora_panel, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_radius(lora_panel, 8, LV_PART_MAIN);

    lora_status_label = lv_label_create(lora_panel);
    lv_label_set_text(lora_status_label, "LoRa 状态: 初始化中...");
    lv_obj_align(lora_status_label, LV_ALIGN_TOP_MID, 0, 10);

    lora_msg_label = lv_label_create(lora_panel);
    lv_label_set_text(lora_msg_label, "消息: 暂无");
    lv_obj_align(lora_msg_label, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* btn = lv_btn_create(lora_panel);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_t* btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "发送演示");
    lv_obj_center(btn_label);
    lv_obj_add_event_cb(btn, [](lv_event_t* e) { lora_send_demo(); }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* back_btn = lv_btn_create(lora_panel);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_LEFT, 10, -20);
    lv_obj_t* back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "返回");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, lora_on_back, LV_EVENT_CLICKED, NULL);

    lora_hw_init();
    lora_update_status("LoRa 状态: 已初始化");
    if (!lora_timer) lora_timer = lv_timer_create(lora_on_timer, 1000, NULL);
}

void lora_app_task()
{
    // 可用于外部周期性调用
    lora_on_timer(NULL);
}

// 供 APPLaunch 设置 go_back_home 回调
extern "C" void lora_set_go_back_home(void (*cb)(void))
{
    go_back_home_cb = cb;
}
