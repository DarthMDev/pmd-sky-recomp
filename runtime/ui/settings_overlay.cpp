#include "settings_overlay.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

namespace skyrecomp::ui {
namespace {

constexpr const char* kSectionVideo = "Video";
constexpr const char* kSectionController = "Controller";
constexpr const char* kSectionKeyboard = "Keyboard";
constexpr const char* kSectionSystem = "System";

constexpr const char* kKeyAspectRatio = "sky.aspect_ratio";
constexpr const char* kKeyHudLayout = "sky.hud_layout";
constexpr const char* kKeyRefreshRate = "sky.refresh_rate";
constexpr const char* kKeyShaderFilter = "sky.shader_filter";
constexpr const char* kKeyWeatherBloom = "sky.weather_bloom";
constexpr const char* kKeyAudioVolume = "sky.audio_volume";
constexpr const char* kKeyControllerPreset = "sky.controller_preset";
constexpr const char* kKeyControllerPresetPage = "sky.controller_preset_page";
constexpr const char* kKeyResetController = "sky.pad.reset";
constexpr const char* kKeyResetKeyboard = "sky.key.reset";
constexpr const char* kKeyResume = RECOMP_RUNTIME_UI_KEY_RESUME;

constexpr const char* const kAspectChoices[] = {
    "4:3 (256x192)",
    "16:10 (308x192)",
    "16:9 (342x192)",
    "21:9 (448x192)",
};

constexpr const char* const kHudLayoutChoices[] = {
    "Dual-Screen (Default)",
    "Hybrid",
};

constexpr const char* const kRefreshChoices[] = {
    "60Hz (Vanilla)",
    "120Hz (ProMotion)",
    "144Hz",
    "240Hz",
    "Uncapped VRR",
};

constexpr const char* const kShaderChoices[] = {
    "Nearest",
    "Sharp Bilinear",
    "Pixellate",
    "NDS LCD Grid",
    "CRT Scanline",
};

constexpr const char* const kControllerChoices[] = {
    "Standard DS",
    "Modern Dungeon",
    "Swapped B/A",
    "Custom",
};

// Gamepad codes are SDL controller-button enum values + 1. Keyboard codes are
// SDL scancodes. The lists are stable choice values so the saved file never
// depends on choice ordering.
constexpr int kPadChoiceValues[] = {
    0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
    100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111};
constexpr const char* const kPadChoices[] = {
    "Unbound", "A", "B", "X", "Y", "Back/Select", "Start", "L Stick",
    "R Stick", "L", "R", "Dpad Up", "Dpad Down", "Dpad Left",
    "Dpad Right", "Misc 1", "Misc 2", "Misc 3", "Misc 4", "Misc 5",
    "Misc 6", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad",
    "Left Stick Right", "Left Stick Left", "Left Stick Down",
    "Left Stick Up", "Right Stick Right", "Right Stick Left",
    "Right Stick Down", "Right Stick Up", "LT Pressed", "LT Released",
    "RT Pressed", "RT Released"};

constexpr int kKeyChoiceValues[] = {
    0,
    4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 26, 27, 28, 29,
    30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
    40, 42, 43, 44, 45, 46, 47, 48, 49, 51, 52, 53, 54, 55, 56,
    59, 60, 61,
    79, 80, 81, 82,
    224, 225, 226, 227, 228, 229, 230, 231};
constexpr uint16_t kDsBits[kBindingCount] = {
    1u << 6, 1u << 7, 1u << 5, 1u << 4,
    1u << 0, 1u << 1, 1u << 10, 1u << 11,
    1u << 9, 1u << 8, 1u << 3, 1u << 2};

constexpr const char* const kKeyChoices[] = {
    "Unbound",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
    "Enter", "Backspace", "Tab", "Space", "-", "=", "[", "]",
    "\\", ";", "'", "`", ",", ".", "/",
    "F2", "F3", "F4",
    "Right", "Left", "Down", "Up",
    "LCtrl", "LShift", "LAlt", "LGui", "RCtrl", "RShift", "RAlt", "RGui"};

constexpr std::array<const char*, kBindingCount> kBindingActionNames = {
    "Up", "Down", "Left", "Right", "A", "B", "X", "Y", "L", "R",
    "Start", "Select"};
constexpr std::array<const char*, kBindingCount> kBindingKeySuffixes = {
    "up", "down", "left", "right", "a", "b", "x", "y", "l", "r",
    "start", "select"};
constexpr std::array<const char*, kBindingCount> kPadBindingKeys = {
    "sky.pad.up", "sky.pad.down", "sky.pad.left", "sky.pad.right",
    "sky.pad.a", "sky.pad.b", "sky.pad.x", "sky.pad.y", "sky.pad.l",
    "sky.pad.r", "sky.pad.start", "sky.pad.select"};
constexpr std::array<const char*, kBindingCount> kKeyBindingKeys = {
    "sky.key.up", "sky.key.down", "sky.key.left", "sky.key.right",
    "sky.key.a", "sky.key.b", "sky.key.x", "sky.key.y", "sky.key.l",
    "sky.key.r", "sky.key.start", "sky.key.select"};

constexpr RecompRuntimeUiItem MakeBindingItem(const char* key,
                                              const char* section,
                                              const char* label,
                                              const char* description) {
    return RecompRuntimeUiItem{
        .key = key,
        .section = section,
        .label = label,
        .description = description,
        .type = RECOMP_RUNTIME_UI_ACTION,
        .minimum = 0,
        .maximum = 0,
        .step = 0,
        .choices = nullptr,
        .choice_count = 0,
        .choice_values = nullptr,
    };
}

constexpr RecompRuntimeUiItem kMenuItems[] = {
    {
        .key = kKeyAspectRatio,
        .section = kSectionVideo,
        .label = "Aspect Ratio",
        .description = "True 2D widescreen width. Applies live in Hybrid mode.",
        .type = RECOMP_RUNTIME_UI_CHOICE,
        .minimum = 0,
        .maximum = 3,
        .step = 1,
        .choices = kAspectChoices,
        .choice_count = std::size(kAspectChoices),
        .choice_values = nullptr,
    },
    {
        .key = kKeyHudLayout,
        .section = kSectionVideo,
        .label = "HUD Layout",
        .description = "Stacked top/bottom (default), or a single-window hybrid with one main screen and one small inset.",
        .type = RECOMP_RUNTIME_UI_CHOICE,
        .minimum = 0,
        .maximum = 1,
        .step = 1,
        .choices = kHudLayoutChoices,
        .choice_count = std::size(kHudLayoutChoices),
        .choice_values = nullptr,
    },
    {
        .key = kKeyRefreshRate,
        .section = kSectionVideo,
        .label = "Refresh Interpolation",
        .description = "Sub-pixel camera & entity interpolation between 60Hz logic ticks.",
        .type = RECOMP_RUNTIME_UI_CHOICE,
        .minimum = 0,
        .maximum = 4,
        .step = 1,
        .choices = kRefreshChoices,
        .choice_count = std::size(kRefreshChoices),
        .choice_values = nullptr,
    },
    {
        .key = kKeyShaderFilter,
        .section = kSectionVideo,
        .label = "HD Shader Filter",
        .description = "Nearest, bilinear, pixellate, LCD grid, or CRT scanline presentation.",
        .type = RECOMP_RUNTIME_UI_CHOICE,
        .minimum = 0,
        .maximum = 4,
        .step = 1,
        .choices = kShaderChoices,
        .choice_count = std::size(kShaderChoices),
        .choice_values = nullptr,
    },
    {
        .key = kKeyWeatherBloom,
        .section = kSectionVideo,
        .label = "Weather Ambient Bloom",
        .description = "Per-weather dungeon bloom & LUT grading driven by GetWeatherColorTable.",
        .type = RECOMP_RUNTIME_UI_BOOL,
        .minimum = 0,
        .maximum = 1,
        .step = 1,
        .choices = nullptr,
        .choice_count = 0,
        .choice_values = nullptr,
    },
    {
        .key = kKeyAudioVolume,
        .section = kSectionVideo,
        .label = "Audio Volume",
        .description = "Master audio output volume percentage (0% - 100%).",
        .type = RECOMP_RUNTIME_UI_INT,
        .minimum = 0,
        .maximum = 100,
        .step = 10,
        .choices = nullptr,
        .choice_count = 0,
        .choice_values = nullptr,
    },
    {
        .key = kKeyControllerPresetPage,
        .section = kSectionController,
        .label = "Preset",
        .description = "Standard: D-pad moves; South=A, East=B, West=X, North=Y, LB=L, RB=R. Modern Dungeon: LB=Y and North=L. Swapped B/A: South=B and East=A. Custom: edit rows below.",
        .type = RECOMP_RUNTIME_UI_CHOICE,
        .minimum = 0,
        .maximum = 3,
        .step = 1,
        .choices = kControllerChoices,
        .choice_count = std::size(kControllerChoices),
        .choice_values = nullptr,
    },
    {
        .key = kKeyResetController,
        .section = kSectionController,
        .label = "Restore controller defaults",
        .description = "Reset all controller assignments to the standard layout.",
        .type = RECOMP_RUNTIME_UI_ACTION,
        .minimum = 0,
        .maximum = 0,
        .step = 0,
        .choices = nullptr,
        .choice_count = 0,
        .choice_values = nullptr,
    },
    MakeBindingItem(kPadBindingKeys[0], kSectionController,
                    kBindingActionNames[0], "Press Enter, then press a controller button or move a stick. Backspace clears; Guide or Escape cancels."),
    MakeBindingItem(kPadBindingKeys[1], kSectionController,
                    kBindingActionNames[1], "Press Enter, then press a controller button or move a stick. Backspace clears; Guide or Escape cancels."),
    MakeBindingItem(kPadBindingKeys[2], kSectionController,
                    kBindingActionNames[2], "Press Enter, then press a controller button or move a stick. Backspace clears; Guide or Escape cancels."),
    MakeBindingItem(kPadBindingKeys[3], kSectionController,
                    kBindingActionNames[3], "Press Enter, then press a controller button or move a stick. Backspace clears; Guide or Escape cancels."),
    MakeBindingItem(kPadBindingKeys[4], kSectionController,
                    kBindingActionNames[4], "Press Enter, then press a controller button or move a stick. Backspace clears; Guide or Escape cancels."),
    MakeBindingItem(kPadBindingKeys[5], kSectionController,
                    kBindingActionNames[5], "Press Enter, then press a controller button or move a stick. Backspace clears; Guide or Escape cancels."),
    MakeBindingItem(kPadBindingKeys[6], kSectionController,
                    kBindingActionNames[6], "Press Enter, then press a controller button or move a stick. Backspace clears; Guide or Escape cancels."),
    MakeBindingItem(kPadBindingKeys[7], kSectionController,
                    kBindingActionNames[7], "Press Enter, then press a controller button or move a stick. Backspace clears; Guide or Escape cancels."),
    MakeBindingItem(kPadBindingKeys[8], kSectionController,
                    kBindingActionNames[8], "Press Enter, then press a controller button or move a stick. Backspace clears; Guide or Escape cancels."),
    MakeBindingItem(kPadBindingKeys[9], kSectionController,
                    kBindingActionNames[9], "Press Enter, then press a controller button or move a stick. Backspace clears; Guide or Escape cancels."),
    MakeBindingItem(kPadBindingKeys[10], kSectionController,
                    kBindingActionNames[10], "Press Enter, then press a controller button or move a stick. Backspace clears; Guide or Escape cancels."),
    MakeBindingItem(kPadBindingKeys[11], kSectionController,
                    kBindingActionNames[11], "Press Enter, then press a controller button or move a stick. Backspace clears; Guide or Escape cancels."),
    MakeBindingItem(kKeyBindingKeys[0], kSectionKeyboard,
                    kBindingActionNames[0], "Press Enter, then press the key to assign. Backspace clears the binding; Escape cancels."),
    MakeBindingItem(kKeyBindingKeys[1], kSectionKeyboard,
                    kBindingActionNames[1], "Press Enter, then press the key to assign. Backspace clears the binding; Escape cancels."),
    MakeBindingItem(kKeyBindingKeys[2], kSectionKeyboard,
                    kBindingActionNames[2], "Press Enter, then press the key to assign. Backspace clears the binding; Escape cancels."),
    MakeBindingItem(kKeyBindingKeys[3], kSectionKeyboard,
                    kBindingActionNames[3], "Press Enter, then press the key to assign. Backspace clears the binding; Escape cancels."),
    MakeBindingItem(kKeyBindingKeys[4], kSectionKeyboard,
                    kBindingActionNames[4], "Press Enter, then press the key to assign. Backspace clears the binding; Escape cancels."),
    MakeBindingItem(kKeyBindingKeys[5], kSectionKeyboard,
                    kBindingActionNames[5], "Press Enter, then press the key to assign. Backspace clears the binding; Escape cancels."),
    MakeBindingItem(kKeyBindingKeys[6], kSectionKeyboard,
                    kBindingActionNames[6], "Press Enter, then press the key to assign. Backspace clears the binding; Escape cancels."),
    MakeBindingItem(kKeyBindingKeys[7], kSectionKeyboard,
                    kBindingActionNames[7], "Press Enter, then press the key to assign. Backspace clears the binding; Escape cancels."),
    MakeBindingItem(kKeyBindingKeys[8], kSectionKeyboard,
                    kBindingActionNames[8], "Press Enter, then press the key to assign. Backspace clears the binding; Escape cancels."),
    MakeBindingItem(kKeyBindingKeys[9], kSectionKeyboard,
                    kBindingActionNames[9], "Press Enter, then press the key to assign. Backspace clears the binding; Escape cancels."),
    MakeBindingItem(kKeyBindingKeys[10], kSectionKeyboard,
                    kBindingActionNames[10], "Press Enter, then press the key to assign. Backspace clears the binding; Escape cancels."),
    MakeBindingItem(kKeyBindingKeys[11], kSectionKeyboard,
                    kBindingActionNames[11], "Press Enter, then press the key to assign. Backspace clears the binding; Escape cancels."),
    {
        .key = kKeyResetKeyboard,
        .section = kSectionKeyboard,
        .label = "Restore keyboard defaults",
        .description = "Reset all keyboard assignments to the standard layout.",
        .type = RECOMP_RUNTIME_UI_ACTION,
        .minimum = 0,
        .maximum = 0,
        .step = 0,
        .choices = nullptr,
        .choice_count = 0,
        .choice_values = nullptr,
    },
    {
        .key = kKeyResume,
        .section = kSectionSystem,
        .label = "Resume Explorers of Sky",
        .description = "Close the settings overlay and return to the game.",
        .type = RECOMP_RUNTIME_UI_ACTION,
        .minimum = 0,
        .maximum = 0,
        .step = 0,
        .choices = nullptr,
        .choice_count = 0,
        .choice_values = nullptr,
    },
};

/// Trims leading and trailing ASCII whitespace from a string view.
[[nodiscard]] std::string TrimWhitespace(std::string_view sv) {
    size_t start = 0;
    while (start < sv.size() && (sv[start] == ' ' || sv[start] == '\t' || sv[start] == '\r')) {
        ++start;
    }
    size_t end = sv.size();
    while (end > start && (sv[end - 1] == ' ' || sv[end - 1] == '\t' || sv[end - 1] == '\r')) {
        --end;
    }
    return std::string(sv.substr(start, end - start));
}

int BindingSlotForKey(std::string_view key, bool* controller) noexcept {
    if (controller == nullptr) return -1;
    std::string_view suffix;
    if (key.rfind("sky.pad.", 0) == 0) {
        *controller = true;
        suffix = key.substr(8);
    } else if (key.rfind("sky.key.", 0) == 0) {
        *controller = false;
        suffix = key.substr(8);
    } else {
        return -1;
    }
    for (size_t i = 0; i < kBindingKeySuffixes.size(); ++i) {
        if (suffix == kBindingKeySuffixes[i]) return static_cast<int>(i);
    }
    return -1;
}

bool IsValidBindingChoice(int value, bool controller) noexcept {
    if (!controller) return value >= 0 && value < 512;
    const int* values = controller ? kPadChoiceValues : kKeyChoiceValues;
    const size_t count = controller ? std::size(kPadChoiceValues)
                                    : std::size(kKeyChoiceValues);
    for (size_t i = 0; i < count; ++i) {
        if (values[i] == value) return true;
    }
    return false;
}

std::string BindingDisplayName(int value, bool controller) {
    const int* values = controller ? kPadChoiceValues : kKeyChoiceValues;
    const char* const* choices = controller ? kPadChoices : kKeyChoices;
    const size_t count = controller ? std::size(kPadChoiceValues)
                                    : std::size(kKeyChoiceValues);
    for (size_t i = 0; i < count; ++i) {
        if (values[i] == value) return choices[i];
    }
    if (controller && value == 6) return "Guide (reserved)";
    if (!controller && value == 41) return "Escape (reserved)";
    if (!controller && value == 58) return "F1 (reserved)";
    if (value == 0) return "Unbound";
    return std::string(controller ? "Input " : "Key ") +
           std::to_string(value);
}

bool ParseBindings(std::string_view text, BindingArray& out,
                   bool controller) noexcept {
    BindingArray parsed{};
    size_t index = 0;
    size_t start = 0;
    while (start <= text.size() && index < parsed.size()) {
        size_t comma = text.find(',', start);
        if (comma == std::string_view::npos) comma = text.size();
        const std::string token = TrimWhitespace(text.substr(start, comma - start));
        parsed[index] = std::atoi(token.c_str());
        if (!IsValidBindingChoice(parsed[index], controller)) return false;
        ++index;
        if (comma == text.size()) break;
        start = comma + 1;
    }
    if (index != parsed.size()) return false;
    out = parsed;
    return true;
}

void ApplyControllerPreset(ControllerRemapPreset preset,
                           BindingArray& bindings) noexcept {
    switch (preset) {
        case ControllerRemapPreset::ModernDungeon:
            bindings = {12, 13, 14, 15, 1, 2, 3, 10, 4, 11, 7, 5};
            break;
        case ControllerRemapPreset::SwappedConfirmCancel:
            bindings = {12, 13, 14, 15, 2, 1, 3, 4, 10, 11, 7, 5};
            break;
        case ControllerRemapPreset::StandardNintendoDs:
            bindings = kDefaultControllerBindings;
            break;
        case ControllerRemapPreset::Custom:
        default:
            break;
    }
}

} // namespace

SkySettingsOverlay::SkySettingsOverlay(const SkySettingsState& initial_state) noexcept
    : state_(initial_state) {
    menu_items_.assign(std::begin(kMenuItems), std::end(kMenuItems));
    RefreshBindingLabels();
    RecompRuntimeUiConfig cfg{};
    cfg.title = "SkyRecompiled HD Settings";
    cfg.subtitle = "PMD: Explorers of Sky -- Widescreen / High-Refresh / Metal Shaders";
    cfg.items = menu_items_.data();
    cfg.item_count = menu_items_.size();
    cfg.callbacks = {
        .context = this,
        .get_value = &SkySettingsOverlay::CallbackGetValue,
        .set_value = &SkySettingsOverlay::CallbackSetValue,
        .run_action = &SkySettingsOverlay::CallbackRunAction,
        .is_enabled = &SkySettingsOverlay::CallbackIsEnabled,
        .save = &SkySettingsOverlay::CallbackSave,
        .visibility_changed = nullptr,
        .get_text = nullptr,
        .set_text = nullptr,
    };
    cfg.theme = "gba";
    cfg.accept_label = "Select";
    cfg.back_label = "Close (F1 / Guide)";

    ui_ = recomp_runtime_ui_create(&cfg);
    if (ui_ != nullptr) {
        recomp_runtime_ui_set_backdrop(ui_, RECOMP_RUNTIME_UI_DEFAULT_DIM, 0.94f);
    }
}

SkySettingsOverlay::~SkySettingsOverlay() noexcept {
    if (ui_ != nullptr) {
        recomp_runtime_ui_destroy(ui_);
        ui_ = nullptr;
    }
}

int SkySettingsOverlay::CallbackGetValue(
    void* context,
    const RecompRuntimeUiItem* item,
    int* value_out) noexcept {
    if (context == nullptr || item == nullptr || item->key == nullptr || value_out == nullptr) {
        return 0;
    }
    const auto* self = static_cast<const SkySettingsOverlay*>(context);
    const std::string_view key(item->key);

    if (key == kKeyAspectRatio) {
        *value_out = static_cast<int>(self->state_.aspect_ratio);
        return 1;
    }
    if (key == kKeyHudLayout) {
        *value_out = static_cast<int>(self->state_.hud_layout);
        return 1;
    }
    if (key == kKeyRefreshRate) {
        *value_out = static_cast<int>(self->state_.refresh_rate);
        return 1;
    }
    if (key == kKeyShaderFilter) {
        *value_out = static_cast<int>(self->state_.shader_filter);
        return 1;
    }
    if (key == kKeyWeatherBloom) {
        *value_out = self->state_.weather_ambient_bloom ? 1 : 0;
        return 1;
    }
    if (key == kKeyAudioVolume) {
        *value_out = self->state_.audio_volume_percent;
        return 1;
    }
    if (key == kKeyControllerPresetPage || key == kKeyControllerPreset) {
        *value_out = static_cast<int>(self->state_.controller_preset);
        return 1;
    }
    bool controller = false;
    const int binding_slot = BindingSlotForKey(key, &controller);
    if (binding_slot >= 0) {
        *value_out = controller
            ? self->state_.controller_bindings[static_cast<size_t>(binding_slot)]
            : self->state_.keyboard_bindings[static_cast<size_t>(binding_slot)];
        return 1;
    }
    return 0;
}

int SkySettingsOverlay::CallbackSetValue(
    void* context,
    const RecompRuntimeUiItem* item,
    int value) noexcept {
    if (context == nullptr || item == nullptr || item->key == nullptr) {
        return 0;
    }
    auto* self = static_cast<SkySettingsOverlay*>(context);
    const std::string_view key(item->key);

    bool changed = false;
    if (key == kKeyAspectRatio) {
        self->state_.aspect_ratio = static_cast<gpu2d::AspectRatioPreset>(std::clamp(value, 0, 3));
        changed = true;
    } else if (key == kKeyHudLayout) {
        self->state_.hud_layout = static_cast<gpu2d::HudLayoutMode>(std::clamp(value, 0, 1));
        changed = true;
    } else if (key == kKeyRefreshRate) {
        self->state_.refresh_rate = static_cast<camera::RefreshRateMode>(std::clamp(value, 0, 4));
        changed = true;
    } else if (key == kKeyShaderFilter) {
        self->state_.shader_filter = static_cast<shaders::ShaderFilterPreset>(std::clamp(value, 0, 4));
        changed = true;
    } else if (key == kKeyWeatherBloom) {
        self->state_.weather_ambient_bloom = (value != 0);
        changed = true;
    } else if (key == kKeyAudioVolume) {
        self->state_.audio_volume_percent = std::clamp(value, 0, 100);
        self->state_.audio_muted = (self->state_.audio_volume_percent == 0);
        changed = true;
    } else if (key == kKeyControllerPresetPage ||
               key == kKeyControllerPreset) {
        self->state_.controller_preset =
            static_cast<ControllerRemapPreset>(std::clamp(value, 0, 3));
        if (self->state_.controller_preset != ControllerRemapPreset::Custom) {
            ApplyControllerPreset(self->state_.controller_preset,
                                  self->state_.controller_bindings);
        }
        changed = true;
    } else {
        bool controller = false;
        const int binding_slot = BindingSlotForKey(key, &controller);
        if (binding_slot >= 0 && IsValidBindingChoice(value, controller)) {
            if (controller) {
                self->state_.controller_preset = ControllerRemapPreset::Custom;
            }
            auto& bindings = controller ? self->state_.controller_bindings
                                        : self->state_.keyboard_bindings;
            bindings[static_cast<size_t>(binding_slot)] = value;
            changed = true;
        }
    }

    if (changed) {
        self->RefreshBindingLabels();
        self->NotifyBoundConsumers();
        return 1;
    }
    return 0;
}

int SkySettingsOverlay::CallbackRunAction(
    void* context,
    const RecompRuntimeUiItem* item) noexcept {
    if (context == nullptr || item == nullptr || item->key == nullptr) {
        return 0;
    }
    auto* self = static_cast<SkySettingsOverlay*>(context);
    const std::string_view key(item->key);
    if (key == kKeyResume && self->ui_ != nullptr) {
        recomp_runtime_ui_close(self->ui_);
        return 1;
    }
    if (key == kKeyResetController) {
        self->state_.controller_preset =
            ControllerRemapPreset::StandardNintendoDs;
        self->state_.controller_bindings = kDefaultControllerBindings;
        self->RefreshBindingLabels();
        self->NotifyBoundConsumers();
        CallbackSave(self);
        return 1;
    }
    if (key == kKeyResetKeyboard) {
        self->state_.keyboard_bindings = kDefaultKeyboardBindings;
        self->RefreshBindingLabels();
        self->NotifyBoundConsumers();
        CallbackSave(self);
        return 1;
    }
    bool controller = false;
    const int slot = BindingSlotForKey(key, &controller);
    if (slot >= 0) {
        self->capture_binding_slot_ = slot;
        self->capture_controller_binding_ = controller;
        if (self->ui_ != nullptr) {
            recomp_runtime_ui_set_toast(
                self->ui_, controller ? "Waiting for controller input"
                                      : "Waiting for keyboard input",
                controller ? "Press a button or move a stick. Guide cancels."
                           : "Press a key. Backspace clears; Escape cancels.");
        }
        return 1;
    }
    return 0;
}

int SkySettingsOverlay::CallbackIsEnabled(
    void* /*context*/,
    const RecompRuntimeUiItem* /*item*/) noexcept {
    return 1;
}

void SkySettingsOverlay::CallbackSave(void* context) noexcept {
    if (context == nullptr) {
        return;
    }
    const auto* self = static_cast<const SkySettingsOverlay*>(context);
    if (!self->bindings_.config_file_path.empty()) {
        (void)self->SaveToFile(self->bindings_.config_file_path);
    }
}

void SkySettingsOverlay::NotifyBoundConsumers() noexcept {
    if (bindings_.ppu != nullptr &&
        bindings_.camera_hook != nullptr &&
        bindings_.interpolator != nullptr &&
        bindings_.shader_pipeline != nullptr) {
        ApplyToSubsystems(
            *bindings_.ppu,
            *bindings_.camera_hook,
            *bindings_.interpolator,
            *bindings_.shader_pipeline);
    } else {
        if (bindings_.ppu != nullptr) {
            bindings_.ppu->SetAspectRatio(state_.aspect_ratio);
            bindings_.ppu->SetHudLayoutMode(state_.hud_layout);
        }
        if (bindings_.camera_hook != nullptr) {
            bindings_.camera_hook->SetAspectRatio(state_.aspect_ratio);
        }
        if (bindings_.interpolator != nullptr) {
            bindings_.interpolator->SetRefreshRateMode(state_.refresh_rate);
        }
        if (bindings_.shader_pipeline != nullptr) {
            bindings_.shader_pipeline->SetFilterPreset(state_.shader_filter);
            bindings_.shader_pipeline->SetWeatherBloomEnabled(state_.weather_ambient_bloom);
        }
    }

    if (bindings_.on_audio_settings_changed) {
        bindings_.on_audio_settings_changed(GetEffectiveAudioVolumePercent(), state_.audio_muted);
    }
    if (bindings_.on_controller_mapping_changed) {
        bindings_.on_controller_mapping_changed(GetActiveButtonMapping());
    }
    if (bindings_.on_controller_bindings_changed) {
        bindings_.on_controller_bindings_changed(state_.controller_bindings);
    }
    if (bindings_.on_keyboard_bindings_changed) {
        bindings_.on_keyboard_bindings_changed(state_.keyboard_bindings);
    }
}

void SkySettingsOverlay::BindRuntimeConsumers(RuntimeConsumerBindings bindings) noexcept {
    bindings_ = std::move(bindings);
    NotifyBoundConsumers();
}

bool SkySettingsOverlay::IsVisible() const noexcept {
    return ui_ != nullptr && recomp_runtime_ui_is_open(ui_) != 0;
}

void SkySettingsOverlay::SetVisible(bool visible) noexcept {
    if (ui_ == nullptr) {
        return;
    }
    if (visible) {
        recomp_runtime_ui_open(ui_);
    } else {
        recomp_runtime_ui_close(ui_);
    }
}

void SkySettingsOverlay::SetState(const SkySettingsState& state) noexcept {
    state_ = state;
    state_.audio_volume_percent = std::clamp(state_.audio_volume_percent, 0, 100);
    RefreshBindingLabels();
    NotifyBoundConsumers();
}

size_t SkySettingsOverlay::GetSelectedIndex() const noexcept {
    if (ui_ == nullptr) {
        return 0;
    }
    return recomp_runtime_ui_view_selected_row(ui_);
}

bool SkySettingsOverlay::CaptureKeyboardBinding(int scancode) noexcept {
    if (capture_binding_slot_ < 0) return false;
    if (capture_controller_binding_) {
        if (scancode == 41) FinishBindingCapture(0, true);
        else if (scancode == 42) FinishBindingCapture(0, false);
        return true;
    }
    if (scancode == 41 || scancode == 58) { // Escape or reserved F1.
        FinishBindingCapture(0, true);
    } else if (scancode == 42) { // Backspace clears the selected binding.
        FinishBindingCapture(0, false);
    } else if (scancode > 0 && scancode < 512) {
        FinishBindingCapture(scancode, false);
    }
    return true;
}

bool SkySettingsOverlay::CaptureControllerBinding(int binding_code) noexcept {
    if (capture_binding_slot_ < 0 || !capture_controller_binding_) return false;
    if (binding_code == 6) { // Guide is reserved for opening/closing settings.
        FinishBindingCapture(0, true);
    } else if (IsValidBindingChoice(binding_code, true)) {
        FinishBindingCapture(binding_code, false);
    }
    return true;
}

void SkySettingsOverlay::FinishBindingCapture(int value, bool cancel) noexcept {
    const int slot = capture_binding_slot_;
    const bool controller = capture_controller_binding_;
    capture_binding_slot_ = -1;
    capture_controller_binding_ = false;
    if (!cancel && slot >= 0 && static_cast<size_t>(slot) < kBindingCount) {
        if (controller) {
            state_.controller_preset = ControllerRemapPreset::Custom;
            state_.controller_bindings[static_cast<size_t>(slot)] = value;
        } else {
            state_.keyboard_bindings[static_cast<size_t>(slot)] = value;
        }
        NotifyBoundConsumers();
        CallbackSave(this);
    }
    RefreshBindingLabels();
    if (ui_ != nullptr) {
        recomp_runtime_ui_set_toast(
            ui_, cancel ? "Binding cancelled" : "Binding assigned",
            cancel ? "No changes were made." : "The new input is saved.");
    }
}

void SkySettingsOverlay::RefreshBindingLabels() noexcept {
    for (RecompRuntimeUiItem& item : menu_items_) {
        bool controller = false;
        const int slot = item.key == nullptr
            ? -1 : BindingSlotForKey(item.key, &controller);
        if (slot < 0) continue;
        const size_t label_slot = static_cast<size_t>(slot) +
            (controller ? 0 : kBindingCount);
        const auto& bindings = controller ? state_.controller_bindings
                                          : state_.keyboard_bindings;
        binding_labels_[label_slot] =
            std::string(kBindingActionNames[static_cast<size_t>(slot)]) +
            ": " + BindingDisplayName(
                bindings[static_cast<size_t>(slot)], controller);
        item.label = binding_labels_[label_slot].c_str();
    }
}

bool SkySettingsOverlay::HandleInput(const OverlayInputState& input) noexcept {
    if (ui_ == nullptr) {
        return false;
    }

    const bool chord_now = input.gamepad_select_held && input.gamepad_start_held;
    const bool chord_edge = chord_now && !prev_chord_held_;
    prev_chord_held_ = chord_now;

    if (input.f1_pressed || input.gamepad_guide_pressed || chord_edge) {
        SetVisible(!IsVisible());
    }

    if (input.nav_input.has_value()) {
        if (*input.nav_input == RECOMP_RUNTIME_UI_INPUT_TOGGLE) {
            SetVisible(!IsVisible());
        } else if (IsVisible()) {
            (void)recomp_runtime_ui_handle_input(ui_, *input.nav_input, 1, 0);
        }
    }

    return IsVisible();
}

void SkySettingsOverlay::ApplyToSubsystems(
    gpu2d::WidescreenPpu& ppu,
    camera::DungeonCameraHook& camera_hook,
    camera::SubpixelInterpolator& interpolator,
    shaders::WeatherShaderPipeline& shader_pipeline) const noexcept {
    ppu.SetAspectRatio(state_.aspect_ratio);
    ppu.SetHudLayoutMode(state_.hud_layout);
    camera_hook.SetAspectRatio(state_.aspect_ratio);
    interpolator.SetRefreshRateMode(state_.refresh_rate);
    shader_pipeline.SetFilterPreset(state_.shader_filter);
    shader_pipeline.SetWeatherBloomEnabled(state_.weather_ambient_bloom);
}

void SkySettingsOverlay::ScaleAudioSamplesInt16(std::span<int16_t> pcm_samples) const noexcept {
    const int eff_vol = GetEffectiveAudioVolumePercent();
    if (eff_vol >= 100) {
        return;
    }
    if (eff_vol <= 0) {
        std::fill(pcm_samples.begin(), pcm_samples.end(), int16_t{0});
        return;
    }
    for (int16_t& sample : pcm_samples) {
        const int32_t scaled = (static_cast<int32_t>(sample) * eff_vol) / 100;
        sample = static_cast<int16_t>(std::clamp<int32_t>(scaled, -32768, 32767));
    }
}

DsButtonMapping SkySettingsOverlay::GetActiveButtonMapping() const noexcept {
    DsButtonMapping map{};
    const auto& bindings = state_.controller_bindings;
    auto bit_for_code = [&bindings](int code, int slot) {
        if (slot < 0 || static_cast<size_t>(slot) >= bindings.size()) return uint16_t{0};
        return bindings[static_cast<size_t>(slot)] == code
            ? kDsBits[static_cast<size_t>(slot)] : uint16_t{0};
    };
    map.confirm_bit = bit_for_code(state_.controller_bindings[4], 4);
    map.cancel_bit = bit_for_code(state_.controller_bindings[5], 5);
    map.menu_bit = bit_for_code(state_.controller_bindings[6], 6);
    map.sort_bit = bit_for_code(state_.controller_bindings[7], 7);
    map.shoulder_l_bit = bit_for_code(state_.controller_bindings[8], 8);
    map.shoulder_r_bit = bit_for_code(state_.controller_bindings[9], 9);
    return map;
}

uint16_t SkySettingsOverlay::RemapDsKeyinputMask(uint16_t logical_pressed_mask) const noexcept {
    constexpr uint16_t kActionBitsMask =
        (1u << 0) | (1u << 1) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11);
    const DsButtonMapping map = GetActiveButtonMapping();
    uint16_t out = logical_pressed_mask & static_cast<uint16_t>(~kActionBitsMask);
    if ((logical_pressed_mask & (1u << 0)) != 0u) {
        out |= map.confirm_bit;
    }
    if ((logical_pressed_mask & (1u << 1)) != 0u) {
        out |= map.cancel_bit;
    }
    if ((logical_pressed_mask & (1u << 8)) != 0u) {
        out |= map.shoulder_r_bit;
    }
    if ((logical_pressed_mask & (1u << 9)) != 0u) {
        out |= map.shoulder_l_bit;
    }
    if ((logical_pressed_mask & (1u << 10)) != 0u) {
        out |= map.menu_bit;
    }
    if ((logical_pressed_mask & (1u << 11)) != 0u) {
        out |= map.sort_bit;
    }
    return out;
}

