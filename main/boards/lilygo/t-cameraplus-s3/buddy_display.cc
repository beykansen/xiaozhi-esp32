#include "buddy_display.h"
#include "application.h"
#include "board.h"
#include "display/lvgl_display/lvgl_theme.h"
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
constexpr int kStatusRefreshTicks = 10;
constexpr int kTalkBorderWidth = 4;
constexpr int kFaceLayerIndex = 1;
constexpr int kPanelPadding = 12;
constexpr int kPanelRowGap = 8;
constexpr int kMenuItemHeight = 64;
constexpr int kClearButtonSize = 36;
constexpr int kThinkingEyeDrift = 7;
constexpr int kThinkingCycleMs = 650;
constexpr int kSubtitleMsPerChar = 90;
constexpr int kSubtitleMinScrollMs = 4000;
constexpr int kSubtitleLingerMs = 12000;
constexpr int kThinkingTimeoutMs = 45000;
constexpr uint32_t kAccentColor = 0x4FC3F7;
constexpr uint32_t kListeningColor = 0xFF7043;
constexpr const char* kDarkThemeName = "dark";
constexpr const char* kLightThemeName = "light";
constexpr const char* kSleepOptions = "Never\n30 s\n1 min\n2 min\n5 min\n10 min";
constexpr int kSleepOptionSeconds[] = {-1, 30, 60, 120, 300, 600};
constexpr int kSleepOptionCount = sizeof(kSleepOptionSeconds) / sizeof(kSleepOptionSeconds[0]);
constexpr int kMenuItemCount = 2;
constexpr const char* kMenuIcons[kMenuItemCount] = {MATERIAL_SYMBOLS_INFO, MATERIAL_SYMBOLS_SETTINGS};
constexpr const char* kMenuLabels[kMenuItemCount] = {"Status", "Settings"};
constexpr BuddyScreen kMenuTargets[kMenuItemCount] = {BuddyScreen::kStatus, BuddyScreen::kSettings};

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

void SetTranslateYAnimation(void* var, int32_t value) {
    lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(var), value, 0);
}

}  // namespace

BuddyDisplay::BuddyDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                           int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {}

