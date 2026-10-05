#include <openvr.h>
#include <utility/ScopeGuard.hpp>
#include <aer/ConstantsPool.h>

#include "mods/VR.hpp"

#include <../../../_deps/directxtk12-src/Inc/RenderTargetState.h>
#include <../../../_deps/directxtk12-src/Inc/ResourceUploadBatch.h>
#include <../../../_deps/directxtk12-src/Inc/CommonStates.h>

#include "D3D12Component.hpp"
#include <../../../_deps/directxtk12-src/Src/d3dx12.h>
#include <d3dcompiler.h>
#include <../../../_deps/directxtk12-src/Inc/ScreenGrab.h>
#include <utility/String.hpp>
#include <ModSettings.h>
#include <wincodec.h>

typedef HRESULT(WINAPI* PFN_D3D12_GET_DEBUG_INTERFACE)(REFIID, void**);

namespace vrmod {
vr::EVRCompositorError D3D12Component::on_frame(VR* vr) {
    if (!m_initialized || m_force_reset) {
        setup();
    }

    auto& hook = g_framework->get_d3d12_hook();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};
    const auto backbuffer_index = swapchain->GetCurrentBackBufferIndex();
    if (FAILED(swapchain->GetBuffer(backbuffer_index, IID_PPV_ARGS(&backbuffer))) || backbuffer == nullptr) {
        spdlog::error("[VR] Failed to get back buffer");
        return vr::VRCompositorError_None;
    }

    if (vr->is_hmd_active()) {
        prepare_comfort_textures(vr);
    }

    auto runtime = vr->get_runtime();
    if (!runtime->is_openxr()) {
        m_prev_backbuffer = backbuffer;
        return vr::VRCompositorError_None;
    }

    if (runtime->ready() && vr->m_openxr->frame_began) {
        auto fw_rt = g_framework->get_rendertarget_d3d12();
        if (fw_rt && g_framework->is_drawing_ui()) {
            m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, fw_rt.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }

    dump_backbuffer(vr, backbuffer.Get());
    if (vr->m_openxr->ready()) {
        copy_native_stereo_eyes(vr, backbuffer.Get());
    }

    if (runtime->ready() && runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE) {
        runtime->synchronize_frame(vr->m_presenter_frame_count);

        if (!runtime->got_first_poses) {
            runtime->update_poses(vr->m_presenter_frame_count + 1);
        }
    }

    if (vr->m_openxr->ready()) {
        if (runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE || !vr->m_openxr->frame_began) {
            vr->m_openxr->begin_frame(vr->m_presenter_frame_count);
        }

        std::vector<XrCompositionLayerBaseHeader*> quad_layers{};
        auto& openxr_overlay = vr->get_overlay_component().get_openxr();

        if (m_native_hud_ready && !ModSettings::showFlatScreenDisplay() && m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::GAME_UI)) {
            if (const auto hud_quad = openxr_overlay.generate_game_ui_quad()) {
                quad_layers.push_back((XrCompositionLayerBaseHeader*)&hud_quad->get());
            }
        }

        if (m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI)) {
            if (const auto framework_quad = openxr_overlay.generate_framework_ui_quad()) {
                quad_layers.push_back((XrCompositionLayerBaseHeader*)&framework_quad->get());
            }
        }

        auto result = vr->m_openxr->end_frame(quad_layers, vr->m_presenter_frame_count);
        if (result == XR_ERROR_LAYER_INVALID) {
            spdlog::info("[VR] Attempting to correct invalid layer");
            m_openxr.wait_for_all_copies();
            spdlog::info("[VR] Calling xrEndFrame again");
            result = vr->m_openxr->end_frame(quad_layers, vr->m_presenter_frame_count);
        }

        vr->m_openxr->needs_pose_update = true;
        vr->m_submitted = result == XR_SUCCESS;
    }

    // Allows the desktop window to be recorded.
    if (vr->m_desktop_fix->value() && runtime->ready() && m_prev_backbuffer != backbuffer && m_prev_backbuffer != nullptr) {
        auto& copier = m_generic_copiers[vr->m_presenter_frame_count % m_generic_copiers.size()];
        copier.wait(INFINITE);
        copier.copy(m_prev_backbuffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
        copier.execute();
    }

    m_prev_backbuffer = backbuffer;
    return vr::VRCompositorError_None;
}

