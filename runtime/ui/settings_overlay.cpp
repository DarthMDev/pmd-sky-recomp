#include "settings_overlay.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace skyrecomp::ui {
namespace {

constexpr const char* kSectionName = "SkyRecompiled";

constexpr const char* kKeyAspectRatio = "sky.aspect_ratio";
constexpr const char* kKeyHudLayout = "sky.hud_layout";
constexpr const char* kKeyRefreshRate = "sky.refresh_rate";
constexpr const char* kKeyShaderFilter = "sky.shader_filter";
constexpr const char* kKeyWeatherBloom = "sky.weather_bloom";
constexpr const char* kKeyAudioVolume = "sky.audio_volume";
constexpr const char* kKeyControllerPreset = "sky.controller_preset";
constexpr const char* kKeyResume = RECOMP_RUNTIME_UI_KEY_RESUME;

constexpr const char* const kAspectChoices[] = {
    "4:3 (256x192)",
    "16:10 (308x192)",
    "16:9 (342x192)",
    "21:9 (448x192)",
};

constexpr const char* const kHudLayoutChoices[] = {
    "Dual-Screen Edge-Anchored",
    "Single-Screen Modern",
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
};

constexpr RecompRuntimeUiItem kMenuItems[] = {
    {
        .key = kKeyAspectRatio,
        .section = kSectionName,
        .label = "Aspect Ratio",
        .description = "True 2D widescreen viewport width & overlay29 tile-column streaming radius.",
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
        .section = kSectionName,
        .label = "HUD Layout",
        .description = "Edge-anchored classic HUD or single-screen modern translucent HUD + minimap.",
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
        .section = kSectionName,
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
        .section = kSectionName,
        .label = "HD Shader Filter",
        .description = "Apple Metal / SDL_GPU pixel-art anti-aliasing and sub-pixel display mask.",
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
        .section = kSectionName,
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
        .section = kSectionName,
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
        .key = kKeyControllerPreset,
        .section = kSectionName,
        .label = "Controller Mapping",
        .description = "Gamepad button layout preset for dungeon exploration and menus.",
        .type = RECOMP_RUNTIME_UI_CHOICE,
        .minimum = 0,
        .maximum = 2,
        .step = 1,
        .choices = kControllerChoices,
        .choice_count = std::size(kControllerChoices),
        .choice_values = nullptr,
    },
    {
        .key = kKeyResume,
        .section = kSectionName,
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

} // namespace

SkySettingsOverlay::SkySettingsOverlay(const SkySettingsState& initial_state) noexcept
    : state_(initial_state) {
    RecompRuntimeUiConfig cfg{};
    cfg.title = "SkyRecompiled HD Settings";
    cfg.subtitle = "PMD: Explorers of Sky -- Widescreen / High-Refresh / Metal Shaders";
    cfg.items = kMenuItems;
    cfg.item_count = std::size(kMenuItems);
    cfg.callbacks = {
        .context = this,
        .get_value = &SkySettingsOverlay::CallbackGetValue,
        .set_value = &SkySettingsOverlay::CallbackSetValue,
        .run_action = &SkySettingsOverlay::CallbackRunAction,
        .is_enabled = &SkySettingsOverlay::CallbackIsEnabled,
        .save = nullptr,
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
        *value_out = self->state_.audio_muted ? 0 : self->state_.audio_volume_percent;
        return 1;
    }
    if (key == kKeyControllerPreset) {
        *value_out = static_cast<int>(self->state_.controller_preset);
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

    if (key == kKeyAspectRatio) {
        self->state_.aspect_ratio = static_cast<gpu2d::AspectRatioPreset>(std::clamp(value, 0, 3));
        return 1;
    }
    if (key == kKeyHudLayout) {
        self->state_.hud_layout = static_cast<gpu2d::HudLayoutMode>(std::clamp(value, 0, 1));
        return 1;
    }
    if (key == kKeyRefreshRate) {
        self->state_.refresh_rate = static_cast<camera::RefreshRateMode>(std::clamp(value, 0, 4));
        return 1;
    }
    if (key == kKeyShaderFilter) {
        self->state_.shader_filter = static_cast<shaders::ShaderFilterPreset>(std::clamp(value, 0, 4));
        return 1;
    }
    if (key == kKeyWeatherBloom) {
        self->state_.weather_ambient_bloom = (value != 0);
        return 1;
    }
    if (key == kKeyAudioVolume) {
        self->state_.audio_volume_percent = std::clamp(value, 0, 100);
        self->state_.audio_muted = (self->state_.audio_volume_percent == 0);
        return 1;
    }
    if (key == kKeyControllerPreset) {
        self->state_.controller_preset = static_cast<ControllerRemapPreset>(std::clamp(value, 0, 2));
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
    if (std::string_view(item->key) == kKeyResume && self->ui_ != nullptr) {
        recomp_runtime_ui_close(self->ui_);
        return 1;
    }
    return 0;
}

int SkySettingsOverlay::CallbackIsEnabled(
    void* /*context*/,
    const RecompRuntimeUiItem* /*item*/) noexcept {
    return 1;
}

bool SkySettingsOverlay::IsVisible() const noexcept {
    return ui_ != nullptr && recomp_runtime_ui_is_open(ui_) != 0;
}

void SkySettingsOverlay::SetVisible(bool visible) noexcept {
    if (ui_ == nullptr) {
        return;
    }
    if (visible) {
        if (!recomp_runtime_ui_open_section(ui_, kSectionName)) {
            recomp_runtime_ui_open(ui_);
        }
    } else {
        recomp_runtime_ui_close(ui_);
    }
}

void SkySettingsOverlay::SetState(const SkySettingsState& state) noexcept {
    state_ = state;
    state_.audio_volume_percent = std::clamp(state_.audio_volume_percent, 0, 100);
}

size_t SkySettingsOverlay::GetSelectedIndex() const noexcept {
    if (ui_ == nullptr) {
        return 0;
    }
    return recomp_runtime_ui_view_selected_row(ui_);
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

DsButtonMapping SkySettingsOverlay::GetActiveButtonMapping() const noexcept {
    DsButtonMapping map{};
    switch (state_.controller_preset) {
        case ControllerRemapPreset::StandardNintendoDs:
            break;
        case ControllerRemapPreset::ModernDungeon:
            // Swap Y (diagonal/sort) onto Left Shoulder for effortless diagonal dungeon movement
            map.shoulder_l_bit = (1u << 11); // KEY_Y
            map.sort_bit = (1u << 9);        // KEY_L
            break;
        case ControllerRemapPreset::SwappedConfirmCancel:
            std::swap(map.confirm_bit, map.cancel_bit);
            break;
    }
    return map;
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
    return oss.str();
}

bool SkySettingsOverlay::LoadFromToml(std::string_view toml_text) noexcept {
    SkySettingsState parsed = state_;
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
            const int v = std::clamp(std::atoi(val.c_str()), 0, 2);
            parsed.controller_preset = static_cast<ControllerRemapPreset>(v);
        }
    }

    SetState(parsed);
    return true;
}

} // namespace skyrecomp::ui
