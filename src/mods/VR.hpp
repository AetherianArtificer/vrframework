#pragma once

#include <array>

#include <chrono>
#include <bitset>
#include <memory>
#include <shared_mutex>

#include <openvr.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl.h>

#include "utility/Patch.hpp"
#include "math/Math.hpp"
#include "vr/D3D11Component.hpp"
#include "vr/D3D12Component.hpp"
#include "vr/OverlayComponent.hpp"
#include "vr/runtimes/OpenXR.hpp"
#include "vr/runtimes/OpenVR.hpp"

#include "Mod.hpp"

#ifndef VR_DEFAULT_SYNC_INTERVAL
#define VR_DEFAULT_SYNC_INTERVAL 0
#endif

class VR : public Mod {
public:
    inline VR() {
        add_components_vr();
    }

    static const inline std::string s_action_trigger = "/actions/default/in/Trigger";
    static const inline std::string s_action_grip = "/actions/default/in/Grip";
    static const inline std::string s_action_joystick = "/actions/default/in/Joystick";
    static const inline std::string s_action_joystick_click = "/actions/default/in/JoystickClick";

    static const inline std::string s_action_a_button_left = "/actions/default/in/AButtonLeft";
    static const inline std::string s_action_b_button_left = "/actions/default/in/BButtonLeft";
    static const inline std::string s_action_a_button_touch_left = "/actions/default/in/AButtonTouchLeft";
    static const inline std::string s_action_b_button_touch_left = "/actions/default/in/BButtonTouchLeft";

    static const inline std::string s_action_a_button_right = "/actions/default/in/AButtonRight";
    static const inline std::string s_action_b_button_right = "/actions/default/in/BButtonRight";
    static const inline std::string s_action_a_button_touch_right = "/actions/default/in/AButtonTouchRight";
    static const inline std::string s_action_b_button_touch_right = "/actions/default/in/BButtonTouchRight";

    static const inline std::string s_action_dpad_up = "/actions/default/in/DPad_Up";
    static const inline std::string s_action_dpad_right = "/actions/default/in/DPad_Right";
    static const inline std::string s_action_dpad_down = "/actions/default/in/DPad_Down";
    static const inline std::string s_action_dpad_left = "/actions/default/in/DPad_Left";
    static const inline std::string s_action_system_button = "/actions/default/in/SystemButton";
    static const inline std::string s_action_thumbrest_touch_left = "/actions/default/in/ThumbrestTouchLeft";
    static const inline std::string s_action_thumbrest_touch_right = "/actions/default/in/ThumbrestTouchRight";
    static std::shared_ptr<VR>& get();

    std::string_view get_name() const override { return "VR"; }

    // Called when the mod is initialized
    std::optional<std::string> on_initialize_d3d_thread() override;

//    void on_lua_state_created(sol::state& lua) override;

    void on_pre_imgui_frame() override;
    void on_present() override;
    void on_post_present() override;

//    void on_update_transform(void* transform) override;
//    void on_update_camera_controller(void* controller) override;
//    bool on_pre_gui_draw_element(void* gui_element, void* primitive_context) override;
//    void on_gui_draw_element(void* gui_element, void* primitive_context) override;
//    void on_pre_update_before_lock_scene(void* ctx) override;
//    void on_pre_lightshaft_draw(void* shaft, void* render_context) override;
//    void on_lightshaft_draw(void* shaft, void* render_context) override;

//    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
//    void on_application_entry(void* entry, const char* name, size_t hash) override;

    void on_draw_ui() override;
    void on_device_reset() override;

    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
    void on_config_save(utility::Config& cfg) override;

    // Application entries
    void on_pre_update_hid(void* entry);
    void on_update_hid(void* entry);
    virtual bool on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) override;
    void on_xinput_get_state(uint32_t* retval, uint32_t user_index, XINPUT_STATE* state) override;
    void on_xinput_get_capabilities(uint32_t* retval, uint32_t user_index, uint32_t flags, XINPUT_CAPABILITIES* capabilities) override;
    void on_xinput_set_state(uint32_t* retval, uint32_t user_index, XINPUT_VIBRATION* vibration) override;
    void on_begin_rendering(int frame);
//    void on_pre_end_rendering(void* entry);
    void on_end_rendering(void* entry);