void D3D12Component::on_post_present(VR* vr) {
    if (m_graphics_memory != nullptr) {
        auto& hook = g_framework->get_d3d12_hook();
        auto command_queue = hook->get_command_queue();

        m_graphics_memory->Commit(command_queue);
    }
}

void D3D12Component::on_reset(VR* vr) {
    auto runtime = vr->get_runtime();

    for (auto& copier : m_generic_copiers) {
        copier.reset();
    }
    
    m_prev_backbuffer.Reset();
    m_graphics_memory.reset();
    m_initialized = false;

    if (runtime->is_openxr() && runtime->loaded) {
        if (m_openxr.last_resolution[0] != vr->get_hmd_width() || m_openxr.last_resolution[1] != vr->get_hmd_height()) {
            m_openxr.create_swapchains();
        }

        // end the frame before something terrible happens
        //vr->m_openxr.synchronize_frame();
        //vr->m_openxr.begin_frame();
        //vr->m_openxr.end_frame();
    }
}

void D3D12Component::setup() {
    spdlog::info("[VR] Setting up d3d12 textures...");

    m_prev_backbuffer.Reset();

    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer.");
        return;
    }

    if (m_graphics_memory == nullptr) {
        m_graphics_memory = std::make_unique<DirectX::DX12::GraphicsMemory>(device);
    }

    for (auto& copier : m_generic_copiers) {
        copier.setup();
    }

    const auto backbuffer_desc = backbuffer->GetDesc();
    m_backbuffer_size[0] = (uint32_t)backbuffer_desc.Width;
    m_backbuffer_size[1] = backbuffer_desc.Height;
    spdlog::info("[VR] D3D12 Backbuffer width: {}, height: {}", backbuffer_desc.Width, backbuffer_desc.Height);

    m_initialized = true;
    m_force_reset = false;
}

bool D3D12Component::setup_native_stereo_textures(ID3D12Resource* backbuffer, VR* vr) {
    auto device = g_framework->get_d3d12_hook()->get_device();
    const auto bb_desc = backbuffer->GetDesc();

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    if (m_native_source.texture == nullptr || m_native_source.texture->GetDesc().Width != bb_desc.Width ||
        m_native_source.texture->GetDesc().Height != bb_desc.Height || m_native_source.texture->GetDesc().Format != bb_desc.Format) {
        auto desc = bb_desc;
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        ComPtr<ID3D12Resource> texture{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&texture)))) {
            spdlog::error("[VR] Failed to create the native stereo source texture");
            return false;
        }
        texture->SetName(L"Native stereo source");
        if (!m_native_source.setup(device, texture.Get(), std::nullopt, std::nullopt, L"Native stereo source")) {
            return false;
        }
    }

    for (uint32_t eye = 0; eye < 2; ++eye) {
        const auto& swapchain = vr->m_openxr->swapchains[eye];
        auto& ctx = m_native_eye[eye];
        if (ctx.texture != nullptr && ctx.texture->GetDesc().Width == (UINT64)swapchain.width && ctx.texture->GetDesc().Height == (UINT)swapchain.height) {
            continue;
        }
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, swapchain.width, swapchain.height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        ComPtr<ID3D12Resource> texture{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&texture)))) {
            spdlog::error("[VR] Failed to create the native stereo eye texture");
            return false;
        }
        texture->SetName(eye == 0 ? L"Native stereo left eye" : L"Native stereo right eye");
        if (!ctx.setup(device, texture.Get(), std::nullopt, std::nullopt, L"Native stereo eye")) {
            return false;
        }
        spdlog::info("[VR] Native stereo eye {} texture {}x{} from back buffer {}x{}", eye, swapchain.width, swapchain.height, bb_desc.Width, bb_desc.Height);
    }
    return true;
}

