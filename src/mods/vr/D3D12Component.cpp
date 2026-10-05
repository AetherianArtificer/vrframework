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
#include <wincodec.h>

typedef HRESULT(WINAPI* PFN_D3D12_GET_DEBUG_INTERFACE)(REFIID, void**);

namespace vrmod {
vr::EVRCompositorError D3D12Component::on_frame(VR* vr) {
    if (m_openvr.left_eye_tex[0].texture == nullptr || m_force_reset) {
        setup();
    }

    auto& hook = g_framework->get_d3d12_hook();
    
    // get device
    auto device = hook->get_device();

    // get command queue
    auto command_queue = hook->get_command_queue();

    // get swapchain
    auto swapchain = hook->get_swap_chain();

    // get back buffer
    ComPtr<ID3D12Resource> backbuffer{};

    const auto backbuffer_index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(backbuffer_index, IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer");
        return vr::VRCompositorError_None;
    }

    if (backbuffer == nullptr) {
        spdlog::error("[VR] Failed to get back buffer.");
        return vr::VRCompositorError_None;
    }

    if (vr->is_hmd_active()) {
        draw_comfort_vignette(vr, backbuffer.Get());
    }

    const bool native_stereo = vr->is_native_stereo() && vr->get_runtime()->is_openxr();

    if (!m_backbuffer_is_8bit) {
        auto command_list = m_backbuffer_copy.commands.cmd_list.Get();
        m_backbuffer_copy.commands.wait(INFINITE);

        // Copy current backbuffer into our copy so we can use it as an SRV.
        m_backbuffer_copy.commands.copy(backbuffer.Get(), m_backbuffer_copy.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);

        // Convert the backbuffer to 8-bit.
        render_srv_to_rtv(command_list, m_backbuffer_copy, m_converted_eye_tex, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        m_backbuffer_copy.commands.execute();
    }

    auto eye_texture = m_backbuffer_is_8bit ? backbuffer : m_converted_eye_tex.texture;

    auto runtime = vr->get_runtime();
    bool is_left_eye_frame = vr->m_presenter_frame_count % 2 == vr->m_left_eye_interval;
    bool is_right_eye_frame = !is_left_eye_frame;
    
    if (runtime->is_openxr() && runtime->ready() && vr->m_openxr->frame_began) {
        if (is_right_eye_frame || native_stereo) {
            auto fw_rt = g_framework->get_rendertarget_d3d12();

            if (fw_rt && g_framework->is_drawing_ui()) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, fw_rt.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        }
    }

    if (native_stereo) {
        dump_backbuffer(vr, backbuffer.Get());
        if (vr->m_openxr->ready()) {
            copy_native_stereo_eyes(vr, backbuffer.Get());
        }
    } else if (is_left_eye_frame) {
        // If m_frame_count is even, we're rendering the left eye.
        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
#ifdef OPENXR_FRAME_CROP_COPY
            D3D12_BOX src_box{};
            src_box.left = 0;
            src_box.right = m_backbuffer_size[0];
            src_box.top = 0;
            src_box.bottom = m_backbuffer_size[1];
            src_box.front = 0;
            src_box.back = 1;
            m_openxr.copy(0, eye_texture.Get(), D3D12_RESOURCE_STATE_PRESENT, &src_box);
#else
            m_openxr.copy(0, eye_texture.Get());
#endif
        }

        // OpenVR texture
        // Copy the back buffer to the left eye texture (m_left_eye_tex0 holds the intermediate frame).
        if (runtime->is_openvr()) {
            m_openvr.copy_left(eye_texture.Get());

            vr::D3D12TextureData_t left_tex {
                m_openvr.get_left().texture.Get(),
                command_queue,
                0
            };

            vr::VRTextureWithPose_t left_eye{};
            left_eye.handle = (void*)&left_tex;
            left_eye.eType = vr::TextureType_DirectX12;
            left_eye.eColorSpace = vr::ColorSpace_Auto;
            left_eye.mDeviceToAbsoluteTracking = GlobalPool::get_openvr_pose(vr->m_presenter_frame_count);

            const auto left_bounds = vr::VRTextureBounds_t{runtime->view_bounds[0][0], runtime->view_bounds[0][2],
                                                           runtime->view_bounds[0][1], runtime->view_bounds[0][3]};
            auto e = vr::VRCompositor()->Submit(vr::Eye_Left, (vr::Texture_t*)&left_eye, &left_bounds, vr::Submit_TextureWithPose);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                return e;
            }

            if (vr->is_using_async_aer()) {
                vr::D3D12TextureData_t right_tex {
                    m_openvr.get_right(true).texture.Get(),
                    command_queue,
                    0
                };

                vr::VRTextureWithPose_t right_eye{};
                right_eye.handle = (void*)&right_tex;
                right_eye.eType = vr::TextureType_DirectX12;
                right_eye.eColorSpace = vr::ColorSpace_Auto;
                right_eye.mDeviceToAbsoluteTracking = GlobalPool::get_openvr_pose(vr->m_presenter_frame_count-1);

                const auto right_bounds = vr::VRTextureBounds_t{runtime->view_bounds[1][0], runtime->view_bounds[1][2],
                                                                 runtime->view_bounds[1][1], runtime->view_bounds[1][3]};
                e = vr::VRCompositor()->Submit(vr::Eye_Right, (vr::Texture_t*)&right_eye, &right_bounds, vr::Submit_TextureWithPose);
                if (e != vr::VRCompositorError_None) {
                    spdlog::error("[VR] VRCompositor failed to submit right eye (resubmit): {}", (int)e);
                    return e;
                }
                vr->m_submitted = true;
            }
        }
    } else {
        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
#ifdef OPENXR_FRAME_CROP_COPY
                D3D12_BOX src_box{};
                src_box.left = 0;
                src_box.right = m_backbuffer_size[0];
                src_box.top = 0;
                src_box.bottom = m_backbuffer_size[1];
                src_box.front = 0;
                src_box.back = 1;
                m_openxr.copy(1, eye_texture.Get(), D3D12_RESOURCE_STATE_PRESENT, &src_box);
#else
                m_openxr.copy(1, eye_texture.Get());
#endif
        }