//    void on_pre_wait_rendering(void* entry);
    void on_wait_rendering(int frame);
    // The render thread starts submitting a frame: with pipelining, the frame begins here.
    void on_render_start(int frame);
    void set_pipelined_frames(bool pipelined) { m_pipelined_frames = pipelined; }
    void set_resolution_scale(float scale) {
        m_resolution_scale->value() = scale;
        if (m_openxr) {
            m_openxr->resolution_scale = scale;
        }
    }
    void set_world_scale(float scale) { m_world_scale_option->value() = scale; }

    // Headset refresh period in seconds, or 0 when unknown.
    double get_display_period() {
        if (!get_runtime()->is_openxr() || !m_openxr->ready()) {
            return 0.0;
        }
        return m_openxr->get_pipeline_state().frame_state.predictedDisplayPeriod / 1e9;
    }

    // 0 = off, 1 = strongest; drawn over each eye image before submission.
    void set_comfort_vignette(float strength) { m_comfort_vignette = strength; }
    float get_comfort_vignette() const { return m_comfort_vignette; }
    // 0 = clear, 1 = black; drawn over each eye image before submission.
    void set_comfort_fade(float alpha) { m_comfort_fade = alpha; }
    float get_comfort_fade() const { return m_comfort_fade; }

    auto get_backbuffer_size() const {
        if( m_is_d3d12) {
            return m_d3d12.get_backbuffer_size();
        } else {
            return m_d3d11.get_backbuffer_size();
        }
    }

    //unsigned long frame_count_temp{0};
    float separation_ipd{0.068f};

    template<typename T = VRRuntime>
    T* get_runtime() const {
        return (T*)m_runtime.get();
    }

    auto get_hmd() const {
        return m_openvr->hmd;
    }

    auto& get_openvr_poses() const {
        return m_openvr->render_poses;
    }

    auto& get_overlay_component() {
        return m_overlay_component;
    }

    auto get_hmd_width() const {
        return get_runtime()->get_width();
    }

    auto get_hmd_height() const {
        return get_runtime()->get_height();
    }

    auto get_last_controller_update() const {
        return m_last_controller_update;
    }