void D3D12Component::copy_native_stereo_eyes(VR* vr, ID3D12Resource* backbuffer) {
    if (!setup_native_stereo_textures(backbuffer, vr)) {
        return;
    }

    const auto desc = backbuffer->GetDesc();
    const bool mono = vr->is_native_mono_frame();

    const auto eye_format = m_native_eye[0].texture->GetDesc().Format;
    if (m_native_copy_batch == nullptr || m_native_copy_format != eye_format) {
        auto device = g_framework->get_d3d12_hook()->get_device();
        DirectX::ResourceUploadBatch upload{ device };
        upload.Begin();
        DirectX::RenderTargetState output_state{ eye_format, DXGI_FORMAT_UNKNOWN };
        DirectX::SpriteBatchPipelineStateDescription pd{ output_state, &DirectX::DX12::CommonStates::Opaque };
        m_native_copy_batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, pd);
        DirectX::SpriteBatchPipelineStateDescription ui_pd{ output_state, &DirectX::DX12::CommonStates::AlphaBlend };
        m_native_ui_batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, ui_pd);
        upload.End(g_framework->get_d3d12_hook()->get_command_queue()).wait();
        m_native_copy_format = eye_format;
    }

    auto& commands = m_native_source.commands;
    commands.wait(INFINITE);
    commands.copy(backbuffer, m_native_source.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);

    // Each eye shows its own captured image, or the whole back buffer when it has none.
    std::array<bool, 2> captured{};
    if (!mono) {
        auto device = g_framework->get_d3d12_hook()->get_device();
        for (uint32_t eye = 0; eye < 2; ++eye) {
            auto source = vr->get_native_eye_source(eye);
            if (source == nullptr) {
                continue;
            }
            if (m_native_capture_source[eye] != source.Get() || m_native_capture[eye].texture == nullptr) {
                m_native_capture[eye].reset();
                const auto format = source->GetDesc().Format;
                DXGI_FORMAT typed = format;
                switch (format) {
                case DXGI_FORMAT_R8G8B8A8_TYPELESS: typed = DXGI_FORMAT_R8G8B8A8_UNORM; break;
                case DXGI_FORMAT_B8G8R8A8_TYPELESS: typed = DXGI_FORMAT_B8G8R8A8_UNORM; break;
                case DXGI_FORMAT_R10G10B10A2_TYPELESS: typed = DXGI_FORMAT_R10G10B10A2_UNORM; break;
                case DXGI_FORMAT_R16G16B16A16_TYPELESS: typed = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
                default: break;
                }
                if (!m_native_capture[eye].setup(device, source.Get(), typed, typed, L"Native stereo eye capture")) {
                    spdlog::error("[VR] Native stereo eye {} capture has no shader view (format {})", eye, (uint32_t)format);
                    m_native_capture[eye].reset();
                    m_native_capture_source[eye] = nullptr;
                    continue;
                }
                m_native_capture_source[eye] = source.Get();
                spdlog::info("[VR] Native stereo eye {} shows its captured image {}x{} format {}", eye, source->GetDesc().Width, source->GetDesc().Height, (uint32_t)format);
            }
            captured[eye] = true;
        }
    }

    bool ui_ready = false;
    if (!mono && (captured[0] || captured[1])) {
        if (auto ui = vr->get_native_ui_source(); ui != nullptr) {
            if (m_native_ui_resource != ui.Get() || m_native_ui.texture == nullptr) {
                m_native_ui.reset();
                m_native_ui_resource = nullptr;
                const auto format = ui->GetDesc().Format;
                const auto typed = format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM
                                 : format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM : format;
                if (m_native_ui.setup(g_framework->get_d3d12_hook()->get_device(), ui.Get(), typed, typed, L"Native stereo UI layer")) {
                    m_native_ui_resource = ui.Get();
                    spdlog::info("[VR] Native stereo UI layer {}x{} format {} drawn over both eyes", ui->GetDesc().Width, ui->GetDesc().Height, (uint32_t)format);
                } else {
                    m_native_ui.reset();
                }
            }
            ui_ready = m_native_ui_resource != nullptr;
        }
    }

    auto command_list = commands.cmd_list.Get();
    for (uint32_t eye = 0; eye < 2; ++eye) {
        auto& dst = m_native_eye[eye];
        const auto dst_desc = dst.texture->GetDesc();
        const RECT source{ 0, 0, (LONG)desc.Width, (LONG)desc.Height };

        D3D12_RESOURCE_BARRIER barriers[]{
            CD3DX12_RESOURCE_BARRIER::Transition(dst.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(m_native_source.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        command_list->ResourceBarrier(2, barriers);

        D3D12_VIEWPORT viewport{ 0.0f, 0.0f, (float)dst_desc.Width, (float)dst_desc.Height, D3D12_MIN_DEPTH, D3D12_MAX_DEPTH };
        D3D12_RECT scissor{ 0, 0, (LONG)dst_desc.Width, (LONG)dst_desc.Height };
        const auto rtv = dst.get_rtv();
        command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        command_list->RSSetViewports(1, &viewport);
        command_list->RSSetScissorRects(1, &scissor);
        auto& view = captured[eye] ? m_native_capture[eye] : m_native_source;
        ID3D12DescriptorHeap* heaps[]{ view.srv_heap->Heap() };
        command_list->SetDescriptorHeaps(1, heaps);

        m_native_copy_batch->SetViewport(viewport);
        m_native_copy_batch->Begin(command_list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);
        const RECT dest{ 0, 0, (LONG)dst_desc.Width, (LONG)dst_desc.Height };
        if (captured[eye]) {
            // Within a few pixels of the eye's size the image is copied pixel for pixel; resampling it blurs the whole eye.
            const auto capture_desc = m_native_capture[eye].texture->GetDesc();
            const LONG capture_width = (LONG)capture_desc.Width;
            const LONG capture_height = (LONG)capture_desc.Height;
            const bool same_size = std::abs(capture_width - dest.right) <= 16 && std::abs(capture_height - dest.bottom) <= 16;
            const RECT exact{ 0, 0, std::min(capture_width, dest.right), std::min(capture_height, dest.bottom) };
            if (same_size) {
                m_native_copy_batch->Draw(view.get_srv_gpu(), DirectX::XMUINT2{ (uint32_t)capture_width, (uint32_t)capture_height }, exact, &exact, DirectX::Colors::White);
            } else {
                m_native_copy_batch->Draw(view.get_srv_gpu(), DirectX::XMUINT2{ (uint32_t)capture_width, (uint32_t)capture_height }, dest, DirectX::Colors::White);
            }
        } else {
            m_native_copy_batch->Draw(view.get_srv_gpu(), DirectX::XMUINT2{ (uint32_t)desc.Width, (uint32_t)desc.Height }, dest, &source, DirectX::Colors::White);
        }
        m_native_copy_batch->End();

        // Comfort vignette and fade, drawn here because the eyes no longer show the back buffer.
        const float vignette = vr->get_comfort_vignette();
        const float fade = vr->get_comfort_fade();
        const bool comfort = m_vignette_srv_heap != nullptr && (vignette > 0.01f || fade > 0.01f);

        // The UI layer, shifted toward the nose in each eye so the HUD sits a couple of metres away instead of at infinity.
        if (ui_ready && captured[eye] && !vr->is_native_hud_panel()) {
            const auto ui_desc = m_native_ui.texture->GetDesc();
            const LONG shift = (LONG)(dest.right * 0.006f) * (eye == 0 ? 1 : -1);
            const RECT ui_dest{ dest.left + shift, dest.top, dest.right + shift, dest.bottom };
            ID3D12DescriptorHeap* ui_heaps[]{ m_native_ui.srv_heap->Heap() };
            command_list->SetDescriptorHeaps(1, ui_heaps);
            m_native_ui_batch->SetViewport(viewport);
            m_native_ui_batch->Begin(command_list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);
            m_native_ui_batch->Draw(m_native_ui.get_srv_gpu(), DirectX::XMUINT2{ (uint32_t)ui_desc.Width, (uint32_t)ui_desc.Height }, ui_dest, DirectX::Colors::White);
            m_native_ui_batch->End();
        }
        if (comfort) {
            ID3D12DescriptorHeap* comfort_heaps[]{ m_vignette_srv_heap->Heap() };
            command_list->SetDescriptorHeaps(1, comfort_heaps);
            m_native_ui_batch->SetViewport(viewport);
            m_native_ui_batch->Begin(command_list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);
            if (vignette > 0.01f) {
                const float scale = 2.0f - std::clamp(vignette, 0.0f, 1.0f);
                const float alpha = std::clamp(vignette / 0.3f, 0.0f, 1.0f);
                const float w = (float)dest.right;
                const float h = (float)dest.bottom;
                const RECT ring{ (LONG)(w * 0.5f * (1.0f - scale)), (LONG)(h * 0.5f * (1.0f - scale)), (LONG)(w * 0.5f * (1.0f + scale)), (LONG)(h * 0.5f * (1.0f + scale)) };
                m_native_ui_batch->Draw(m_vignette_srv_heap->GetGpuHandle(0), DirectX::XMUINT2{ 256, 256 }, ring, DirectX::XMVECTORF32{ { { 1.0f, 1.0f, 1.0f, alpha } } });
            }
            if (fade > 0.01f) {
                const float a = std::clamp(fade, 0.0f, 1.0f);
                m_native_ui_batch->Draw(m_vignette_srv_heap->GetGpuHandle(1), DirectX::XMUINT2{ 1, 1 }, dest, DirectX::XMVECTORF32{ { { 1.0f, 1.0f, 1.0f, a } } });
            }
            m_native_ui_batch->End();
        }

        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        command_list->ResourceBarrier(2, barriers);
    }
    m_native_hud_ready = false;
    if (ui_ready && vr->is_native_hud_panel()) {
        const auto& panel_swapchain = vr->m_openxr->swapchains[(uint32_t)runtimes::OpenXR::SwapchainIndex::GAME_UI];
        auto& target = m_native_hud_target;
        if (panel_swapchain.width > 0 && (target.texture == nullptr || target.texture->GetDesc().Width != (UINT64)panel_swapchain.width ||
                                          target.texture->GetDesc().Height != (UINT)panel_swapchain.height)) {
            auto device = g_framework->get_d3d12_hook()->get_device();
            const CD3DX12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, panel_swapchain.width, panel_swapchain.height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            ComPtr<ID3D12Resource> texture{};
            if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&texture)))) {
                texture->SetName(L"Native stereo HUD panel");
                if (!target.setup(device, texture.Get(), std::nullopt, std::nullopt, L"Native stereo HUD panel")) {
                    target.reset();
                }
            }
        }
        if (target.texture != nullptr) {
            const auto target_desc = target.texture->GetDesc();
            const auto ui_desc = m_native_ui.texture->GetDesc();
            const auto to_rt = CD3DX12_RESOURCE_BARRIER::Transition(target.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            command_list->ResourceBarrier(1, &to_rt);
            const auto rtv = target.get_rtv();
            const float clear[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
            command_list->ClearRenderTargetView(rtv, clear, 0, nullptr);
            D3D12_VIEWPORT viewport{ 0.0f, 0.0f, (float)target_desc.Width, (float)target_desc.Height, D3D12_MIN_DEPTH, D3D12_MAX_DEPTH };
            D3D12_RECT scissor{ 0, 0, (LONG)target_desc.Width, (LONG)target_desc.Height };
            command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            command_list->RSSetViewports(1, &viewport);
            command_list->RSSetScissorRects(1, &scissor);
            ID3D12DescriptorHeap* ui_heaps[]{ m_native_ui.srv_heap->Heap() };
            command_list->SetDescriptorHeaps(1, ui_heaps);
            m_native_copy_batch->SetViewport(viewport);
            m_native_copy_batch->Begin(command_list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);
            const RECT dest{ 0, 0, (LONG)target_desc.Width, (LONG)target_desc.Height };
            m_native_copy_batch->Draw(m_native_ui.get_srv_gpu(), DirectX::XMUINT2{ (uint32_t)ui_desc.Width, (uint32_t)ui_desc.Height }, dest, DirectX::Colors::White);
            m_native_copy_batch->End();
            const auto to_srv = CD3DX12_RESOURCE_BARRIER::Transition(target.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            command_list->ResourceBarrier(1, &to_srv);
            m_native_hud_ready = true;
        }
    }

    commands.execute();

    for (uint32_t eye = 0; eye < 2; ++eye) {
        m_openxr.copy(eye, m_native_eye[eye].texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    if (m_native_hud_ready) {
        m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::GAME_UI, m_native_hud_target.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // The images each eye of the headset receives, next to the back buffer capture.
    if (!m_eye_dump_path.empty()) {
        auto command_queue = g_framework->get_d3d12_hook()->get_command_queue();
        for (uint32_t eye = 0; eye < 2; ++eye) {
            auto path = m_eye_dump_path;
            const auto dot = path.rfind(L'.');
            path.insert(dot == std::wstring::npos ? path.size() : dot, eye == 0 ? L"_hmd_left" : L"_hmd_right");
            const auto hr = DirectX::SaveWICTextureToFile(command_queue, m_native_eye[eye].texture.Get(), GUID_ContainerFormatPng, path.c_str(),
                                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            spdlog::info("[VR] {} eye image saved to {} (hr {:x}, {})", eye == 0 ? "Left" : "Right", utility::narrow(path), (uint32_t)hr,
                         captured[eye] ? "captured eye image" : "back buffer");
        }
        m_eye_dump_path.clear();
    }
}

void D3D12Component::dump_backbuffer(VR* vr, ID3D12Resource* backbuffer) {
    auto path = vr->take_backbuffer_dump_request();
    if (path.empty()) {
        return;
    }
    auto command_queue = g_framework->get_d3d12_hook()->get_command_queue();
    const auto hr = DirectX::SaveWICTextureToFile(command_queue, backbuffer, GUID_ContainerFormatPng, path.c_str(), D3D12_RESOURCE_STATE_PRESENT,
                                                  D3D12_RESOURCE_STATE_PRESENT);
    const auto desc = backbuffer->GetDesc();
    spdlog::info("[VR] Back buffer {}x{} saved to {} (hr {:x})", desc.Width, desc.Height, utility::narrow(path), (uint32_t)hr);
    m_eye_dump_path = path;
}

void D3D12Component::prepare_comfort_textures(VR* vr) {
    const float strength = vr->get_comfort_vignette();
    const float fade = vr->get_comfort_fade();
    if (strength <= 0.01f && fade <= 0.01f) {
        return;
    }

    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();

    if (m_vignette_texture == nullptr) {
        constexpr UINT kSize = 256;
        constexpr float kInner = 0.35f;

        // Premultiplied black with alpha rising from kInner to the edge.
        std::vector<uint32_t> pixels(kSize * kSize);
        for (UINT y = 0; y < kSize; ++y) {
            for (UINT x = 0; x < kSize; ++x) {
                const float dx = (x + 0.5f) / kSize * 2.0f - 1.0f;
                const float dy = (y + 0.5f) / kSize * 2.0f - 1.0f;
                const float t = std::clamp((std::sqrt(dx * dx + dy * dy) - kInner) / (1.0f - kInner), 0.0f, 1.0f);
                const auto alpha = static_cast<uint32_t>(t * t * (3.0f - 2.0f * t) * 255.0f);
                pixels[y * kSize + x] = alpha << 24;
            }
        }

        const auto heap_props = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto tex_desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, 1);
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_vignette_texture)))) {
            spdlog::error("[VR] Failed to create vignette texture");
            return;
        }

        const auto fade_desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1);
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &fade_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_fade_texture)))) {
            spdlog::error("[VR] Failed to create fade texture");
            m_vignette_texture.Reset();
            return;
        }
        const uint32_t opaque_black = 0xFF000000;

        DirectX::ResourceUploadBatch upload{ device };
        upload.Begin();
        D3D12_SUBRESOURCE_DATA data{ pixels.data(), (LONG_PTR)(kSize * 4), (LONG_PTR)(kSize * kSize * 4) };
        upload.Upload(m_vignette_texture.Get(), 0, &data, 1);
        upload.Transition(m_vignette_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        D3D12_SUBRESOURCE_DATA fade_data{ &opaque_black, 4, 4 };
        upload.Upload(m_fade_texture.Get(), 0, &fade_data, 1);
        upload.Transition(m_fade_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        upload.End(command_queue).wait();

        m_vignette_srv_heap = std::make_unique<DirectX::DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 2);
        device->CreateShaderResourceView(m_vignette_texture.Get(), nullptr, m_vignette_srv_heap->GetCpuHandle(0));
        device->CreateShaderResourceView(m_fade_texture.Get(), nullptr, m_vignette_srv_heap->GetCpuHandle(1));
    }
}