bool SkySettingsOverlay::RenderArgb8888(
    std::span<uint32_t> framebuffer_argb,
    int width,
    int height) noexcept {
    if (!IsVisible()) {
        return false;
    }
    if (width <= 0 || height <= 0 ||
        framebuffer_argb.size() < static_cast<size_t>(width) * static_cast<size_t>(height)) {
        return false;
    }

    recomp_runtime_ui_render_argb8888(
        ui_,
        framebuffer_argb.data(),
        width,
        height,
        width * static_cast<int>(sizeof(uint32_t)));
    return true;
}

std::string SkySettingsOverlay::SerializeToToml() const {
    std::ostringstream oss;
    oss << "[graphics]\n";
    oss << "aspect_ratio = " << static_cast<int>(state_.aspect_ratio) << "\n";
    oss << "hud_layout = " << static_cast<int>(state_.hud_layout) << "\n";
    oss << "refresh_rate = " << static_cast<int>(state_.refresh_rate) << "\n";
    oss << "shader_filter = " << static_cast<int>(state_.shader_filter) << "\n";
    oss << "weather_ambient_bloom = " << (state_.weather_ambient_bloom ? "true" : "false") << "\n\n";
    oss << "[audio]\n";
    oss << "volume_percent = " << state_.audio_volume_percent << "\n";
    oss << "muted = " << (state_.audio_muted ? "true" : "false") << "\n\n";
    oss << "[input]\n";
    oss << "controller_preset = " << static_cast<int>(state_.controller_preset) << "\n";
    oss << "controller_bindings = \"";
    for (size_t i = 0; i < state_.controller_bindings.size(); ++i) {
        if (i) oss << ',';
        oss << state_.controller_bindings[i];
    }
    oss << "\"\nkeyboard_bindings = \"";
    for (size_t i = 0; i < state_.keyboard_bindings.size(); ++i) {
        if (i) oss << ',';
        oss << state_.keyboard_bindings[i];
    }
    oss << "\"\n";
    return oss.str();
}