//    int32_t get_frame_count() const;
//    int32_t get_game_frame_count() const;

    // Native stereo: both eyes render every engine frame, each over the whole frame, and the game hands over a copy of
    // each finished eye image; the back buffer holds whichever eye rendered last.
    bool is_native_stereo() const { return m_native_stereo; }
    void request_native_stereo(bool enable) { m_native_stereo_requested = enable; }
    // Frames without both eye images (loading screens, fullscreen menus) show the back buffer to both eyes.
    // Decided on the game thread for the frame it starts, read when that frame is presented.
    void set_native_mono_frame(bool mono) { m_frame_mono[m_engine_frame_count & 7] = mono; }
    bool is_native_mono_frame() const { return m_frame_mono[m_presenter_frame_count & 7]; }
    void set_frame_flat(bool flat) { m_frame_flat[m_engine_frame_count & 7] = flat; }
    bool is_presented_frame_flat() const { return m_frame_flat[m_presenter_frame_count & 7]; }
    // The texture stays in PIXEL_SHADER_RESOURCE state between frames.
    void set_native_eye_source(uint32_t eye, ID3D12Resource* texture) {
        std::scoped_lock _{ m_eye_source_mutex };
        m_native_eye_sources[eye & 1] = texture;
        m_native_eye_source_frames[eye & 1] = m_engine_frame_count;
    }
    // The game's UI layer, drawn over both captured eye images; same state rules as the eye images.
    void set_native_ui_source(ID3D12Resource* texture) {
        std::scoped_lock _{ m_eye_source_mutex };
        m_native_ui_source = texture;
        m_native_ui_source_frame = m_engine_frame_count;
    }
    Microsoft::WRL::ComPtr<ID3D12Resource> get_native_ui_source() {
        std::scoped_lock _{ m_eye_source_mutex };
        if (m_engine_frame_count - m_native_ui_source_frame > 3) {
            return nullptr;
        }
        return m_native_ui_source;
    }
    // Native stereo can show the game's HUD on a panel in the world instead of over each eye: width and distance in metres.
    void set_native_hud_panel(bool enabled, float width, float distance) {
        m_native_hud_panel = enabled;
        m_native_hud_panel_width = width;
        m_native_hud_panel_distance = distance;
    }
    bool is_native_hud_panel() const { return m_native_hud_panel; }

    // Parts of the HUD shown on the wrists like watches, each with its hand (-1 none, 0 left, 1 right), its rectangle in
    // the HUD image as fractions, and its width on the wrist in metres. They are cut out of the floating HUD panel.
    struct WristPanel {
        int hand{ -1 };
        std::array<float, 4> rect{};
        float width{ 0.0f };
    };
    static constexpr int kWristPanels = 2;
    static constexpr int kWristImageWidth = 1024;
    static constexpr int kWristImageHeight = 512;
    void set_native_wrist_panels(const std::array<WristPanel, kWristPanels>& panels) {
        std::scoped_lock _{ m_wrist_mtx };
        m_wrist_panels = panels;
    }
    std::array<WristPanel, kWristPanels> get_native_wrist_panels() const {
        std::scoped_lock _{ m_wrist_mtx };
        return m_wrist_panels;
    }
    // Other parts of the HUD left off the floating HUD panel, as fractions of the HUD image (left, top, right, bottom).
    static constexpr int kHudCuts = 2;
    void set_native_hud_cuts(const std::array<std::array<float, 4>, kHudCuts>& cuts) {
        std::scoped_lock _{ m_wrist_mtx };
        m_hud_cuts = cuts;
    }
    std::array<std::array<float, 4>, kHudCuts> get_native_hud_cuts() const {
        std::scoped_lock _{ m_wrist_mtx };
        return m_hud_cuts;
    }
    bool has_native_wrist_panels() const {
        std::scoped_lock _{ m_wrist_mtx };
        return m_wrist_panels[0].hand >= 0 || m_wrist_panels[1].hand >= 0;
    }
    // A panel's place in the wrist image: its own half, the part fitted inside it at its aspect ratio.
    static std::array<int32_t, 4> wrist_slot(int slot, float part_width, float part_height) {
        const float cell = (float)kWristImageWidth / kWristPanels;
        const float scale = std::min(cell / std::max(part_width, 1.0f), (float)kWristImageHeight / std::max(part_height, 1.0f));
        const int32_t w = (int32_t)(part_width * scale);
        const int32_t h = (int32_t)(part_height * scale);
        return { (int32_t)(slot * cell), 0, (int32_t)(slot * cell) + w, h };
    }
    float get_native_hud_panel_width() const { return m_native_hud_panel_width; }
    float get_native_hud_panel_distance() const { return m_native_hud_panel_distance; }
    // Null when the eye has not been captured in the last few frames.
    Microsoft::WRL::ComPtr<ID3D12Resource> get_native_eye_source(uint32_t eye) {
        std::scoped_lock _{ m_eye_source_mutex };
        if (m_engine_frame_count - m_native_eye_source_frames[eye & 1] > 3) {
            return nullptr;
        }
        return m_native_eye_sources[eye & 1];
    }
    // The next presented back buffer is saved as a PNG to this path.
    void request_backbuffer_dump(std::wstring path) {
        std::scoped_lock _{ m_dump_mutex };
        m_dump_path = std::move(path);
    }
    std::wstring take_backbuffer_dump_request() {
        std::scoped_lock _{ m_dump_mutex };
        return std::exchange(m_dump_path, {});
    }

    bool is_gui_enabled() const {
        return true;
    }

    // Functions that generally use a mutex or have more complex logic
    Matrix4x4f get_transform_offset();
    void set_transform_offset(const Matrix4x4f& offset);
    void recenter_view();
    // Counts recenters, so code holding head-relative state can reset it.
    uint32_t get_recenter_count() const { return m_recenter_count.load(); }

    glm::quat get_gui_rotation_offset();
    void set_gui_rotation_offset(const glm::quat& offset);
    void recenter_gui(const glm::quat& from);

    Matrix4x4f get_current_eye_transform(bool flip = false);
    Matrix4x4f get_eye_transform(VRRuntime::Eye eye);
    Matrix4x4f get_current_projection_matrix(bool flip = false);

    auto& get_controllers() const {
        return m_controllers;
    }

    bool is_using_controllers() const {
        return m_controllers_allowed->value() && is_hmd_active() && !m_controllers.empty() && (std::chrono::steady_clock::now() - m_last_controller_update) <= std::chrono::seconds((int32_t)m_motion_controls_inactivity_timer->value());
    }

    bool is_using_controllers_within(std::chrono::seconds seconds) const {
        return m_controllers_allowed->value() && is_hmd_active() && !m_controllers.empty() && (std::chrono::steady_clock::now() - m_last_controller_update) <= seconds;
    }

    bool is_hmd_active() const {
        return get_runtime()->ready();
    }
    
    bool is_openvr_loaded() const {
        return m_openvr != nullptr && m_openvr->loaded;
    }

    bool is_openxr_loaded() const {
        return m_openxr != nullptr && m_openxr->loaded;
    }

    bool is_using_hmd_oriented_audio() {
        return m_hmd_oriented_audio->value();
    }

    void toggle_hmd_oriented_audio() {
        m_hmd_oriented_audio->toggle();
    }

    const Matrix4x4f& get_last_render_matrix() {
        return m_render_camera_matrix;
    }

    Vector4f get_position(uint32_t index)  const;
    Vector4f get_velocity(uint32_t index)  const;
    Vector4f get_angular_velocity(uint32_t index)  const;
    Matrix4x4f get_rotation(uint32_t index)  const;
    Matrix4x4f get_transform(uint32_t index) const;
    // Controller grip pose (palm), same space as get_transform; falls back to the aim pose.
    Matrix4x4f get_grip_transform(uint32_t index) const;
    // Eye height above the floor in metres (OpenXR stage space), or a negative value when unavailable.
    float get_floor_eye_height() const;

    // Analog trigger/grip (0..1) and finger touch sensing for hand poses.
    struct FingerState {
        float trigger{ 0.0f };
        float grip{ 0.0f };
        bool trigger_touch{ false };
        bool thumb_touch{ false };
    };
    FingerState get_finger_state(bool left) const;

    // A tracked joint in the same stage space as get_transform.
    struct TrackedJoint {
        Vector3f position{};
        glm::quat orientation{ 1.0f, 0.0f, 0.0f, 0.0f };
        bool position_valid{ false };
        bool orientation_valid{ false };
    };

    // Quest body tracking (XR_FB_body_tracking, with XR_META_body_tracking_full_body when available).
    // Joints use XrFullBodyJointMETA indices (0-69 match XrBodyJointFB; 70-83 are the legs).
    struct BodyTrackingState {
        bool supported{ false };      // runtime reports supportsBodyTracking
        bool full_body{ false };      // legs available (supportsFullBodyTracking)
        bool active{ false };         // this frame's locations are active
        bool high_fidelity{ false };  // XR_META_body_tracking_fidelity reports high fidelity
        float confidence{ 0.0f };
        uint32_t joint_count{ 0 };
        std::array<TrackedJoint, 84> joints{};
    };
    // Returns false when no tracker exists; check `active` for this frame's data.
    bool get_body_tracking(BodyTrackingState& out) const;

    // Hand tracking (XR_EXT_hand_tracking), XrHandJointEXT indices.
    struct HandTrackingState {
        bool supported{ false };
        bool active{ false };
        int data_source{ 0 };  // 0 unknown, 1 tracked hand (unobstructed), 2 derived from the controller
        std::array<TrackedJoint, 26> joints{};
    };
    bool get_hand_tracking(bool left, HandTrackingState& out) const;
    vr::HmdMatrix34_t get_raw_transform(uint32_t index) const;

    const auto& get_eyes() const {
        return get_runtime()->eyes;
    }

    void apply_hmd_transform(glm::quat& rotation, Vector4f& position);