void D3D12Component::OpenXR::initialize(XrSessionCreateInfo& session_info) {
    std::scoped_lock _{this->mtx};

	auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();

    this->binding.device = device;
    this->binding.queue = command_queue;

    spdlog::info("[VR] Searching for xrGetD3D12GraphicsRequirementsKHR...");
    PFN_xrGetD3D12GraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(VR::get()->m_openxr->instance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)(&fn));

    XrGraphicsRequirementsD3D12KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    gr.adapterLuid = device->GetAdapterLuid();
    gr.minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    spdlog::info("[VR] Calling xrGetD3D12GraphicsRequirementsKHR");
    fn(VR::get()->m_openxr->instance, VR::get()->m_openxr->system, &gr);

    session_info.next = &this->binding;
}

std::optional<std::string> D3D12Component::OpenXR::create_swapchains() {
    std::scoped_lock _{this->mtx};

    spdlog::info("[VR] Creating OpenXR swapchains for D3D12");

    this->destroy_swapchains();
    
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto  lDxgiSwapChain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    // Get the existing backbuffer
    // so we can get the format and stuff.
    if (FAILED(lDxgiSwapChain->GetBuffer(lDxgiSwapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer.");
        return "Failed to get back buffer.";
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    auto backbuffer_desc = backbuffer->GetDesc();
    auto& vr = VR::get();
    auto& openxr = vr->m_openxr;

    this->contexts.clear();
    this->contexts.resize((size_t)runtimes::OpenXR::SwapchainIndex::END);

    backbuffer_desc.Width = vr->get_hmd_width();
    backbuffer_desc.Height = vr->get_hmd_height();

    auto create_swapchain = [&](uint32_t swapchainIndex, int format, int width, int height) -> std::optional<std::string> {
        spdlog::info("[VR] Creating swapchain for eye {}", swapchainIndex);
        spdlog::info("[VR] Width: {}", width);
        spdlog::info("[VR] Height: {}", height);
        spdlog::info("[VR] Backbuffer Format: {}", format);

        // Create the swapchain.
        XrSwapchainCreateInfo swapchain_create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchain_create_info.arraySize = 1;
        swapchain_create_info.format = format;
        swapchain_create_info.width = width;
        swapchain_create_info.height = height;
        swapchain_create_info.mipCount = 1;
        swapchain_create_info.faceCount = 1;
        swapchain_create_info.sampleCount = backbuffer_desc.SampleDesc.Count;
        swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

        auto& swapchain = vr->m_openxr->swapchains[swapchainIndex];
        swapchain.width = swapchain_create_info.width;
        swapchain.height = swapchain_create_info.height;

        if (xrCreateSwapchain(openxr->session, &swapchain_create_info, &swapchain.handle) != XR_SUCCESS) {
            spdlog::error("[VR] D3D12: Failed to create swapchain.");
            return "Failed to create swapchain.";
        }


        uint32_t image_count{};
        auto result = xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count, nullptr);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images.");
            return "Failed to enumerate swapchain images.";
        }

        spdlog::info("[VR] Runtime wants {} images for swapchain {}", image_count, swapchainIndex);

        auto& ctx = this->contexts[swapchainIndex];

        ctx.textures.clear();
        ctx.textures.resize(image_count);
        ctx.texture_contexts.clear();
        ctx.texture_contexts.resize(image_count);
        ctx.swapchain_index = swapchainIndex;

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            ctx.texture_contexts[j] = std::make_unique<d3d12::TextureContext>();
            ctx.texture_contexts[j]->commands.setup((std::wstring{L"OpenXR Commands "} + std::to_wstring(swapchainIndex) + L" " + std::to_wstring(j)).c_str());
        }

        result = xrEnumerateSwapchainImages(swapchain.handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)&ctx.textures[0]);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images after texture creation.");
            return "Failed to enumerate swapchain images after texture creation.";
        }

        /*for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j].texture->AddRef();
            const auto ref_count = ctx.textures[j].texture->Release();

            spdlog::info("[VR] AFTER Swapchain texture {} {} ref count: {}", i, j, ref_count);
        }*/
        return std::nullopt;
    };


    if(auto err = create_swapchain((int)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, (int)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, backbuffer_desc.Width, backbuffer_desc.Height)) {
        return err;
    }
    if(auto err = create_swapchain((int)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, (int)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, backbuffer_desc.Width, backbuffer_desc.Height)) {
        return err;
    }
    if(auto err = create_swapchain((int)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, (int)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, backbuffer_desc.Width, backbuffer_desc.Height)) {
        return err;
    }
    if(auto err = create_swapchain((int)runtimes::OpenXR::SwapchainIndex::GAME_UI, (int)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, backbuffer_desc.Width, backbuffer_desc.Height)) {
        return err;
    }
    this->last_resolution = {vr->get_hmd_width(), vr->get_hmd_height()};

    return std::nullopt;
}