        // OpenVR texture
        // Copy the back buffer to the right eye texture.
        if (runtime->is_openvr()) {
            if (vr->is_using_async_aer()) {
                vr::D3D12TextureData_t left_tex {
                    m_openvr.get_left().texture.Get(),
                    command_queue,
                    0
                };

                vr::VRTextureWithPose_t left_eye{};
                left_eye.handle = (void*)&left_tex;
                left_eye.eType = vr::TextureType_DirectX12;
                left_eye.eColorSpace = vr::ColorSpace_Auto;
                left_eye.mDeviceToAbsoluteTracking = GlobalPool::get_openvr_pose(vr->m_presenter_frame_count - 1);

                const auto left_bounds = vr::VRTextureBounds_t{runtime->view_bounds[0][0], runtime->view_bounds[0][2],
                                                               runtime->view_bounds[0][1], runtime->view_bounds[0][3]};
                auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &left_bounds, vr::Submit_TextureWithPose); /*, vr::Submit_Default);*/

                if (e != vr::VRCompositorError_None) {
                    spdlog::error("[VR] VRCompositor failed to submit left eye (resubmit): {}", (int)e);
                    return e;
                }
            }
            m_openvr.copy_right(eye_texture.Get());

            vr::D3D12TextureData_t right_tex {
                m_openvr.get_right().texture.Get(),
                command_queue,
                0
            };

            vr::VRTextureWithPose_t right_eye{};
            right_eye.handle = (void*)&right_tex;
            right_eye.eType = vr::TextureType_DirectX12;
            right_eye.eColorSpace = vr::ColorSpace_Auto;
            right_eye.mDeviceToAbsoluteTracking = GlobalPool::get_openvr_pose(vr->m_presenter_frame_count);

