#include "events/SDL_touch_c.h"
#include "video/SDL_sysvideo.h"
#include "SDL_espidftouch.h"
#include <stdbool.h>

#include "SDL_espidfshared.h"
#include "esp_log.h"

#define ESPIDF_TOUCH_ID         1
#define ESPIDF_TOUCH_FINGER     1


void ESPIDF_InitTouch(void)
{
    esp_err_t ret = esp_bsp_sdl_touch_init();
    if (ret == ESP_OK) {
        SDL_AddTouch(ESPIDF_TOUCH_ID, SDL_TOUCH_DEVICE_DIRECT, "Touchscreen");
        ESP_LOGI("SDL", "ESPIDF_InitTouch - Touch support enabled");
    } else if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI("SDL", "ESPIDF_InitTouch - Touch not supported on this board");
    } else {
        ESP_LOGE("SDL", "ESPIDF_InitTouch - Touch initialization failed: %s", esp_err_to_name(ret));
    }
}

void ESPIDF_PumpTouchEvent(SDL_VideoDevice *_this)
{
    if (!display_config.has_touch) {
        return;
    }

    static bool was_pressed = false;
    esp_bsp_sdl_touch_info_t touch_info;

    esp_err_t ret = esp_bsp_sdl_touch_read(&touch_info);
    if (ret != ESP_OK) {
        return;
    }

    /* 取设备的第一个窗口：NULL 会导致合成鼠标事件被丢弃 */
    SDL_Window *window = _this ? _this->windows : NULL;

    /* SDL 触摸坐标为归一化 0..1；面板像素必须除以显示分辨率，
       否则合成鼠标事件被钳到窗口边缘/外 -> 应用层永远收不到点击 */
    const float nx = (float)touch_info.x / (float)display_config.width;
    const float ny = (float)touch_info.y / (float)display_config.height;

    if (touch_info.pressed != was_pressed) {
        was_pressed = touch_info.pressed;
        ESP_LOGD("SDL", "touch state: %d, [%d, %d]", touch_info.pressed, touch_info.x, touch_info.y);
        /* 注意：type 参数是 SDL_EventType，必须传 FINGER_DOWN/UP；
           传 bool(0/1) 会让 down 恒为 false，触摸永远无法按下 */
        SDL_SendTouch(0, ESPIDF_TOUCH_ID, ESPIDF_TOUCH_FINGER,
                      window,
                      touch_info.pressed ? SDL_EVENT_FINGER_DOWN : SDL_EVENT_FINGER_UP,
                      nx,
                      ny,
                      touch_info.pressed ? 1.0f : 0.0f);
    } else if (touch_info.pressed) {
        SDL_SendTouchMotion(0, ESPIDF_TOUCH_ID, ESPIDF_TOUCH_FINGER,
                            window,
                            nx,
                            ny,
                            1.0f);
    }
}

int ESPIDF_CalibrateTouch(float screenX[], float screenY[], float touchX[], float touchY[])
{
    return 0;
}

void ESPIDF_ChangeTouchMode(int raw)
{
    return;
}

void ESPIDF_ReadTouchRawPosition(float* x, float* y)
{
    return;
}

void ESPIDF_QuitTouch(void)
{
    // ts_close(ts);
}