void D3D12Component::OpenXR::destroy_swapchains() {
    std::scoped_lock _{this->mtx};

	if (this->contexts.empty()) {
        return;
    }

    spdlog::info("[VR] Destroying swapchains.");

    for (auto i = 0; i < this->contexts.size(); ++i) {
        auto& ctx = this->contexts[i];
        if(ctx.swapchain_index < 0) {
            continue;
        }
        ctx.texture_contexts.clear();

        auto result = xrDestroySwapchain(VR::get()->m_openxr->swapchains[i].handle);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to destroy swapchain {}.", i);
        } else {
            spdlog::info("[VR] Destroyed swapchain {}.", i);
        }

        ctx.textures.clear();
        VR::get()->m_openxr->swapchains[i] = {};
    }

    this->contexts.clear();
}

void D3D12Component::OpenXR::copy(uint32_t swapchain_idx, ID3D12Resource* resource, D3D12_RESOURCE_STATES src_state,
    D3D12_BOX* src_box) {
    std::scoped_lock _{this->mtx};

    auto& vr = VR::get();

    if (!vr->m_openxr->should_render()) {
        return;
    }

    if (!vr->m_openxr->frame_began) {
        if (vr->m_openxr->get_synchronize_stage() != VRRuntime::SynchronizeStage::VERY_LATE) {
            spdlog::error("[VR] OpenXR: Frame not begun when trying to copy.");
            return;
        }
    }

    if (this->contexts[swapchain_idx].num_textures_acquired > 0) {
        spdlog::info("[VR] Already acquired textures for swapchain {}?", swapchain_idx);
    }

    const auto& swapchain = vr->m_openxr->swapchains[swapchain_idx];
    auto& ctx = this->contexts[swapchain_idx];

    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

    uint32_t texture_index{};
    auto result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);

    if (result == XR_ERROR_RUNTIME_FAILURE) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        spdlog::info("[VR] Attempting to correct...");

        for (auto& texture_ctx : ctx.texture_contexts) {
            texture_ctx->commands.reset();
        }

        texture_index = 0;
        result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);
    }


    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
    } else {
        ctx.num_textures_acquired++;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        //wait_info.timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)).count();
        wait_info.timeout = XR_INFINITE_DURATION;
        result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        } else {
            auto& texture_ctx = ctx.texture_contexts[texture_index];
            texture_ctx->commands.wait(INFINITE);

            if (src_box == nullptr) {
                const auto is_depth = false;/*swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH ||
                                      swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE ||
                                      swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE;*/
                const auto dst_state = is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;

                texture_ctx->commands.copy(
                    resource,
                    ctx.textures[texture_index].texture,
                    src_state,
                    dst_state);
            } else {
                UINT offsetX = 0;
                UINT offsetY = 0;
                const UINT box_width = src_box->right - src_box->left;
                const UINT box_height = src_box->bottom - src_box->top;
                if (box_width > (UINT)swapchain.width) {
                    src_box->left += (box_width - swapchain.width) >> 1;
                    src_box->right = src_box->left + swapchain.width;
                } else {
                    offsetX = (swapchain.width - box_width) >> 1;
                }

                if (box_height > (UINT)swapchain.height) {
                    src_box->top += (box_height - swapchain.height) >> 1;
                    src_box->bottom = src_box->top + swapchain.height;
                } else {
                    offsetY = (swapchain.height - box_height) >> 1;
                }
                texture_ctx->commands.copy_region(
                    resource,
                    ctx.textures[texture_index].texture, offsetX, offsetY, src_box,
                    src_state,
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
            }

            texture_ctx->commands.execute();

            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            auto result = xrReleaseSwapchainImage(swapchain.handle, &release_info);

            // SteamVR shenanigans.
            if (result == XR_ERROR_RUNTIME_FAILURE) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                spdlog::info("[VR] Attempting to correct...");

                result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

                if (result != XR_SUCCESS) {
                    spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                }

                for (auto& texture_ctx : ctx.texture_contexts) {
                    texture_ctx->commands.wait(INFINITE);
                }

                result = xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                return;
            }

            ctx.num_textures_acquired--;
            ctx.ever_acquired = true;

        }
    }
}
} // namespace vrmod