//    void apply_hmd_transform(::REJoint* camera_joint);
    
    bool is_hand_behind_head(VRRuntime::Hand hand, float sensitivity = 0.2f) const;
    bool is_action_active_any_joystick(vr::VRActionHandle_t action) const;
    bool is_action_active(vr::VRActionHandle_t action, vr::VRInputValueHandle_t source = vr::k_ulInvalidInputValueHandle) const;
    Vector2f get_joystick_axis(vr::VRInputValueHandle_t handle) const;


    inline vr::VRActionHandle_t get_action_handle(std::string_view action_path) {
        if (auto it = m_action_handles.find(action_path.data()); it != m_action_handles.end()) {
            return it->second;
        }

        return vr::k_ulInvalidActionHandle;
    }

    Vector2f get_left_stick_axis() const;
    Vector2f get_right_stick_axis() const;

    void trigger_haptic_vibration(float seconds_from_now, float duration, float frequency, float amplitude, vr::VRInputValueHandle_t source = vr::k_ulInvalidInputValueHandle);
    
//    auto get_action_set() const { return m_action_set; }
//    auto& get_active_action_set() const { return m_active_action_set; }
//    auto get_action_trigger() const { return m_action_trigger; }
//    auto get_action_grip() const { return m_action_grip; }
//    auto get_action_joystick() const { return m_action_joystick; }
//    auto get_action_joystick_click() const { return m_action_joystick_click; }
//    auto get_action_a_button() const { return m_action_a_button; }
//    auto get_action_b_button() const { return m_action_b_button; }
//    auto get_action_weapon_dial() const { return m_action_weapon_dial; }
//    auto get_action_minimap() const { return m_action_minimap; }
//    auto get_action_block() const { return m_action_block; }
//    auto get_action_dpad_up() const { return m_action_dpad_up; }
//    auto get_action_dpad_down() const { return m_action_dpad_down; }
//    auto get_action_dpad_left() const { return m_action_dpad_left; }
//    auto get_action_dpad_right() const { return m_action_dpad_right; }
//    auto get_action_heal() const { return m_action_heal; }


    int get_left_controller_index() const {
        const auto wants_swap = m_swap_controllers->value();

        if (m_runtime->is_openxr()) {
            return wants_swap ? 2 : 1;
        } else if (m_runtime->is_openvr()) {
            return !m_controllers.empty() ? (wants_swap ? m_controllers[1] : m_controllers[0]) : -1;
        }

        return -1;
    }


    int get_right_controller_index() const {
        const auto wants_swap = m_swap_controllers->value();

        if (m_runtime->is_openxr()) {
            return wants_swap ? 1 : 2;
        } else if (m_runtime->is_openvr()) {
            return !m_controllers.empty() ? (wants_swap ? m_controllers[0] : m_controllers[1]) : -1;
        }

        return -1;
    }

    auto get_left_joystick() const { return m_left_joystick; }
    auto get_right_joystick() const { return m_right_joystick; }

    const auto& get_action_handles() const { return m_action_handles;}

    auto get_ui_scale() const { return m_ui_scale_option->value(); }
    const auto& get_raw_projections() const { return get_runtime()->raw_projections; }

    void unhide_crosshair() {
        m_last_crosshair_hide = std::chrono::steady_clock::now();
    }

    void update_action_states();