            const auto right_bounds = vr::VRTextureBounds_t{runtime->view_bounds[1][0], runtime->view_bounds[1][2],
                                                             runtime->view_bounds[1][1], runtime->view_bounds[1][3]};
            auto e = vr::VRCompositor()->Submit(vr::Eye_Right, (vr::Texture_t*)&right_eye, &right_bounds, vr::Submit_TextureWithPose);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e);
                return e;
            }
            vr->m_submitted = true;

            ++m_openvr.texture_counter;
        }
    }

    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    if (is_right_eye_frame || vr->submits_every_frame()) {
        ////////////////////////////////////////////////////////////////////////////////
        // OpenXR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->ready() && runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE) {
            runtime->synchronize_frame(vr->m_presenter_frame_count);

            if (!runtime->got_first_poses) {
                runtime->update_poses(vr->m_presenter_frame_count + 1);
            }
        }

        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            if (runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE || !vr->m_openxr->frame_began) {
                vr->m_openxr->begin_frame(vr->m_presenter_frame_count);
            }

            std::vector<XrCompositionLayerBaseHeader*> quad_layers{};

            auto& openxr_overlay = vr->get_overlay_component().get_openxr();

            if (m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI)) {
                const auto framework_quad = openxr_overlay.generate_framework_ui_quad();
                if (framework_quad) {
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

        ////////////////////////////////////////////////////////////////////////////////
        // OpenVR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openvr()) {
            if (runtime->needs_pose_update) {
                vr->m_submitted = false;
                spdlog::info("[VR] Runtime needed pose update inside present (frame {})", vr->m_engine_frame_count);
                return vr::VRCompositorError_None;
            }

            //++m_openvr.texture_counter;
        }

        // Allows the desktop window to be recorded.
        if (vr->m_desktop_fix->value() && (is_right_eye_frame || native_stereo)) {
            if (runtime->ready() && m_prev_backbuffer != backbuffer && m_prev_backbuffer != nullptr) {
                auto& copier = m_generic_copiers[vr->m_presenter_frame_count % m_generic_copiers.size()];
                copier.wait(INFINITE);
                copier.copy(m_prev_backbuffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                copier.execute();
            }
        }
    }

    m_prev_backbuffer = backbuffer;

    return e;
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

    for (auto& ctx : m_openvr.left_eye_tex) {
        ctx.reset();
    }

    for (auto& ctx : m_openvr.right_eye_tex) {
        ctx.reset();
    }

    for (auto& copier : m_generic_copiers) {
        copier.reset();
    }
    
    m_prev_backbuffer.Reset();
    m_backbuffer_copy.reset();
    m_converted_eye_tex.reset();
    m_graphics_memory.reset();
    m_vignette_batch.reset();
    m_vignette_format = DXGI_FORMAT_UNKNOWN;

    if (runtime->is_openxr() && runtime->loaded) {
        if (m_openxr.last_resolution[0] != vr->get_hmd_width() || m_openxr.last_resolution[1] != vr->get_hmd_height()) {
            m_openxr.create_swapchains();
        }

        // end the frame before something terrible happens
        //vr->m_openxr.synchronize_frame();
        //vr->m_openxr.begin_frame();
        //vr->m_openxr.end_frame();
    }

    m_openvr.texture_counter = 0;
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

    const auto backbuffer_desc = backbuffer->GetDesc();

    // DXGI_FORMAT_R11G11B10_FLOAT
    m_backbuffer_is_8bit = backbuffer_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM;

    auto backbuffer_srv_desc = backbuffer_desc;
    backbuffer_srv_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    backbuffer_srv_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    // Create copy of backbuffer to use as SRV to convert from HDR to 8bit
    if (!m_backbuffer_is_8bit) {
            ComPtr<ID3D12Resource> backbuffer_copy{};
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_srv_desc, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                                                       IID_PPV_ARGS(backbuffer_copy.GetAddressOf()))))
            {
                spdlog::error("[VR] Failed to create backbuffer copy.");
                return;
            }

        if (!m_backbuffer_copy.setup(device, backbuffer_copy.Get(), std::nullopt, std::nullopt)) {
                spdlog::error("[VR] Error setting up backbuffer copy texture RTV/SRV.");
            }
    }

    auto rt_desc = backbuffer_desc;

    // rt_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    rt_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    spdlog::info("[VR] D3D12 Backbuffer width: {}, height: {}", backbuffer_desc.Width, backbuffer_desc.Height);

    // Create converted eye texture
    if (!m_backbuffer_is_8bit) {
        ComPtr<ID3D12Resource> eye_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                IID_PPV_ARGS(eye_tex.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create converted eye texture.");
            return;
        }

        if (!m_converted_eye_tex.setup(device, eye_tex.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up converted eye texture RTV/SRV.");
        }
    }

    for (auto& ctx : m_openvr.left_eye_tex) {
        ComPtr<ID3D12Resource> left_eye_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(left_eye_tex.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create left eye texture.");
            return;
        }

        left_eye_tex->SetName(L"OpenVR Left Eye Texture");
        if (!ctx.setup(device, left_eye_tex.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up left eye texture RTV/SRV.");
        }
    }
 
    for (auto& ctx : m_openvr.right_eye_tex) {
        ComPtr<ID3D12Resource> right_eye_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(right_eye_tex.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create right eye texture.");
            return;
        }

        right_eye_tex->SetName(L"OpenVR Right Eye Texture");
        if (!ctx.setup(device, right_eye_tex.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up right eye texture RTV/SRV.");
        }
    }

    for (auto& copier : m_generic_copiers) {
        copier.setup();
    }

    setup_sprite_batch_pso(rt_desc.Format);

    m_backbuffer_size[0] = rt_desc.Width;
    m_backbuffer_size[1] = rt_desc.Height;

    spdlog::info("[VR] d3d12 textures have been setup");
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
    const auto half_width = (LONG)(desc.Width / 2);
    const bool mono = vr->is_native_mono_frame();

    const auto eye_format = m_native_eye[0].texture->GetDesc().Format;
    if (m_native_copy_batch == nullptr || m_native_copy_format != eye_format) {
        auto device = g_framework->get_d3d12_hook()->get_device();
        DirectX::ResourceUploadBatch upload{ device };
        upload.Begin();
        DirectX::RenderTargetState output_state{ eye_format, DXGI_FORMAT_UNKNOWN };
        DirectX::SpriteBatchPipelineStateDescription pd{ output_state, &DirectX::DX12::CommonStates::Opaque };
        m_native_copy_batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, pd);
        upload.End(g_framework->get_d3d12_hook()->get_command_queue()).wait();
        m_native_copy_format = eye_format;
    }

    auto& commands = m_native_source.commands;
    commands.wait(INFINITE);
    commands.copy(backbuffer, m_native_source.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);

    // Each half of the side-by-side buffer is stretched over its eye's whole image.
    auto command_list = commands.cmd_list.Get();
    for (uint32_t eye = 0; eye < 2; ++eye) {
        auto& dst = m_native_eye[eye];
        const auto dst_desc = dst.texture->GetDesc();
        const LONG left = (mono ? 0 : (LONG)eye) * half_width;
        const RECT source{ left, 0, left + half_width, (LONG)desc.Height };

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
        ID3D12DescriptorHeap* heaps[]{ m_native_source.srv_heap->Heap() };
        command_list->SetDescriptorHeaps(1, heaps);

        m_native_copy_batch->SetViewport(viewport);
        m_native_copy_batch->Begin(command_list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);
        const RECT dest{ 0, 0, (LONG)dst_desc.Width, (LONG)dst_desc.Height };
        m_native_copy_batch->Draw(m_native_source.get_srv_gpu(), DirectX::XMUINT2{ (uint32_t)desc.Width, (uint32_t)desc.Height }, dest, &source, DirectX::Colors::White);
        m_native_copy_batch->End();

        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        command_list->ResourceBarrier(2, barriers);
    }
    commands.execute();

    for (uint32_t eye = 0; eye < 2; ++eye) {
        m_openxr.copy(eye, m_native_eye[eye].texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
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
}

void D3D12Component::draw_comfort_vignette(VR* vr, ID3D12Resource* backbuffer) {
    const float strength = vr->get_comfort_vignette();
    const float fade = vr->get_comfort_fade();
    if ((strength <= 0.01f && fade <= 0.01f) || backbuffer == nullptr) {
        return;
    }

    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    const auto desc = backbuffer->GetDesc();

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
        m_vignette_rtv_heap = std::make_unique<DirectX::DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 1);
        m_vignette_commands.setup(L"Comfort vignette");
    }

    if (m_vignette_batch == nullptr || m_vignette_format != desc.Format) {
        DirectX::ResourceUploadBatch upload{ device };
        upload.Begin();
        DirectX::RenderTargetState output_state{ desc.Format, DXGI_FORMAT_UNKNOWN };
        DirectX::SpriteBatchPipelineStateDescription pd{ output_state };
        m_vignette_batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, pd);
        upload.End(command_queue).wait();
        m_vignette_format = desc.Format;
    }

    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
    rtv_desc.Format = desc.Format;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const auto rtv = m_vignette_rtv_heap->GetCpuHandle(0);
    device->CreateRenderTargetView(backbuffer, &rtv_desc, rtv);

    m_vignette_commands.wait(INFINITE);
    auto command_list = m_vignette_commands.cmd_list.Get();

    const auto to_rt = CD3DX12_RESOURCE_BARRIER::Transition(backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    command_list->ResourceBarrier(1, &to_rt);
    command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const auto width = (float)desc.Width;
    const auto height = (float)desc.Height;
    D3D12_VIEWPORT viewport{ 0.0f, 0.0f, width, height, D3D12_MIN_DEPTH, D3D12_MAX_DEPTH };
    D3D12_RECT scissor{ 0, 0, (LONG)desc.Width, (LONG)desc.Height };
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor);

    ID3D12DescriptorHeap* heaps[]{ m_vignette_srv_heap->Heap() };
    command_list->SetDescriptorHeaps(1, heaps);

    // Stronger vignettes shrink the clear area; weak ones also fade in.
    const float scale = 2.0f - std::clamp(strength, 0.0f, 1.0f);
    const float alpha = std::clamp(strength / 0.3f, 0.0f, 1.0f);
    const int eye_count = vr->is_native_stereo() ? 2 : 1;
    const float eye_width = width / (float)eye_count;

    m_vignette_batch->SetViewport(viewport);
    m_vignette_batch->Begin(command_list);
    for (int eye = 0; eye < eye_count && strength > 0.01f; ++eye) {
        const float x0 = eye_width * (float)eye;
        const RECT dest{
            (LONG)(x0 + eye_width * 0.5f * (1.0f - scale)), (LONG)(height * 0.5f * (1.0f - scale)),
            (LONG)(x0 + eye_width * 0.5f * (1.0f + scale)), (LONG)(height * 0.5f * (1.0f + scale)),
        };
        m_vignette_batch->Draw(m_vignette_srv_heap->GetGpuHandle(0), DirectX::XMUINT2{ 256, 256 }, dest, DirectX::XMVECTORF32{ { { 1.0f, 1.0f, 1.0f, alpha } } });
    }
    if (fade > 0.01f) {
        const RECT full{ 0, 0, (LONG)desc.Width, (LONG)desc.Height };
        const float a = std::clamp(fade, 0.0f, 1.0f);
        m_vignette_batch->Draw(m_vignette_srv_heap->GetGpuHandle(1), DirectX::XMUINT2{ 1, 1 }, full, DirectX::XMVECTORF32{ { { 1.0f, 1.0f, 1.0f, a } } });
    }
    m_vignette_batch->End();

    const auto to_present = CD3DX12_RESOURCE_BARRIER::Transition(backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    command_list->ResourceBarrier(1, &to_present);

    m_vignette_commands.has_commands = true;
    m_vignette_commands.execute();
}

void D3D12Component::setup_sprite_batch_pso(DXGI_FORMAT output_format) {
    spdlog::info("[D3D12] Setting up sprite batch PSO");

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    DirectX::ResourceUploadBatch upload{ device };
    upload.Begin();

    DirectX::RenderTargetState output_state{output_format, DXGI_FORMAT_UNKNOWN};
    DirectX::SpriteBatchPipelineStateDescription pd{output_state};

    m_sprite_batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, pd);

    auto result = upload.End(command_queue);
    result.wait();

    spdlog::info("[D3D12] Sprite batch PSO setup complete");
}