BuddyDisplay::~BuddyDisplay() {
    DisplayLockGuard lock(this);
    for (auto timer : {state_timer_, blink_timer_, subtitle_timer_, thinking_timer_}) {
        if (timer != nullptr) {
            lv_timer_delete(timer);
        }
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
    HideStockEmoji();
    CreateFace(screen);
    CreateTalkSurface(screen);
    CreateClearChatButton(screen);
    CreateMenuPanel(screen);
    CreateStatusPanel(screen);
    CreateSettingsPanel(screen);
    ApplyThemeColors();
    ApplyMood(FaceMood::kNeutral);

    state_timer_ = lv_timer_create(StateTimerCallback, kStateTickMs, this);
    blink_timer_ = lv_timer_create(BlinkTimerCallback, kBlinkMinIntervalMs, this);
    subtitle_timer_ = lv_timer_create(SubtitleTimerCallback, kSubtitleLingerMs, this);
    lv_timer_pause(subtitle_timer_);
    thinking_timer_ = lv_timer_create(ThinkingTimerCallback, kThinkingTimeoutMs, this);
    lv_timer_pause(thinking_timer_);
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

void BuddyDisplay::HideStockEmoji() {
    for (auto obj : {emoji_box_, emoji_label_, emoji_image_}) {
        if (obj != nullptr) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void BuddyDisplay::CreateClearChatButton(lv_obj_t* screen) {
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    clear_chat_button_ = lv_button_create(screen);
    lv_obj_set_size(clear_chat_button_, kClearButtonSize, kClearButtonSize);
    lv_obj_align(clear_chat_button_, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_opa(clear_chat_button_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(clear_chat_button_, 0, 0);
    lv_obj_set_style_border_width(clear_chat_button_, 0, 0);
    lv_obj_set_style_pad_all(clear_chat_button_, 0, 0);
    lv_obj_move_foreground(clear_chat_button_);
    auto icon = lv_label_create(clear_chat_button_);
    lv_label_set_text(icon, MATERIAL_SYMBOLS_DELETE);
    lv_obj_set_style_text_font(icon, lvgl_theme->icon_font()->font(), 0);
    lv_obj_set_style_text_opa(icon, LV_OPA_50, 0);
    lv_obj_center(icon);
    lv_obj_add_event_cb(clear_chat_button_, ClearChatEventCallback, LV_EVENT_CLICKED, this);
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

lv_obj_t* BuddyDisplay::CreatePanel(lv_obj_t* screen, const char* title) {
    auto panel = lv_obj_create(screen);
    lv_obj_set_size(panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, kPanelPadding, 0);
    lv_obj_set_style_pad_row(panel, kPanelRowGap, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(panel);

    auto title_label = lv_label_create(panel);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(kAccentColor), 0);
    return panel;
}

void BuddyDisplay::CreateMenuPanel(lv_obj_t* screen) {
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    menu_panel_ = CreatePanel(screen, "Menu");
    for (int i = 0; i < kMenuItemCount; ++i) {
        auto item = lv_button_create(menu_panel_);
        lv_obj_set_size(item, LV_PCT(100), kMenuItemHeight);
        lv_obj_set_style_radius(item, kEyeRadius, 0);
        lv_obj_set_style_shadow_width(item, 0, 0);
        lv_obj_set_style_border_width(item, 2, 0);
        lv_obj_set_style_pad_all(item, kPanelPadding, 0);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(item, kPanelPadding, 0);
        auto icon = lv_label_create(item);
        lv_label_set_text(icon, kMenuIcons[i]);
        lv_obj_set_style_text_font(icon, lvgl_theme->icon_font()->font(), 0);
        auto label = lv_label_create(item);
        lv_label_set_text(label, kMenuLabels[i]);
        lv_obj_add_event_cb(item, MenuItemEventCallback, LV_EVENT_CLICKED, this);
        menu_items_[i] = item;
    }
}

void BuddyDisplay::CreateStatusPanel(lv_obj_t* screen) {
    status_panel_ = CreatePanel(screen, "Status");
    status_list_ = lv_label_create(status_panel_);
    lv_obj_set_width(status_list_, LV_PCT(100));
    lv_label_set_long_mode(status_list_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(status_list_, "");
}

void BuddyDisplay::CreateSettingsPanel(lv_obj_t* screen) {
    settings_panel_ = CreatePanel(screen, "Settings");

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
    sleep_dropdown_ = lv_dropdown_create(settings_panel_);
    lv_dropdown_set_options(sleep_dropdown_, kSleepOptions);
    lv_obj_set_width(sleep_dropdown_, LV_PCT(100));
    if (controls_.get_sleep_seconds) {
        lv_dropdown_set_selected(sleep_dropdown_, SleepOptionIndex(controls_.get_sleep_seconds()));
    }
    lv_obj_add_event_cb(sleep_dropdown_, SleepDropdownEventCallback, LV_EVENT_VALUE_CHANGED, this);

    auto hint = lv_label_create(settings_panel_);
    lv_label_set_text(hint, "Hold the main button to go back");
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_opa(hint, LV_OPA_60, 0);
}

void BuddyDisplay::ApplyThemeColors() {
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    if (lvgl_theme == nullptr || face_ == nullptr) {
        return;
    }
    for (auto part : {left_eye_, right_eye_}) {
        lv_obj_set_style_bg_color(part, lvgl_theme->text_color(), 0);
        lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_bg_color(mouth_, lv_color_hex(kAccentColor), 0);
    lv_obj_set_style_bg_opa(mouth_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(clear_chat_button_, lvgl_theme->text_color(), 0);
    HideStockEmoji();
    for (auto panel : {menu_panel_, status_panel_, settings_panel_}) {
        lv_obj_set_style_bg_color(panel, lvgl_theme->background_color(), 0);
        lv_obj_set_style_text_color(panel, lvgl_theme->text_color(), 0);
    }
    for (auto item : menu_items_) {
        lv_obj_set_style_bg_color(item, lvgl_theme->background_color(), 0);
        lv_obj_set_style_text_color(item, lvgl_theme->text_color(), 0);
        lv_obj_set_style_border_color(item, lvgl_theme->border_color(), 0);
    }
    HighlightMenuItem(menu_index_);
    lv_obj_set_style_bg_color(sleep_dropdown_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_text_color(sleep_dropdown_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_border_color(sleep_dropdown_, lvgl_theme->border_color(), 0);
    lv_obj_set_style_bg_color(dark_mode_switch_, lv_color_hex(kAccentColor),
                              static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | static_cast<lv_style_selector_t>(LV_STATE_CHECKED));
}

void BuddyDisplay::SetTheme(Theme* theme) {
    LcdDisplay::SetTheme(theme);
    DisplayLockGuard lock(this);
    ApplyThemeColors();
}

void BuddyDisplay::SetChatMessage(const char* role, const char* content) {
    bool clearing = content == nullptr || content[0] == '\0';
    if (clearing && subtitle_timer_ != nullptr) {
        DisplayLockGuard lock(this);
        lv_timer_reset(subtitle_timer_);
        lv_timer_resume(subtitle_timer_);
        return;
    }
    if (subtitle_timer_ != nullptr) {
        DisplayLockGuard lock(this);
        lv_timer_pause(subtitle_timer_);
    }
    LcdDisplay::SetChatMessage(role, content);
    if (chat_message_label_ != nullptr) {
        DisplayLockGuard lock(this);
        int32_t scroll_ms = static_cast<int32_t>(strlen(content)) * kSubtitleMsPerChar;
        lv_obj_set_style_anim_duration(chat_message_label_, scroll_ms > kSubtitleMinScrollMs ? scroll_ms : kSubtitleMinScrollMs, 0);
        lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }
}

void BuddyDisplay::SetPowerSaveMode(bool on) {
    sleeping_ = on;
    if (on) {
        DisplayLockGuard lock(this);
        ShowScreen(BuddyScreen::kChat);
    }
    LcdDisplay::SetPowerSaveMode(on);
}

void BuddyDisplay::StartThinkingEyes() {
    for (auto part : {left_eye_, right_eye_}) {
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, part);
        lv_anim_set_exec_cb(&anim, SetTranslateYAnimation);
        lv_anim_set_values(&anim, -kThinkingEyeDrift, kThinkingEyeDrift);
        lv_anim_set_duration(&anim, kThinkingCycleMs);
        lv_anim_set_reverse_duration(&anim, kThinkingCycleMs);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
        lv_anim_start(&anim);
    }
}

void BuddyDisplay::StopThinkingEyes() {
    for (auto part : {left_eye_, right_eye_}) {
        lv_anim_delete(part, SetTranslateYAnimation);
        lv_obj_set_style_translate_y(part, 0, 0);
    }
}

void BuddyDisplay::SetEyes(int width, int height, int offset_x, int offset_y) {
    lv_anim_delete(left_eye_, SetHeightAnimation);
    lv_anim_delete(right_eye_, SetHeightAnimation);
    StopThinkingEyes();
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
            StartThinkingEyes();
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
    if (thinking_ || emotion_ == "thinking") {
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

void BuddyDisplay::SetThinking(bool thinking) {
    thinking_ = thinking;
    if (thinking_timer_ == nullptr) {
        return;
    }
    if (thinking) {
        lv_timer_reset(thinking_timer_);
        lv_timer_resume(thinking_timer_);
    } else {
        lv_timer_pause(thinking_timer_);
    }
}

void BuddyDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    emotion_ = emotion != nullptr ? emotion : "neutral";
    SetThinking(emotion_ == "thinking");
    HideStockEmoji();
    if (face_ != nullptr) {
        auto mood = MoodFor(Application::GetInstance().GetDeviceState());
        if (mood != mood_) {
            ApplyMood(mood);
        }
    }
}

void BuddyDisplay::ShowScreen(BuddyScreen screen) {
    screen_ = screen;
    lv_obj_add_flag(menu_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(settings_panel_, LV_OBJ_FLAG_HIDDEN);
    switch (screen) {
        case BuddyScreen::kMenu:
            lv_obj_remove_flag(menu_panel_, LV_OBJ_FLAG_HIDDEN);
            break;
        case BuddyScreen::kStatus:
            RefreshStatus();
            lv_obj_scroll_to_y(status_panel_, 0, LV_ANIM_OFF);
            lv_obj_remove_flag(status_panel_, LV_OBJ_FLAG_HIDDEN);
            break;
        case BuddyScreen::kSettings:
            lv_obj_scroll_to_y(settings_panel_, 0, LV_ANIM_OFF);
            lv_obj_remove_flag(settings_panel_, LV_OBJ_FLAG_HIDDEN);
            break;
        case BuddyScreen::kChat:
        default:
            break;
    }
}

void BuddyDisplay::HighlightMenuItem(int index) {
    menu_index_ = index;
    for (int i = 0; i < kMenuItemCount; ++i) {
        if (menu_items_[i] == nullptr) {
            continue;
        }
        lv_obj_set_style_border_width(menu_items_[i], i == index ? 3 : 1, 0);
        lv_obj_set_style_border_color(menu_items_[i], i == index ? lv_color_hex(kAccentColor)
                                                                  : static_cast<LvglTheme*>(current_theme_)->border_color(), 0);
    }
}

void BuddyDisplay::OpenMenuItem(int index) {
    if (index < 0 || index >= kMenuItemCount) {
        return;
    }
    HighlightMenuItem(index);
    ShowScreen(kMenuTargets[index]);
}

void BuddyDisplay::RefreshStatus() {
    if (!controls_.get_status_lines || status_list_ == nullptr) {
        return;
    }
    std::string text;
    for (const auto& [name, value] : controls_.get_status_lines()) {
        text += name;
        text += ": ";
        text += value;
        text += "\n";
    }
    lv_label_set_text(status_list_, text.c_str());
}

bool BuddyDisplay::OnMainButtonClick() {
    DisplayLockGuard lock(this);
    switch (screen_) {
        case BuddyScreen::kMenu:
            OpenMenuItem(menu_index_);
            return true;
        case BuddyScreen::kStatus:
        case BuddyScreen::kSettings:
            ShowScreen(BuddyScreen::kMenu);
            return true;
        case BuddyScreen::kChat:
        default:
            return true;
    }
}

void BuddyDisplay::OnMainButtonLongPress() {
    DisplayLockGuard lock(this);
    if (screen_ == BuddyScreen::kChat) {
        if (BeginListening()) {
            button_listening_ = true;
        }
    } else {
        ShowScreen(BuddyScreen::kChat);
    }
}

void BuddyDisplay::OpenMenu() {
    DisplayLockGuard lock(this);
    if (sleeping_ || button_listening_) {
        return;
    }
    HighlightMenuItem(0);
    ShowScreen(BuddyScreen::kMenu);
}

void BuddyDisplay::OnMainButtonReleased() {
    DisplayLockGuard lock(this);
    if (!button_listening_) {
        return;
    }
    button_listening_ = false;
    EndListening();
}

bool BuddyDisplay::BeginListening() {
    if (sleeping_ || screen_ != BuddyScreen::kChat || thinking_) {
        return false;
    }
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state != kDeviceStateIdle && state != kDeviceStateSpeaking && state != kDeviceStateListening) {
        return false;
    }
    lv_obj_set_style_border_width(talk_surface_, kTalkBorderWidth, 0);
    stop_listening_pending_ = false;
    if (state != kDeviceStateListening) {
        app.StartListening();
    }
    return true;
}

void BuddyDisplay::EndListening() {
    lv_obj_set_style_border_width(talk_surface_, 0, 0);
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateListening) {
        app.StopListening();
    } else if (state == kDeviceStateConnecting) {
        stop_listening_pending_ = true;
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
        if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
            SetThinking(false);
        }
        auto mood = MoodFor(state);
        if (mood != mood_) {
            ApplyMood(mood);
        }
    }
    if (screen_ == BuddyScreen::kStatus && ++status_ticks_ >= kStatusRefreshTicks) {
        status_ticks_ = 0;
        RefreshStatus();
    }
}

void BuddyDisplay::OnTalkPressed() {
    if (sleeping_) {
        if (controls_.wake_up) {
            controls_.wake_up();
        }
        return;
    }
    BeginListening();
}

void BuddyDisplay::OnTalkReleased() {
    if (sleeping_) {
        return;
    }
    EndListening();
}

void BuddyDisplay::OnDarkModeChanged() {
    bool dark = lv_obj_has_state(dark_mode_switch_, LV_STATE_CHECKED);
    auto theme = LvglThemeManager::GetInstance().GetTheme(dark ? kDarkThemeName : kLightThemeName);
    if (theme != nullptr) {
        SetTheme(theme);
    }
}

void BuddyDisplay::OnSleepOptionChanged() {
    auto index = lv_dropdown_get_selected(sleep_dropdown_);
    if (index < static_cast<uint32_t>(kSleepOptionCount) && controls_.set_sleep_seconds) {
        controls_.set_sleep_seconds(kSleepOptionSeconds[index]);
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

void BuddyDisplay::SubtitleTimerCallback(lv_timer_t* timer) {
    auto display = static_cast<BuddyDisplay*>(lv_timer_get_user_data(timer));
    lv_timer_pause(timer);
    display->LcdDisplay::SetChatMessage("system", "");
}

void BuddyDisplay::ThinkingTimerCallback(lv_timer_t* timer) {
    auto display = static_cast<BuddyDisplay*>(lv_timer_get_user_data(timer));
    ESP_LOGW(TAG, "Thinking indicator timed out");
    display->SetThinking(false);
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

void BuddyDisplay::MenuItemEventCallback(lv_event_t* event) {
    auto display = static_cast<BuddyDisplay*>(lv_event_get_user_data(event));
    auto target = static_cast<lv_obj_t*>(lv_event_get_target(event));
    for (int i = 0; i < kMenuItemCount; ++i) {
        if (display->menu_items_[i] == target) {
            display->OpenMenuItem(i);
            return;
        }
    }
}

void BuddyDisplay::DarkModeEventCallback(lv_event_t* event) {
    static_cast<BuddyDisplay*>(lv_event_get_user_data(event))->OnDarkModeChanged();
}

void BuddyDisplay::SleepDropdownEventCallback(lv_event_t* event) {
    static_cast<BuddyDisplay*>(lv_event_get_user_data(event))->OnSleepOptionChanged();
}

void BuddyDisplay::ClearChatEventCallback(lv_event_t* event) {
    auto display = static_cast<BuddyDisplay*>(lv_event_get_user_data(event));
    if (display->controls_.reset_conversation && !display->sleeping_) {
        display->controls_.reset_conversation();
    }
}