private:
    Vector4f get_position_unsafe(uint32_t index) const;
    Vector4f get_velocity_unsafe(uint32_t index) const;
    Vector4f get_angular_velocity_unsafe(uint32_t index) const;

private:
    // Hooks
//    void on_view_get_size(REManagedObject* scene_view, float* result) override;
//    static void inputsystem_update_hook(void* ctx, REManagedObject* input_system);
//    void on_camera_get_projection_matrix(REManagedObject* camera, Matrix4x4f* result) override;
//    static Matrix4x4f* gui_camera_get_projection_matrix_hook(REManagedObject* camera, Matrix4x4f* result);
//    void on_camera_get_view_matrix(REManagedObject* camera, Matrix4x4f* result) override;

//    bool on_pre_overlay_layer_update(sdk::renderer::layer::Overlay* layer, void* render_context) override;
//    bool on_pre_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) override;

//    bool on_pre_post_effect_layer_update(sdk::renderer::layer::PostEffect* layer, void* render_context) override;
//    bool on_pre_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_context) override;
//    void on_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_context) override;
    uint32_t m_previous_distortion_type{};
    bool m_set_next_post_effect_distortion_type{false};

//    bool on_pre_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) override;
//    void on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) override;
//    bool on_pre_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_context) override;
//    bool m_set_next_scene_layer_data{false};
//
//    struct SceneLayerData {
//        SceneLayerData() = default;
//        SceneLayerData(sdk::renderer::SceneInfo* info)
//            : scene_info(info)
//        {
//            if (scene_info != nullptr) {
//                this->view_projection_matrix = scene_info->view_projection_matrix;
//            }
//        }
//
//        sdk::renderer::SceneInfo* scene_info{};
//        Matrix4x4f view_projection_matrix{};
//    };
//
//    std::array<SceneLayerData, 5> m_scene_layer_data {};

    static void wwise_listener_update_hook(void* listener);

    //static float get_sharpness_hook(void* tonemapping);

    // initialization functions
    std::optional<std::string> initialize_openvr();
    std::optional<std::string> initialize_openvr_input();
    std::optional<std::string> initialize_openxr();
    std::optional<std::string> initialize_openxr_input();
    std::optional<std::string> initialize_openxr_swapchains();
    std::optional<std::string> hijack_resolution();
    std::optional<std::string> hijack_input();
    std::optional<std::string> hijack_camera();
    std::optional<std::string> hijack_wwise_listeners(); // audio hook

    std::optional<std::string> reinitialize_openvr() {
        spdlog::info("Reinitializing OpenVR");
        std::scoped_lock _{m_openvr_mtx};

        m_runtime.reset();
        m_runtime = std::make_shared<VRRuntime>();
        m_openvr.reset();

        // Reinitialize openvr input, hopefully this fixes the issue
        m_controllers.clear();
        m_controllers_set.clear();

        auto e = initialize_openvr();

        if (e) {
            spdlog::error("Failed to reinitialize OpenVR: {}", *e);
        }

        return e;
    }

    std::optional<std::string> reinitialize_openxr() {
        spdlog::info("Reinitializing OpenXR");
        std::scoped_lock _{m_openvr_mtx};

        if (m_is_d3d12) {
            m_d3d12.openxr().destroy_swapchains();
        } else {
            m_d3d11.openxr().destroy_swapchains();
        }

        m_openxr.reset();
        m_runtime.reset();
        m_runtime = std::make_shared<VRRuntime>();
        
        m_controllers.clear();
        m_controllers_set.clear();

        auto e = initialize_openxr();

        if (e) {
            spdlog::error("Failed to reinitialize OpenXR: {}", *e);
        }

        return e;
    }

    bool detect_controllers();
    bool is_any_action_down();
public:
    void update_hmd_state(int frame);
private:
    void update_camera(); // if not in firstperson mode
    void update_camera_origin(); // every frame
    void update_audio_camera();
    void update_render_matrix();
    void restore_audio_camera(); // after wwise listener update
    void restore_camera(); // After rendering
    void set_lens_distortion(bool value);
    void disable_bad_effects();
    void fix_temporal_effects();

    // input functions
    // Purpose: "Emulate" OpenVR input to the game
    // By setting things like input flags based on controller state
//    void openvr_input_to_re2_re3(REManagedObject* input_system);
    void openvr_input_to_re_engine(); // generic, can be used on any game

    // Sets overlay layer to return instantly
    // causes world-space gui elements to render properly
    Patch::Ptr m_overlay_draw_patch{};
    
    mutable std::recursive_mutex m_openvr_mtx{};
    mutable std::recursive_mutex m_wwise_mtx{};
    mutable std::shared_mutex m_gui_mtx{};
    mutable std::shared_mutex m_rotation_mtx{};

    vr::VRTextureBounds_t m_right_bounds{ 0.0f, 0.0f, 1.0f, 1.0f };
    vr::VRTextureBounds_t m_left_bounds{ 0.0f, 0.0f, 1.0f, 1.0f };

    glm::vec3 m_overlay_rotation{-1.550f, 0.0f, -1.330f};
    glm::vec4 m_overlay_position{0.0f, 0.06f, -0.07f, 1.0f};