void D3D12Component::render_srv_to_rtv(ID3D12GraphicsCommandList* command_list, const d3d12::TextureContext& src, const d3d12::TextureContext& dst, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
    const auto dst_desc = dst.texture->GetDesc();
    const auto src_desc = src.texture->GetDesc();
    
    auto& batch = m_sprite_batch;

    D3D12_VIEWPORT viewport{};
    viewport.Width = (float)dst_desc.Width;
    viewport.Height = (float)dst_desc.Height;
    viewport.MinDepth = D3D12_MIN_DEPTH;
    viewport.MaxDepth = D3D12_MAX_DEPTH;
    
    batch->SetViewport(viewport);

    D3D12_RECT scissor_rect{};
    scissor_rect.left = 0;
    scissor_rect.top = 0;
    scissor_rect.right = (LONG)dst_desc.Width;
    scissor_rect.bottom = (LONG)dst_desc.Height;

    // Transition dst to D3D12_RESOURCE_STATE_RENDER_TARGET
    D3D12_RESOURCE_BARRIER barriers[]{
        CD3DX12_RESOURCE_BARRIER::Transition(dst.texture.Get(), dst_state, D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(src.texture.Get(), src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    };
    command_list->ResourceBarrier(2, barriers);

    command_list->ClearRenderTargetView(dst.get_rtv(), DirectX::Colors::Black, 0, nullptr);

    // Set RTV to backbuffer
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_heaps[] = { dst.get_rtv() };
    command_list->OMSetRenderTargets(1, rtv_heaps, FALSE, nullptr);

    // Setup viewport and scissor rects
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor_rect);

    batch->Begin(command_list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);
    RECT dest_rect{ 0, 0, (LONG)dst_desc.Width, (LONG)dst_desc.Height };

    // Set descriptor heaps
    ID3D12DescriptorHeap* game_heaps[] = { src.srv_heap->Heap() };
    command_list->SetDescriptorHeaps(1, game_heaps);

    batch->Draw(src.get_srv_gpu(), 
        DirectX::XMUINT2{ (uint32_t)src_desc.Width, (uint32_t)src_desc.Height },
        dest_rect,
        DirectX::Colors::White);
    //TODO add tonemap for SRGB
    batch->End();

    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[0].Transition.StateAfter = dst_state;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[1].Transition.StateAfter = src_state;
    command_list->ResourceBarrier(2, barriers);
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
    this->contexts.resize(5);

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
