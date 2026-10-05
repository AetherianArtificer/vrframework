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

    auto is_initialized() const { return m_openvr.left_eye_tex[0].texture != nullptr; }

    auto& openxr() { return m_openxr; }

private:
    void setup();
    void setup_sprite_batch_pso(DXGI_FORMAT output_format);
    void render_srv_to_rtv(ID3D12GraphicsCommandList* command_list, const d3d12::TextureContext& src, const d3d12::TextureContext& dst, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state);
    void draw_comfort_vignette(VR* vr, ID3D12Resource* backbuffer);
    void copy_native_stereo_eyes(VR* vr, ID3D12Resource* backbuffer);
    bool setup_native_stereo_textures(ID3D12Resource* backbuffer, VR* vr);
    void dump_backbuffer(VR* vr, ID3D12Resource* backbuffer);

    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D12Resource> m_prev_backbuffer{};
    d3d12::TextureContext m_backbuffer_copy{};
    d3d12::TextureContext m_converted_eye_tex{};
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
    std::array<d3d12::TextureContext, 2> m_native_eye{};
    std::array<d3d12::ResourceCopier, 3> m_generic_copiers{};

    std::unique_ptr<DirectX::DX12::GraphicsMemory> m_graphics_memory{};
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_sprite_batch{};

    d3d12::CommandContext                       m_vignette_commands{};
    ComPtr<ID3D12Resource>                      m_vignette_texture{};
    ComPtr<ID3D12Resource>                      m_fade_texture{};
    std::unique_ptr<DirectX::DescriptorHeap>    m_vignette_srv_heap{};
    std::unique_ptr<DirectX::DescriptorHeap>    m_vignette_rtv_heap{};
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_vignette_batch{};
    DXGI_FORMAT                                 m_vignette_format{ DXGI_FORMAT_UNKNOWN };

    // Mimicking what OpenXR does.
    struct OpenVR {
        d3d12::TextureContext& get_left(const bool past = false) {
            const auto counter = past ? (this->texture_counter - 1) : this->texture_counter;
            auto& ctx = this->left_eye_tex[counter % left_eye_tex.size()];
            return ctx;
        }

        d3d12::TextureContext& get_right(const bool past = false) {
            const auto counter = past ? (this->texture_counter - 1) : this->texture_counter;
            auto& ctx = this->right_eye_tex[counter % right_eye_tex.size()];
            return ctx;
        }

        d3d12::TextureContext& acquire_left() {
            auto& ctx = get_left();
            ctx.commands.wait(INFINITE);

            return ctx;
        }

        d3d12::TextureContext& acquire_right() {
            auto& ctx = get_right();
            ctx.commands.wait(INFINITE);

            return ctx;
        }

        void copy_left(ID3D12Resource* src) {
            auto& ctx = this->acquire_left();
            ctx.commands.copy(src, ctx.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ctx.commands.execute();
        }

        void copy_right(ID3D12Resource* src) {
            auto& ctx = this->acquire_right();
            ctx.commands.copy(src, ctx.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ctx.commands.execute();
        }

        std::array<d3d12::TextureContext, 3> left_eye_tex{};
        std::array<d3d12::TextureContext, 3> right_eye_tex{};
        uint32_t texture_counter{0};
    } m_openvr;

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
    bool m_backbuffer_is_8bit{false};
    bool m_force_reset{false};
    bool m_crop_copy{false};
};
} // namespace vrmod