bool SkySettingsOverlay::LoadFromToml(std::string_view toml_text) noexcept {
    SkySettingsState parsed = state_;
    bool found_setting = false;
    size_t pos = 0;
    while (pos < toml_text.size()) {
        size_t next_nl = toml_text.find('\n', pos);
        if (next_nl == std::string_view::npos) {
            next_nl = toml_text.size();
        }
        const std::string line = TrimWhitespace(toml_text.substr(pos, next_nl - pos));
        pos = next_nl + 1;

        if (line.empty() || line[0] == '#' || line[0] == '[') {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = TrimWhitespace(std::string_view(line).substr(0, eq));
        const std::string val = TrimWhitespace(std::string_view(line).substr(eq + 1));

        if (key == "aspect_ratio") {
            const int v = std::clamp(std::atoi(val.c_str()), 0, 3);
            parsed.aspect_ratio = static_cast<gpu2d::AspectRatioPreset>(v);
        } else if (key == "hud_layout") {
            const int v = std::clamp(std::atoi(val.c_str()), 0, 1);
            parsed.hud_layout = static_cast<gpu2d::HudLayoutMode>(v);
        } else if (key == "refresh_rate") {
            const int v = std::clamp(std::atoi(val.c_str()), 0, 4);
            parsed.refresh_rate = static_cast<camera::RefreshRateMode>(v);
        } else if (key == "shader_filter") {
            const int v = std::clamp(std::atoi(val.c_str()), 0, 4);
            parsed.shader_filter = static_cast<shaders::ShaderFilterPreset>(v);
        } else if (key == "weather_ambient_bloom") {
            parsed.weather_ambient_bloom = (val == "true" || val == "1");
        } else if (key == "volume_percent") {
            parsed.audio_volume_percent = std::clamp(std::atoi(val.c_str()), 0, 100);
        } else if (key == "muted") {
            parsed.audio_muted = (val == "true" || val == "1");
        } else if (key == "controller_preset") {
            const int v = std::clamp(std::atoi(val.c_str()), 0, 3);
            parsed.controller_preset = static_cast<ControllerRemapPreset>(v);
        } else if (key == "controller_bindings") {
            std::string_view bindings = val;
            if (bindings.size() >= 2 && bindings.front() == '"' &&
                bindings.back() == '"') {
                bindings = bindings.substr(1, bindings.size() - 2);
            }
            if (!ParseBindings(bindings, parsed.controller_bindings, true)) continue;
        } else if (key == "keyboard_bindings") {
            std::string_view bindings = val;
            if (bindings.size() >= 2 && bindings.front() == '"' &&
                bindings.back() == '"') {
                bindings = bindings.substr(1, bindings.size() - 2);
            }
            if (!ParseBindings(bindings, parsed.keyboard_bindings, false)) continue;
        } else {
            continue;
        }
        found_setting = true;
    }

    if (!found_setting) {
        return false;
    }
    SetState(parsed);
    return true;
}

bool SkySettingsOverlay::SaveToFile(const std::filesystem::path& path) const noexcept {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out << SerializeToToml();
    return out.good();
}

bool SkySettingsOverlay::LoadFromFile(const std::filesystem::path& path) noexcept {
    if (path.empty()) {
        return false;
    }
    std::ifstream in(path, std::ios::in);
    if (!in.is_open()) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return LoadFromToml(ss.str());
}

} // namespace skyrecomp::ui
