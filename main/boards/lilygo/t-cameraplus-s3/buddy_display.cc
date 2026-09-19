#include "buddy_display.h"
#include "application.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "board.h"
#include "material_symbols.h"

#include <esp_log.h>
#include <esp_random.h>
#include <cstring>

#define TAG "BuddyDisplay"

namespace {

constexpr int kEyeWidth = 34;
constexpr int kEyeHeight = 44;
constexpr int kEyeGap = 56;
constexpr int kEyeOffsetY = -26;
constexpr int kEyeRadius = 12;
constexpr int kBlinkHeight = 4;
constexpr int kBlinkDurationMs = 90;
constexpr int kBlinkMinIntervalMs = 2500;
constexpr int kBlinkExtraIntervalMs = 3500;
constexpr int kMouthWidth = 44;
constexpr int kMouthClosedHeight = 6;
constexpr int kMouthOpenHeight = 26;
constexpr int kMouthOffsetY = 30;
constexpr int kMouthRadius = 10;
constexpr int kTalkCycleMs = 160;
constexpr int kStateTickMs = 100;
constexpr int kTalkBorderWidth = 4;
constexpr int kFaceLayerIndex = 1;
constexpr uint32_t kAccentColor = 0x4FC3F7;
constexpr uint32_t kListeningColor = 0xFF7043;
constexpr int kSettingsButtonSize = 36;
constexpr int kPanelPadding = 12;
constexpr int kPanelRowGap = 10;
constexpr int kRollerVisibleRows = 3;
constexpr const char* kDarkThemeName = "dark";
constexpr const char* kLightThemeName = "light";
constexpr const char* kSleepOptions = "Never\n30 s\n1 min\n2 min\n5 min\n10 min";
constexpr int kSleepOptionSeconds[] = {-1, 30, 60, 120, 300, 600};
constexpr int kSleepOptionCount = sizeof(kSleepOptionSeconds) / sizeof(kSleepOptionSeconds[0]);

int SleepOptionIndex(int seconds) {
    for (int i = 0; i < kSleepOptionCount; ++i) {
        if (kSleepOptionSeconds[i] == seconds) {
            return i;
        }
    }
    return 2;
}

void SetHeightAnimation(void* var, int32_t value) {
    lv_obj_set_height(static_cast<lv_obj_t*>(var), value);
}

}  // namespace

BuddyDisplay::BuddyDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                           int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {}

BuddyDisplay::~BuddyDisplay() {
    DisplayLockGuard lock(this);
    if (state_timer_ != nullptr) {
        lv_timer_delete(state_timer_);
    }
    if (blink_timer_ != nullptr) {
        lv_timer_delete(blink_timer_);
    }
}

void BuddyDisplay::SetControls(BuddyControls controls) {
    controls_ = std::move(controls);
}

void BuddyDisplay::SetupUI() {
    if (setup_ui_called_) {
        return;
    }
    LcdDisplay::SetupUI();

    DisplayLockGuard lock(this);
    auto screen = lv_screen_active();
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    CreateFace(screen);
    CreateTalkSurface(screen);
    CreateSettingsButton(screen);
    CreateSettingsPanel(screen);
    ApplyFaceColors();
    ApplyPanelColors();
    ApplyMood(FaceMood::kNeutral);

    state_timer_ = lv_timer_create(StateTimerCallback, kStateTickMs, this);
    blink_timer_ = lv_timer_create(BlinkTimerCallback, kBlinkMinIntervalMs, this);
}

