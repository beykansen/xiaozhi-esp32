#ifndef BUDDY_DISPLAY_H
#define BUDDY_DISPLAY_H

#include "display/lcd_display.h"
#include "device_state.h"

#include <lvgl.h>
#include <functional>
#include <string>
#include <utility>
#include <vector>

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

enum class BuddyScreen {
    kChat,
    kMenu,
    kStatus,
    kSettings,
};

using StatusLines = std::vector<std::pair<std::string, std::string>>;

struct BuddyControls {
    std::function<int()> get_sleep_seconds;
    std::function<void(int)> set_sleep_seconds;
    std::function<void()> wake_up;
    std::function<StatusLines()> get_status_lines;
    std::function<void()> reset_conversation;
};

class BuddyDisplay : public SpiLcdDisplay {
public:
    BuddyDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                 int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy);
    ~BuddyDisplay();

    void SetControls(BuddyControls controls);
    bool OnMainButtonClick();
    void OnMainButtonLongPress();
    void OnMainButtonReleased();
    bool IsBusy() const { return thinking_ || sleeping_; }
    bool IsChatScreen() const { return screen_ == BuddyScreen::kChat; }

    virtual void SetupUI() override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void SetPowerSaveMode(bool on) override;

private:
    BuddyControls controls_;
    lv_obj_t* face_ = nullptr;
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* mouth_ = nullptr;
    lv_obj_t* talk_surface_ = nullptr;
    lv_obj_t* clear_chat_button_ = nullptr;
    lv_obj_t* menu_panel_ = nullptr;
    lv_obj_t* menu_items_[2] = {nullptr, nullptr};
    lv_obj_t* status_panel_ = nullptr;
    lv_obj_t* status_list_ = nullptr;
    lv_obj_t* settings_panel_ = nullptr;
    lv_obj_t* dark_mode_switch_ = nullptr;
    lv_obj_t* sleep_dropdown_ = nullptr;
    lv_timer_t* state_timer_ = nullptr;
    lv_timer_t* blink_timer_ = nullptr;
    lv_timer_t* subtitle_timer_ = nullptr;
    lv_timer_t* thinking_timer_ = nullptr;
    std::string emotion_ = "neutral";
    FaceMood mood_ = FaceMood::kNeutral;
    BuddyScreen screen_ = BuddyScreen::kChat;
    int menu_index_ = 0;
    int status_ticks_ = 0;
    DeviceState last_state_ = kDeviceStateUnknown;
    bool stop_listening_pending_ = false;
    bool sleeping_ = false;
    bool thinking_ = false;
    bool button_listening_ = false;

    void CreateFace(lv_obj_t* screen);
    void CreateTalkSurface(lv_obj_t* screen);
    void CreateClearChatButton(lv_obj_t* screen);
    void HideStockEmoji();
    lv_obj_t* CreatePanel(lv_obj_t* screen, const char* title);
    void CreateMenuPanel(lv_obj_t* screen);
    void CreateStatusPanel(lv_obj_t* screen);
    void CreateSettingsPanel(lv_obj_t* screen);
    void ApplyThemeColors();
    void ApplyMood(FaceMood mood);
    void SetEyes(int width, int height, int offset_x, int offset_y);
    void SetMouth(int width, int height, int offset_y, int radius);
    void StartMouthTalking();
    void StartThinkingEyes();
    void StopThinkingEyes();
    bool BeginListening();
    void EndListening();
    void StopMouthTalking();
    void Blink();
    void ShowScreen(BuddyScreen screen);
    void HighlightMenuItem(int index);
    void OpenMenuItem(int index);
    void RefreshStatus();
    void SetThinking(bool thinking);
    void OnTalkPressed();
    void OnTalkReleased();
    void OnStateTick();
    void OnDarkModeChanged();
    void OnSleepOptionChanged();
    FaceMood MoodFor(DeviceState state) const;

    static void StateTimerCallback(lv_timer_t* timer);
    static void BlinkTimerCallback(lv_timer_t* timer);
    static void SubtitleTimerCallback(lv_timer_t* timer);
    static void ThinkingTimerCallback(lv_timer_t* timer);
    static void TalkSurfaceEventCallback(lv_event_t* event);
    static void MenuItemEventCallback(lv_event_t* event);
    static void DarkModeEventCallback(lv_event_t* event);
    static void SleepDropdownEventCallback(lv_event_t* event);
    static void ClearChatEventCallback(lv_event_t* event);
};

#endif
