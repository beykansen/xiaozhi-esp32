#ifndef BUDDY_DISPLAY_H
#define BUDDY_DISPLAY_H

#include "display/lcd_display.h"
#include "device_state.h"

#include <lvgl.h>
#include <functional>
#include <string>

enum class FaceMood {
    kNeutral,
    kListening,
    kThinking,
    kSpeaking,
    kHappy,
    kSad,
    kSurprised,
    kSleepy,
};

struct BuddyControls {
    std::function<int()> get_sleep_seconds;
    std::function<void(int)> set_sleep_seconds;
    std::function<void()> wake_up;
};

class BuddyDisplay : public SpiLcdDisplay {
public:
    BuddyDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                 int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy);
    ~BuddyDisplay();

    void SetControls(BuddyControls controls);

    virtual void SetupUI() override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void SetPowerSaveMode(bool on) override;

private:
    BuddyControls controls_;
    lv_obj_t* face_ = nullptr;
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* mouth_ = nullptr;
    lv_obj_t* talk_surface_ = nullptr;
    lv_obj_t* settings_button_ = nullptr;
    lv_obj_t* settings_panel_ = nullptr;
    lv_obj_t* dark_mode_switch_ = nullptr;
    lv_obj_t* sleep_roller_ = nullptr;
    lv_timer_t* state_timer_ = nullptr;
    lv_timer_t* blink_timer_ = nullptr;
    std::string emotion_ = "neutral";
    FaceMood mood_ = FaceMood::kNeutral;
    DeviceState last_state_ = kDeviceStateUnknown;
    bool stop_listening_pending_ = false;
    bool sleeping_ = false;

    void CreateFace(lv_obj_t* screen);
    void CreateTalkSurface(lv_obj_t* screen);
    void CreateSettingsButton(lv_obj_t* screen);
    void CreateSettingsPanel(lv_obj_t* screen);
    void ApplyFaceColors();
    void ApplyPanelColors();
    void ApplyMood(FaceMood mood);
    void SetEyes(int width, int height, int offset_x, int offset_y);
    void SetMouth(int width, int height, int offset_y, int radius);
    void StartMouthTalking();
    void StopMouthTalking();
    void Blink();
    void OnTalkPressed();
    void OnTalkReleased();
    void OnStateTick();
    void OpenSettings();
    void CloseSettings();
    void OnDarkModeChanged();
    void OnSleepOptionChanged();
    FaceMood MoodFor(DeviceState state) const;

    static void StateTimerCallback(lv_timer_t* timer);
    static void BlinkTimerCallback(lv_timer_t* timer);
    static void TalkSurfaceEventCallback(lv_event_t* event);
    static void SettingsButtonEventCallback(lv_event_t* event);
    static void CloseButtonEventCallback(lv_event_t* event);
    static void DarkModeEventCallback(lv_event_t* event);
    static void SleepRollerEventCallback(lv_event_t* event);
};

#endif