void BuddyDisplay::CreateFace(lv_obj_t* screen) {
    face_ = lv_obj_create(screen);
    lv_obj_set_size(face_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_opa(face_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(face_, 0, 0);
    lv_obj_set_style_pad_all(face_, 0, 0);
    lv_obj_remove_flag(face_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(face_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(face_, kFaceLayerIndex);

    left_eye_ = lv_obj_create(face_);
    right_eye_ = lv_obj_create(face_);
    mouth_ = lv_obj_create(face_);
    for (auto part : {left_eye_, right_eye_, mouth_}) {
        lv_obj_set_style_border_width(part, 0, 0);
        lv_obj_set_style_pad_all(part, 0, 0);
        lv_obj_remove_flag(part, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(part, LV_OBJ_FLAG_CLICKABLE);
    }
}

void BuddyDisplay::CreateTalkSurface(lv_obj_t* screen) {
    talk_surface_ = lv_obj_create(screen);
    lv_obj_set_size(talk_surface_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_opa(talk_surface_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(talk_surface_, 0, 0);
    lv_obj_set_style_border_color(talk_surface_, lv_color_hex(kListeningColor), 0);
    lv_obj_set_style_radius(talk_surface_, 0, 0);
    lv_obj_set_style_pad_all(talk_surface_, 0, 0);
    lv_obj_remove_flag(talk_surface_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(talk_surface_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(talk_surface_);
    lv_obj_add_event_cb(talk_surface_, TalkSurfaceEventCallback, LV_EVENT_ALL, this);
}

void BuddyDisplay::CreateSettingsButton(lv_obj_t* screen) {
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    settings_button_ = lv_button_create(screen);
    lv_obj_set_size(settings_button_, kSettingsButtonSize, kSettingsButtonSize);
    lv_obj_align(settings_button_, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_opa(settings_button_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(settings_button_, 0, 0);
    lv_obj_set_style_border_width(settings_button_, 0, 0);
    lv_obj_set_style_pad_all(settings_button_, 0, 0);
    lv_obj_move_foreground(settings_button_);
    auto icon = lv_label_create(settings_button_);
    lv_label_set_text(icon, MATERIAL_SYMBOLS_SETTINGS);
    lv_obj_set_style_text_font(icon, lvgl_theme->icon_font()->font(), 0);
    lv_obj_set_style_text_opa(icon, LV_OPA_60, 0);
    lv_obj_center(icon);
    lv_obj_add_event_cb(settings_button_, SettingsButtonEventCallback, LV_EVENT_CLICKED, this);
}

void BuddyDisplay::CreateSettingsPanel(lv_obj_t* screen) {
    settings_panel_ = lv_obj_create(screen);
    lv_obj_set_size(settings_panel_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(settings_panel_, 0, 0);
    lv_obj_set_style_border_width(settings_panel_, 0, 0);
    lv_obj_set_style_pad_all(settings_panel_, kPanelPadding, 0);
    lv_obj_set_style_pad_row(settings_panel_, kPanelRowGap, 0);
    lv_obj_set_flex_flow(settings_panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(settings_panel_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(settings_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(settings_panel_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(settings_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(settings_panel_);

    auto title = lv_label_create(settings_panel_);
    lv_label_set_text(title, "Settings");

    auto dark_row = lv_obj_create(settings_panel_);
    lv_obj_set_size(dark_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(dark_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dark_row, 0, 0);
    lv_obj_set_style_pad_all(dark_row, 0, 0);
    lv_obj_set_flex_flow(dark_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dark_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(dark_row, LV_OBJ_FLAG_SCROLLABLE);
    auto dark_label = lv_label_create(dark_row);
    lv_label_set_text(dark_label, "Dark mode");
    dark_mode_switch_ = lv_switch_create(dark_row);
    if (current_theme_ != nullptr && current_theme_->name() == kDarkThemeName) {
        lv_obj_add_state(dark_mode_switch_, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(dark_mode_switch_, DarkModeEventCallback, LV_EVENT_VALUE_CHANGED, this);

    auto sleep_label = lv_label_create(settings_panel_);
    lv_label_set_text(sleep_label, "Sleep after");
    sleep_roller_ = lv_roller_create(settings_panel_);
    lv_roller_set_options(sleep_roller_, kSleepOptions, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(sleep_roller_, kRollerVisibleRows);
    lv_obj_set_width(sleep_roller_, LV_PCT(100));
    if (controls_.get_sleep_seconds) {
        lv_roller_set_selected(sleep_roller_, SleepOptionIndex(controls_.get_sleep_seconds()), LV_ANIM_OFF);
    }
    lv_obj_add_event_cb(sleep_roller_, SleepRollerEventCallback, LV_EVENT_VALUE_CHANGED, this);

    auto close_button = lv_button_create(settings_panel_);
    lv_obj_set_width(close_button, LV_PCT(100));
    auto close_label = lv_label_create(close_button);
    lv_label_set_text(close_label, "Close");
    lv_obj_center(close_label);
    lv_obj_add_event_cb(close_button, CloseButtonEventCallback, LV_EVENT_CLICKED, this);
}

void BuddyDisplay::ApplyPanelColors() {
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    if (lvgl_theme == nullptr || settings_panel_ == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(settings_panel_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_text_color(settings_panel_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(settings_button_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(sleep_roller_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_text_color(sleep_roller_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(sleep_roller_, lv_color_hex(kAccentColor), LV_PART_SELECTED);
    lv_obj_set_style_bg_color(dark_mode_switch_, lv_color_hex(kAccentColor), static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | static_cast<lv_style_selector_t>(LV_STATE_CHECKED));
}

void BuddyDisplay::OpenSettings() {
    lv_obj_remove_flag(settings_panel_, LV_OBJ_FLAG_HIDDEN);
}

void BuddyDisplay::CloseSettings() {
    lv_obj_add_flag(settings_panel_, LV_OBJ_FLAG_HIDDEN);
}

void BuddyDisplay::OnDarkModeChanged() {
    bool dark = lv_obj_has_state(dark_mode_switch_, LV_STATE_CHECKED);
    auto theme = LvglThemeManager::GetInstance().GetTheme(dark ? kDarkThemeName : kLightThemeName);
    if (theme != nullptr) {
        SetTheme(theme);
    }
}

void BuddyDisplay::OnSleepOptionChanged() {
    auto index = lv_roller_get_selected(sleep_roller_);
    if (index < static_cast<uint32_t>(kSleepOptionCount) && controls_.set_sleep_seconds) {
        controls_.set_sleep_seconds(kSleepOptionSeconds[index]);
    }
}

void BuddyDisplay::SetPowerSaveMode(bool on) {
    sleeping_ = on;
    LcdDisplay::SetPowerSaveMode(on);
}

void BuddyDisplay::ApplyFaceColors() {
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    if (lvgl_theme == nullptr || left_eye_ == nullptr) {
        return;
    }
    for (auto part : {left_eye_, right_eye_}) {
        lv_obj_set_style_bg_color(part, lvgl_theme->text_color(), 0);
        lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_bg_color(mouth_, lv_color_hex(kAccentColor), 0);
    lv_obj_set_style_bg_opa(mouth_, LV_OPA_COVER, 0);
}

void BuddyDisplay::SetTheme(Theme* theme) {
    LcdDisplay::SetTheme(theme);
    DisplayLockGuard lock(this);
    ApplyFaceColors();
    ApplyPanelColors();
}

void BuddyDisplay::SetEyes(int width, int height, int offset_x, int offset_y) {
    lv_anim_delete(left_eye_, SetHeightAnimation);
    lv_anim_delete(right_eye_, SetHeightAnimation);
    for (auto part : {left_eye_, right_eye_}) {
        lv_obj_set_size(part, width, height);
        lv_obj_set_style_radius(part, kEyeRadius, 0);
    }
    lv_obj_align(left_eye_, LV_ALIGN_CENTER, -kEyeGap / 2 + offset_x, kEyeOffsetY + offset_y);
    lv_obj_align(right_eye_, LV_ALIGN_CENTER, kEyeGap / 2 + offset_x, kEyeOffsetY + offset_y);
}

void BuddyDisplay::SetMouth(int width, int height, int offset_y, int radius) {
    lv_anim_delete(mouth_, SetHeightAnimation);
    lv_obj_set_size(mouth_, width, height);
    lv_obj_set_style_radius(mouth_, radius, 0);
    lv_obj_align(mouth_, LV_ALIGN_CENTER, 0, offset_y);
}

void BuddyDisplay::StartMouthTalking() {
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, mouth_);
    lv_anim_set_exec_cb(&anim, SetHeightAnimation);
    lv_anim_set_values(&anim, kMouthClosedHeight, kMouthOpenHeight);
    lv_anim_set_duration(&anim, kTalkCycleMs);
    lv_anim_set_reverse_duration(&anim, kTalkCycleMs);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim);
}

void BuddyDisplay::StopMouthTalking() {
    lv_anim_delete(mouth_, SetHeightAnimation);
}

void BuddyDisplay::Blink() {
    if (mood_ == FaceMood::kSleepy || mood_ == FaceMood::kHappy) {
        return;
    }
    for (auto part : {left_eye_, right_eye_}) {
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, part);
        lv_anim_set_exec_cb(&anim, SetHeightAnimation);
        lv_anim_set_values(&anim, lv_obj_get_height(part), kBlinkHeight);
        lv_anim_set_duration(&anim, kBlinkDurationMs);
        lv_anim_set_reverse_duration(&anim, kBlinkDurationMs);
        lv_anim_start(&anim);
    }
}

void BuddyDisplay::ApplyMood(FaceMood mood) {
    mood_ = mood;
    StopMouthTalking();
    switch (mood) {
        case FaceMood::kListening:
            SetEyes(kEyeWidth + 6, kEyeHeight + 10, 0, 0);
            SetMouth(kMouthWidth - 14, kMouthClosedHeight, kMouthOffsetY, kMouthRadius);
            break;
        case FaceMood::kThinking:
            SetEyes(kEyeWidth, kEyeHeight - 8, 12, -8);
            SetMouth(kMouthWidth - 20, kMouthClosedHeight, kMouthOffsetY + 4, kMouthRadius);
            break;
        case FaceMood::kSpeaking:
            SetEyes(kEyeWidth, kEyeHeight, 0, 0);
            SetMouth(kMouthWidth, kMouthClosedHeight, kMouthOffsetY, kMouthRadius);
            StartMouthTalking();
            break;
        case FaceMood::kHappy:
            SetEyes(kEyeWidth + 2, kEyeHeight / 3, 0, 4);
            SetMouth(kMouthWidth + 16, kMouthOpenHeight - 6, kMouthOffsetY, kMouthRadius + 6);
            break;
        case FaceMood::kSad:
            SetEyes(kEyeWidth - 4, kEyeHeight - 14, 0, 6);
            SetMouth(kMouthWidth - 10, kMouthClosedHeight - 2, kMouthOffsetY + 10, kMouthRadius);
            break;
        case FaceMood::kSurprised:
            SetEyes(kEyeWidth + 8, kEyeHeight + 8, 0, -4);
            SetMouth(kMouthOpenHeight, kMouthOpenHeight, kMouthOffsetY, kMouthOpenHeight / 2);
            break;
        case FaceMood::kSleepy:
            SetEyes(kEyeWidth, kBlinkHeight, 0, 12);
            SetMouth(kMouthWidth - 20, kMouthClosedHeight, kMouthOffsetY, kMouthRadius);
            break;
        case FaceMood::kNeutral:
        default:
            SetEyes(kEyeWidth, kEyeHeight, 0, 0);
            SetMouth(kMouthWidth, kMouthClosedHeight, kMouthOffsetY, kMouthRadius);
            break;
    }
}

FaceMood BuddyDisplay::MoodFor(DeviceState state) const {
    switch (state) {
        case kDeviceStateListening:
            return FaceMood::kListening;
        case kDeviceStateSpeaking:
            return FaceMood::kSpeaking;
        case kDeviceStateConnecting:
        case kDeviceStateStarting:
        case kDeviceStateActivating:
        case kDeviceStateUpgrading:
            return FaceMood::kThinking;
        case kDeviceStateFatalError:
            return FaceMood::kSad;
        default:
            break;
    }
    if (emotion_ == "thinking") {
        return FaceMood::kThinking;
    }
    if (emotion_ == "happy" || emotion_ == "laughing" || emotion_ == "funny" || emotion_ == "loving" ||
        emotion_ == "winking" || emotion_ == "cool" || emotion_ == "delicious" || emotion_ == "kissy") {
        return FaceMood::kHappy;
    }
    if (emotion_ == "sad" || emotion_ == "crying" || emotion_ == "angry" || emotion_ == "embarrassed") {
        return FaceMood::kSad;
    }
    if (emotion_ == "surprised" || emotion_ == "shocked" || emotion_ == "confused") {
        return FaceMood::kSurprised;
    }
    if (emotion_ == "sleepy") {
        return FaceMood::kSleepy;
    }
    return FaceMood::kNeutral;
}

void BuddyDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    emotion_ = emotion != nullptr ? emotion : "neutral";
    if (face_ != nullptr) {
        auto mood = MoodFor(Application::GetInstance().GetDeviceState());
        if (mood != mood_) {
            ApplyMood(mood);
        }
    }
}

void BuddyDisplay::OnStateTick() {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (stop_listening_pending_ && state == kDeviceStateListening) {
        stop_listening_pending_ = false;
        app.StopListening();
    }
    if (state != last_state_) {
        last_state_ = state;
        auto mood = MoodFor(state);
        if (mood != mood_) {
            ApplyMood(mood);
        }
    }
}

void BuddyDisplay::OnTalkPressed() {
    if (sleeping_) {
        if (controls_.wake_up) {
            controls_.wake_up();
        }
        return;
    }
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    ESP_LOGI(TAG, "Talk surface pressed in state %d", static_cast<int>(state));
    if (state != kDeviceStateIdle && state != kDeviceStateSpeaking && state != kDeviceStateListening) {
        return;
    }
    lv_obj_set_style_border_width(talk_surface_, kTalkBorderWidth, 0);
    stop_listening_pending_ = false;
    if (state != kDeviceStateListening) {
        app.StartListening();
    }
}

void BuddyDisplay::OnTalkReleased() {
    lv_obj_set_style_border_width(talk_surface_, 0, 0);
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    ESP_LOGI(TAG, "Talk surface released in state %d", static_cast<int>(state));
    if (state == kDeviceStateListening) {
        app.StopListening();
    } else if (state == kDeviceStateConnecting) {
        stop_listening_pending_ = true;
    }
}

void BuddyDisplay::StateTimerCallback(lv_timer_t* timer) {
    static_cast<BuddyDisplay*>(lv_timer_get_user_data(timer))->OnStateTick();
}

void BuddyDisplay::BlinkTimerCallback(lv_timer_t* timer) {
    auto display = static_cast<BuddyDisplay*>(lv_timer_get_user_data(timer));
    display->Blink();
    lv_timer_set_period(timer, kBlinkMinIntervalMs + esp_random() % kBlinkExtraIntervalMs);
}

void BuddyDisplay::SettingsButtonEventCallback(lv_event_t* event) {
    static_cast<BuddyDisplay*>(lv_event_get_user_data(event))->OpenSettings();
}

void BuddyDisplay::CloseButtonEventCallback(lv_event_t* event) {
    static_cast<BuddyDisplay*>(lv_event_get_user_data(event))->CloseSettings();
}

void BuddyDisplay::DarkModeEventCallback(lv_event_t* event) {
    static_cast<BuddyDisplay*>(lv_event_get_user_data(event))->OnDarkModeChanged();
}

void BuddyDisplay::SleepRollerEventCallback(lv_event_t* event) {
    static_cast<BuddyDisplay*>(lv_event_get_user_data(event))->OnSleepOptionChanged();
}

void BuddyDisplay::TalkSurfaceEventCallback(lv_event_t* event) {
    auto display = static_cast<BuddyDisplay*>(lv_event_get_user_data(event));
    switch (lv_event_get_code(event)) {
        case LV_EVENT_PRESSED:
            display->OnTalkPressed();
            break;
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
            display->OnTalkReleased();
            break;
        default:
            break;
    }
}