public:
    float m_nearz{ 0.1f };
    float m_farz{ 3000.0f };
private:

    std::shared_ptr<VRRuntime> m_runtime{std::make_shared<VRRuntime>()}; // will point to the real runtime if it exists
    std::shared_ptr<runtimes::OpenVR> m_openvr{std::make_shared<runtimes::OpenVR>()};
    std::shared_ptr<runtimes::OpenXR> m_openxr{std::make_shared<runtimes::OpenXR>()};

    Matrix4x4f m_transform_offset{ glm::identity<Matrix4x4f>() };
    glm::quat m_gui_rotation_offset{ glm::identity<glm::quat>() };

    std::vector<int32_t> m_controllers{};
    std::unordered_set<int32_t> m_controllers_set{};

    // Action set handles
    vr::VRActionSetHandle_t m_action_set{};
    vr::VRActiveActionSet_t m_active_action_set{};

    // Action handles
    vr::VRActionHandle_t m_action_trigger{ };
    vr::VRActionHandle_t m_action_grip{ };
    vr::VRActionHandle_t m_action_joystick{};
    vr::VRActionHandle_t m_action_joystick_click{};

    vr::VRActionHandle_t m_action_a_button_right{};
    vr::VRActionHandle_t m_action_a_button_touch_right{};
    vr::VRActionHandle_t m_action_b_button_right{};
    vr::VRActionHandle_t m_action_b_button_touch_right{};

    vr::VRActionHandle_t m_action_a_button_left{};
    vr::VRActionHandle_t m_action_a_button_touch_left{};
    vr::VRActionHandle_t m_action_b_button_left{};
    vr::VRActionHandle_t m_action_b_button_touch_left{};

    vr::VRActionHandle_t m_action_dpad_up{};
    vr::VRActionHandle_t m_action_dpad_right{};
    vr::VRActionHandle_t m_action_dpad_down{};
    vr::VRActionHandle_t m_action_dpad_left{};
    vr::VRActionHandle_t m_action_system_button{};
    vr::VRActionHandle_t m_action_haptic{};
    vr::VRActionHandle_t m_action_thumbrest_touch_left{};
    vr::VRActionHandle_t m_action_thumbrest_touch_right{};
    
    std::unordered_map<std::string, std::reference_wrapper<vr::VRActionHandle_t>> m_action_handles {
        { s_action_trigger, m_action_trigger },
        { s_action_grip, m_action_grip },
        { s_action_joystick, m_action_joystick },
        { s_action_joystick_click, m_action_joystick_click },

        { s_action_a_button_left, m_action_a_button_left },
        { s_action_b_button_left, m_action_b_button_left },
        { s_action_a_button_touch_left, m_action_a_button_touch_left },
        { s_action_b_button_touch_left, m_action_b_button_touch_left },

        { s_action_a_button_right, m_action_a_button_right },
        { s_action_b_button_right, m_action_b_button_right },
        { s_action_a_button_touch_right, m_action_a_button_touch_right },
        { s_action_b_button_touch_right, m_action_b_button_touch_right },

        { s_action_dpad_up, m_action_dpad_up },
        { s_action_dpad_right, m_action_dpad_right },
        { s_action_dpad_down, m_action_dpad_down },
        { s_action_dpad_left, m_action_dpad_left },

        { s_action_system_button, m_action_system_button },
        { s_action_thumbrest_touch_left, m_action_thumbrest_touch_left },
        { s_action_thumbrest_touch_right, m_action_thumbrest_touch_right },

        // Out
        { "/actions/default/out/Haptic", m_action_haptic },
    };

    // Input sources
    vr::VRInputValueHandle_t m_left_joystick{};
    vr::VRInputValueHandle_t m_right_joystick{};

    // Input system history
    std::bitset<64> m_button_states_down{};
    std::bitset<64> m_button_states_on{};
    std::bitset<64> m_button_states_up{};
    std::chrono::steady_clock::time_point m_last_controller_update{};
    std::chrono::steady_clock::time_point m_last_xinput_update{};
    std::chrono::steady_clock::time_point m_last_xinput_spoof_sent{};
    std::chrono::steady_clock::time_point m_last_interaction_display{};
    std::chrono::steady_clock::time_point m_last_crosshair_hide{};
    uint32_t m_backbuffer_inconsistency_start{};
    std::chrono::nanoseconds m_last_input_delay{};
    std::chrono::nanoseconds m_avg_input_delay{};

    uint32_t m_lowest_xinput_user_index{};

    HANDLE m_present_finished_event{CreateEvent(nullptr, TRUE, FALSE, nullptr)};
    std::atomic<bool> m_pipelined_frames{true};
    std::atomic<uint32_t> m_recenter_count{0};

    Vector4f m_raw_projections[2]{};

    vrmod::D3D11Component m_d3d11{};
    vrmod::D3D12Component m_d3d12{};
    vrmod::OverlayComponent m_overlay_component{};

    Vector4f m_original_camera_position{ 0.0f, 0.0f, 0.0f, 0.0f };
    glm::quat m_original_camera_rotation{ glm::identity<glm::quat>() };

    Matrix4x4f m_original_camera_matrix{ glm::identity<Matrix4x4f>() };

    Vector4f m_original_audio_camera_position{ 0.0f, 0.0f, 0.0f, 0.0f };
    glm::quat m_original_audio_camera_rotation{ glm::identity<glm::quat>() };

    Matrix4x4f m_render_camera_matrix{ glm::identity<Matrix4x4f>() };

