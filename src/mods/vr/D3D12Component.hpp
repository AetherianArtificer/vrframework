#pragma once

#include <array>
#include <optional>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <mutex>
#include <wrl.h>

#include <../../../_deps/directxtk12-src/Inc/GraphicsMemory.h>
#include <../../../_deps/directxtk12-src/Inc/SpriteBatch.h>
#include <../../../_deps/directxtk12-src/Inc/DescriptorHeap.h>

#include "mods/vr/d3d12/CommandContext.hpp"

#include "mods/vr/d3d12/ResourceCopier.hpp"
#include "mods/vr/d3d12/TextureContext.hpp"

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <openvr.h>

class VR;

namespace vrmod {
class D3D12Component {
public:
    vr::EVRCompositorError on_frame(VR* vr);
    void on_post_present(VR* vr);

    void on_reset(VR* vr);

    void force_reset() { m_force_reset = true; }

    const auto& get_backbuffer_size() const { return m_backbuffer_size; }

    auto is_initialized() const { return m_initialized; }

    auto& openxr() { return m_openxr; }

private:
    void setup();
    void prepare_comfort_textures(VR* vr);
    void copy_native_stereo_eyes(VR* vr, ID3D12Resource* backbuffer);
    bool setup_native_stereo_textures(ID3D12Resource* backbuffer, VR* vr);
    void dump_backbuffer(VR* vr, ID3D12Resource* backbuffer);

    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D12Resource> m_prev_backbuffer{};
    // Native stereo: a shader-readable copy of the back buffer, and each eye scaled to its swapchain size.
    d3d12::TextureContext m_native_source{};
    // Copies each half without blending; menus leave the back buffer partly transparent.
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_native_copy_batch{};
    DXGI_FORMAT m_native_copy_format{ DXGI_FORMAT_UNKNOWN };
    // Shader views of the eye images the game hands over in full-frame native stereo.
    std::array<d3d12::TextureContext, 2> m_native_capture{};
    std::array<ID3D12Resource*, 2> m_native_capture_source{};
    std::wstring m_eye_dump_path{};
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_native_ui_batch{};
    d3d12::TextureContext m_native_ui{};
    ID3D12Resource* m_native_ui_resource{};
    // The HUD for the world panel, drawn at the panel swapchain's size.
    d3d12::TextureContext m_native_hud_target{};
    d3d12::TextureContext m_native_wrist_target{};
    bool m_native_wrist_ready{false};
    bool m_native_hud_ready{false};
    std::array<d3d12::TextureContext, 2> m_native_eye{};
    std::array<d3d12::ResourceCopier, 3> m_generic_copiers{};

    std::unique_ptr<DirectX::DX12::GraphicsMemory> m_graphics_memory{};

    ComPtr<ID3D12Resource>                      m_vignette_texture{};
    ComPtr<ID3D12Resource>                      m_fade_texture{};
    std::unique_ptr<DirectX::DescriptorHeap>    m_vignette_srv_heap{};

    bool m_initialized{false};

    struct OpenXR {
        void initialize(XrSessionCreateInfo& session_info);
        std::optional<std::string> create_swapchains();
        void destroy_swapchains();
        void copy(uint32_t swapchain_idx, ID3D12Resource* src, D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT, D3D12_BOX* src_box = nullptr);
        void wait_for_all_copies() {
            std::scoped_lock _{this->mtx};

            for (auto& ctx : this->contexts) {
                if(ctx.swapchain_index < 0) {
                    continue;
                }
                for (auto& texture_ctx : ctx.texture_contexts) {
                    texture_ctx->commands.wait(INFINITE);
                }
            }
        }

        bool ever_acquired(uint32_t swapchain_idx) {
            std::scoped_lock _{this->mtx};
            for(auto& ctx : contexts) {
                if (ctx.swapchain_index == swapchain_idx) {
                    return ctx.ever_acquired;
                }
            }
            return false;
        }

        XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};

        struct SwapchainContext {
            std::vector<XrSwapchainImageD3D12KHR> textures{};
            std::vector<std::unique_ptr<d3d12::TextureContext>> texture_contexts{};
            uint32_t num_textures_acquired{0};
            int swapchain_index{-1};
            bool ever_acquired{false};
        };

        std::vector<SwapchainContext> contexts{};
        std::recursive_mutex mtx{};
        std::array<uint32_t, 2> last_resolution{};
    } m_openxr;

    uint32_t m_backbuffer_size[2]{};
    bool m_force_reset{false};
    bool m_crop_copy{false};
};
} // namespace vrmod