//    sdk::helpers::NativeObject m_via_hid_gamepad{ "via.hid.GamePad" };
protected:
    inline void add_components_vr() {
        m_components = {
            &m_overlay_component
        };
    }


    // options
public:
    int m_engine_frame_count{0};
    std::atomic<float> m_comfort_vignette{0.0f};
    std::atomic<float> m_comfort_fade{0.0f};
    int m_render_frame_count{0};
    int m_presenter_frame_count{0};
    std::atomic<bool> m_native_stereo{false};
    std::atomic<bool> m_native_stereo_requested{false};
    std::array<std::atomic<bool>, 8> m_frame_mono{};
    std::array<std::atomic<bool>, 8> m_frame_flat{};
    std::atomic<bool> m_native_hud_panel{false};
    std::atomic<float> m_native_hud_panel_width{2.0f};
    std::atomic<float> m_native_hud_panel_distance{2.0f};
    mutable std::mutex m_wrist_mtx{};
    std::array<WristPanel, kWristPanels> m_wrist_panels{};
    std::array<std::array<float, 4>, kHudCuts> m_hud_cuts{};
    std::mutex m_eye_source_mutex{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> m_native_eye_sources{};
    std::array<int64_t, 2> m_native_eye_source_frames{ -100, -100 };
    Microsoft::WRL::ComPtr<ID3D12Resource> m_native_ui_source{};
    int64_t m_native_ui_source_frame{ -100 };
    std::mutex m_dump_mutex{};
    std::wstring m_dump_path{};

private:

    bool m_submitted{false};
    //bool m_disable_sharpening{true};

    bool m_needs_camera_restore{false};
    bool m_needs_audio_restore{false};
    bool m_in_render{false};
    bool m_in_lightshaft{false};
    bool m_positional_tracking{true};
    bool m_is_d3d12{false};
    bool m_backbuffer_inconsistency{false};
    bool m_init_finished{false};
    bool m_spoofed_gamepad_connection{false};
public:
    bool m_has_hw_scheduling{false}; // hardware accelerated GPU scheduling
private:

    // on the backburner
    bool m_depth_aided_reprojection{false};

    // == 1 or == 0

    static std::string actions_json;
    static std::string binding_rift_json;
    static std::string bindings_oculus_touch_json;
    static std::string binding_vive;
    static std::string bindings_vive_controller;
    static std::string bindings_knuckles;

    const std::unordered_map<std::string, std::string> m_binding_files {
        { "actions.json", actions_json },
        { "binding_rift.json", binding_rift_json },
        { "bindings_oculus_touch.json", bindings_oculus_touch_json },
        { "binding_vive.json", binding_vive },
        { "bindings_vive_controller.json", bindings_vive_controller },
        { "bindings_knuckles.json", bindings_knuckles }
    };

    inline static const std::vector<std::string> s_sync_interval_options {
        "Early",
        "Late",
        "VeryLate"
    };

    const ModCombo::Ptr m_sync_interval{ ModCombo::create(generate_name("SyncInterval"), s_sync_interval_options, VR_DEFAULT_SYNC_INTERVAL) };

    const ModKey::Ptr m_recenter_view_key{ ModKey::create(generate_name("RecenterViewKey")) };
    const ModToggle::Ptr m_decoupled_pitch{ ModToggle::create(generate_name("DecoupledPitch"), false) };
    const ModToggle::Ptr m_use_custom_view_distance{ ModToggle::create(generate_name("UseCustomViewDistance"), false) };
    const ModToggle::Ptr m_hmd_oriented_audio{ ModToggle::create(generate_name("HMDOrientedAudio"), true) };
    const ModSlider::Ptr m_view_distance{ ModSlider::create(generate_name("CustomViewDistance"), 10.0f, 3000.0f, 500.0f) };
    const ModSlider::Ptr m_motion_controls_inactivity_timer{ ModSlider::create(generate_name("MotionControlsInactivityTimer"), 30.0f, 100.0f, 60.0f) };
    const ModSlider::Ptr m_joystick_deadzone{ ModSlider::create(generate_name("JoystickDeadzone"), 0.01f, 0.9f, 0.15f) };
    const ModSlider::Ptr m_ui_scale_option{ ModSlider::create(generate_name("2DUIScale"), 1.0f, 100.0f, 12.0f) };
    const ModSlider::Ptr m_ui_distance_option{ ModSlider::create(generate_name("2DUIDistance"), 0.01f, 100.0f, 1.0f) };
    const ModSlider::Ptr m_world_scale_option{ ModSlider::create(generate_name("WorldSpaceScale"), 0.5f, 2.0f, 1.0f) };
    const ModSlider::Ptr m_resolution_scale{ ModSlider::create(generate_name("OpenXRResolutionScale"), 0.1f, 5.0f, 1.0f) };
    const ModSlider::Ptr m_horizontal_fov_scale{ ModSlider::create(generate_name("HorizontalFOVScale"), 0.0f, 1.0f, 1.0f) };
    const ModSlider::Ptr m_vertical_fov_scale{ ModSlider::create(generate_name("VerticalFOVScale"), 0.0f, 1.0f, 1.0f) };
    const ModToggle::Ptr m_extended_fov_rage{ ModToggle::create(generate_name("ExtendedScaleFovRange"), false) };
    const ModSlider::Ptr m_flat_screen_distance{ ModSlider::create(generate_name("FlatSCreenDistance"), 0.0f, 2.0f, 1.5f) };

    const ModToggle::Ptr m_force_fps_settings{ ModToggle::create(generate_name("ForceFPS"), true) };
    const ModToggle::Ptr m_force_aa_settings{ ModToggle::create(generate_name("ForceAntiAliasing"), true) };
    const ModToggle::Ptr m_force_motionblur_settings{ ModToggle::create(generate_name("ForceMotionBlur"), true) };
    const ModToggle::Ptr m_force_vsync_settings{ ModToggle::create(generate_name("ForceVSync"), true) };
    const ModToggle::Ptr m_force_lensdistortion_settings{ ModToggle::create(generate_name("ForceLensDistortion"), true) };
    const ModToggle::Ptr m_force_volumetrics_settings{ ModToggle::create(generate_name("ForceVolumetrics"), true) };
    const ModToggle::Ptr m_force_lensflares_settings{ ModToggle::create(generate_name("ForceLensFlares"), true) };
    const ModToggle::Ptr m_force_dynamic_shadows_settings{ ModToggle::create(generate_name("ForceDynamicShadows"), true) };
    const ModToggle::Ptr m_allow_engine_overlays{ ModToggle::create(generate_name("AllowEngineOverlays"), true) };
    const ModToggle::Ptr m_desktop_fix{ ModToggle::create(generate_name("DesktopRecordingFix"), true) };
    const ModToggle::Ptr m_desktop_fix_skip_present{ ModToggle::create(generate_name("DesktopRecordingFixSkipPresent"), true) };
    const ModToggle::Ptr m_enable_asynchronous_rendering{ ModToggle::create(generate_name("AsyncRendering_V2"), true) };
    const ModToggle::Ptr m_swap_controllers{ ModToggle::create(generate_name("SwapControllerInputs"), false) };

    const ModToggle::Ptr m_controllers_allowed{ ModToggle::create(generate_name("ControllersAllowed"), true) };


    bool m_disable_projection_matrix_override{ false };
    bool m_disable_gui_camera_projection_matrix_override{ false };
    bool m_disable_view_matrix_override{false};
    bool m_disable_backbuffer_size_override{false};
    bool m_disable_temporal_fix{false};
    bool m_disable_post_effect_fix{false};

    ValueList m_options{
        *m_recenter_view_key,
//        *m_decoupled_pitch,
//        *m_use_custom_view_distance,
//        *m_hmd_oriented_audio,
//        *m_view_distance,
          *m_motion_controls_inactivity_timer,
//        *m_joystick_deadzone,
//        *m_force_fps_settings,
//        *m_force_aa_settings,
//        *m_force_motionblur_settings,
//        *m_force_vsync_settings,
//        *m_force_lensdistortion_settings,
//        *m_force_volumetrics_settings,
//        *m_force_lensflares_settings,
//        *m_force_dynamic_shadows_settings,
//        *m_ui_scale_option,
//        *m_ui_distance_option,
        *m_world_scale_option,
//        *m_allow_engine_overlays,
        *m_resolution_scale,
        *m_horizontal_fov_scale,
        *m_vertical_fov_scale,
        *m_extended_fov_rage,
        *m_desktop_fix,
        *m_desktop_fix_skip_present,
        *m_sync_interval,
//        *m_enable_asynchronous_rendering
    };

    bool m_use_rotation{true};

    friend class vrmod::D3D11Component;
    friend class vrmod::D3D12Component;
    friend class vrmod::OverlayComponent;
public:
};
