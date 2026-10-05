#include <ModSettings.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <openvr.h>
#include <aer/ConstantsPool.h>

#include "../VR.hpp"

#include "D3D11Component.hpp"

#ifdef VERBOSE_D3D11
#define LOG_VERBOSE(...) spdlog::info(__VA_ARGS__)
#else
#define LOG_VERBOSE
#endif

namespace vrmod {
class DX11StateBackup {
public:
    DX11StateBackup(ID3D11DeviceContext* context) {
        if (context == nullptr) {
            throw std::runtime_error("DX11StateBackup: context is null");
        }

        m_context = context;

        m_old.ScissorRectsCount = m_old.ViewportsCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        m_context->RSGetScissorRects(&m_old.ScissorRectsCount, m_old.ScissorRects);
        m_context->RSGetViewports(&m_old.ViewportsCount, m_old.Viewports);
        m_context->RSGetState(&m_old.RS);
        m_context->OMGetBlendState(&m_old.BlendState, m_old.BlendFactor, &m_old.SampleMask);
        m_context->OMGetDepthStencilState(&m_old.DepthStencilState, &m_old.StencilRef);
        m_context->PSGetShaderResources(0, 1, &m_old.PSShaderResource);
        m_context->PSGetSamplers(0, 1, &m_old.PSSampler);
        m_old.PSInstancesCount = m_old.VSInstancesCount = m_old.GSInstancesCount = 256;
        m_context->PSGetShader(&m_old.PS, m_old.PSInstances, &m_old.PSInstancesCount);
        m_context->VSGetShader(&m_old.VS, m_old.VSInstances, &m_old.VSInstancesCount);
        m_context->VSGetConstantBuffers(0, 1, &m_old.VSConstantBuffer);
        m_context->GSGetShader(&m_old.GS, m_old.GSInstances, &m_old.GSInstancesCount);

        m_context->IAGetPrimitiveTopology(&m_old.PrimitiveTopology);
        m_context->IAGetIndexBuffer(&m_old.IndexBuffer, &m_old.IndexBufferFormat, &m_old.IndexBufferOffset);
        m_context->IAGetVertexBuffers(0, 1, &m_old.VertexBuffer, &m_old.VertexBufferStride, &m_old.VertexBufferOffset);
        m_context->IAGetInputLayout(&m_old.InputLayout);
    }

    virtual ~DX11StateBackup() {
        m_context->RSSetScissorRects(m_old.ScissorRectsCount, m_old.ScissorRects);
        m_context->RSSetViewports(m_old.ViewportsCount, m_old.Viewports);
        m_context->RSSetState(m_old.RS); if (m_old.RS) m_old.RS->Release();
        m_context->OMSetBlendState(m_old.BlendState, m_old.BlendFactor, m_old.SampleMask); if (m_old.BlendState) m_old.BlendState->Release();
        m_context->OMSetDepthStencilState(m_old.DepthStencilState, m_old.StencilRef); if (m_old.DepthStencilState) m_old.DepthStencilState->Release();
        m_context->PSSetShaderResources(0, 1, &m_old.PSShaderResource); if (m_old.PSShaderResource) m_old.PSShaderResource->Release();
        m_context->PSSetSamplers(0, 1, &m_old.PSSampler); if (m_old.PSSampler) m_old.PSSampler->Release();
        m_context->PSSetShader(m_old.PS, m_old.PSInstances, m_old.PSInstancesCount); if (m_old.PS) m_old.PS->Release();
        for (UINT i = 0; i < m_old.PSInstancesCount; i++) if (m_old.PSInstances[i]) m_old.PSInstances[i]->Release();
        m_context->VSSetShader(m_old.VS, m_old.VSInstances, m_old.VSInstancesCount); if (m_old.VS) m_old.VS->Release();
        m_context->VSSetConstantBuffers(0, 1, &m_old.VSConstantBuffer); if (m_old.VSConstantBuffer) m_old.VSConstantBuffer->Release();
        m_context->GSSetShader(m_old.GS, m_old.GSInstances, m_old.GSInstancesCount); if (m_old.GS) m_old.GS->Release();
        for (UINT i = 0; i < m_old.VSInstancesCount; i++) if (m_old.VSInstances[i]) m_old.VSInstances[i]->Release();
        m_context->IASetPrimitiveTopology(m_old.PrimitiveTopology);
        m_context->IASetIndexBuffer(m_old.IndexBuffer, m_old.IndexBufferFormat, m_old.IndexBufferOffset); if (m_old.IndexBuffer) m_old.IndexBuffer->Release();
        m_context->IASetVertexBuffers(0, 1, &m_old.VertexBuffer, &m_old.VertexBufferStride, &m_old.VertexBufferOffset); if (m_old.VertexBuffer) m_old.VertexBuffer->Release();
        m_context->IASetInputLayout(m_old.InputLayout); if (m_old.InputLayout) m_old.InputLayout->Release();
    }

private:
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context{};

    struct BACKUP_DX11_STATE {
        UINT                        ScissorRectsCount, ViewportsCount;
        D3D11_RECT                  ScissorRects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        D3D11_VIEWPORT              Viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        ID3D11RasterizerState*      RS;
        ID3D11BlendState*           BlendState;
        FLOAT                       BlendFactor[4];
        UINT                        SampleMask;
        UINT                        StencilRef;
        ID3D11DepthStencilState*    DepthStencilState;
        ID3D11ShaderResourceView*   PSShaderResource;
        ID3D11SamplerState*         PSSampler;
        ID3D11PixelShader*          PS;
        ID3D11VertexShader*         VS;
        ID3D11GeometryShader*       GS;
        UINT                        PSInstancesCount, VSInstancesCount, GSInstancesCount;
        ID3D11ClassInstance         *PSInstances[256], *VSInstances[256], *GSInstances[256];   // 256 is max according to PSSetShader documentation
        D3D11_PRIMITIVE_TOPOLOGY    PrimitiveTopology;
        ID3D11Buffer*               IndexBuffer, *VertexBuffer, *VSConstantBuffer;
        UINT                        IndexBufferOffset, VertexBufferStride, VertexBufferOffset;
        DXGI_FORMAT                 IndexBufferFormat;
        ID3D11InputLayout*          InputLayout;
    } m_old{};
};

D3D11Component::TextureContext::TextureContext(ID3D11Resource* in_tex, std::optional<DXGI_FORMAT> rtv_format, std::optional<DXGI_FORMAT> srv_format) {
    set(in_tex, rtv_format, srv_format);
}

bool D3D11Component::TextureContext::set(ID3D11Resource* in_tex, std::optional<DXGI_FORMAT> rtv_format, std::optional<DXGI_FORMAT> srv_format) {
    bool is_same_tex = this->tex.Get() == in_tex;

    if (!is_same_tex) {
        this->tex.Reset();
    }

    this->tex = in_tex;

    if (in_tex == nullptr) {
        this->rtv.Reset();
        this->srv.Reset();
        return false;
    }
   
    if (!is_same_tex) {
        this->rtv.Reset();
        this->srv.Reset();

        auto device = g_framework->get_d3d11_hook()->get_device();
        bool made_rtv = false;
        bool made_srv = false;

        if (!rtv_format) {
            if (!FAILED(device->CreateRenderTargetView(tex.Get(), nullptr, &rtv))) {
                made_rtv = true;
            }
        }

        if (!srv_format) {  
            if (!FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) {
                made_srv = true;
            }
        }

        if (!made_rtv) {
            if (!rtv_format) {
                rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            }

            D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
            rtv_desc.Format = *rtv_format;
            rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rtv_desc.Texture2D.MipSlice = 0;

            made_rtv = !FAILED(device->CreateRenderTargetView(tex.Get(), &rtv_desc, &this->rtv));
        }

        if (!made_srv) {
            if (!srv_format) {
                srv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
            srv_desc.Format = *srv_format;
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv_desc.Texture2D.MipLevels = 1;
            srv_desc.Texture2D.MostDetailedMip = 0;

            made_srv = !FAILED(device->CreateShaderResourceView(tex.Get(), &srv_desc, &this->srv));
        }

        if (!made_rtv) {
            spdlog::error("Failed to create render target view for texture");
        }

        if (!made_srv) {
            spdlog::error("Failed to create shader resource view for texture");
        }

        return made_rtv && made_srv;
    }

    return true;
}

bool D3D11Component::TextureContext::clear_rtv(float* color) {
    if (tex == nullptr || rtv == nullptr) {
        return false;
    }

    if (color == nullptr) {
        return false;
    }

    auto device = g_framework->get_d3d11_hook()->get_device();

    ComPtr<ID3D11DeviceContext> context{};
    device->GetImmediateContext(&context);

    context->ClearRenderTargetView(rtv.Get(), color);
    return true;
}

vr::EVRCompositorError D3D11Component::on_frame(VR* vr) {
    // Native stereo presents through D3D12 only.
    static bool logged = false;
    if (!logged) {
        logged = true;
        spdlog::error("[VR] D3D11 rendering is not supported");
    }
    return vr::VRCompositorError_None;
}

void D3D11Component::on_reset(VR* vr) {
    m_backbuffer_rtv.Reset();
    m_backbuffer_copy_rt.reset();
    m_backbuffer_copy_tex.Reset();
    m_left_eye_rt.reset();
    m_right_eye_rt.reset();
    m_left_eye_tex.Reset();
    m_right_eye_tex.Reset();
    m_left_eye_depthstencil.Reset();
    m_right_eye_depthstencil.Reset();
    m_sprite_batch.reset();
    m_toneMap.reset();

    if (vr->get_runtime()->is_openxr() && vr->get_runtime()->loaded) {
        if (m_openxr.last_resolution[0] != vr->get_hmd_width() || m_openxr.last_resolution[1] != vr->get_hmd_height()) {
            m_openxr.create_swapchains();
        }
    }
}

bool D3D11Component::setup() {
    // Get device and swapchain.
    auto& hook = g_framework->get_d3d11_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    // Get back buffer.
    ComPtr<ID3D11Texture2D> backbuffer{};

    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get backbuffer (D3D11).");
        return false;
    }

    // Get backbuffer description.
    D3D11_TEXTURE2D_DESC backbuffer_desc{};

    backbuffer->GetDesc(&backbuffer_desc);
    backbuffer_desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    backbuffer_desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

    m_backbuffer_is_8bit = backbuffer_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM;

    // Create raw RTV of backbuffer.
    if (FAILED(device->CreateRenderTargetView(backbuffer.Get(), nullptr, &m_backbuffer_rtv))) {
        spdlog::error("[VR] Failed to create backbuffer raw render target (D3D11).");
        return false;
    }

    // Create a copy of the backbuffer that has SRV and RTV bind flags so we can pass it to the sprite batch.
    if (FAILED(device->CreateTexture2D(&backbuffer_desc, nullptr, &m_backbuffer_copy_tex))) {
        spdlog::error("[VR] Failed to create backbuffer copy (D3D11).");
        return false;
    }

    if (!m_backbuffer_copy_rt.set(m_backbuffer_copy_tex.Get(), std::nullopt, std::nullopt)) {
        spdlog::error("[VR] Failed to create backbuffer render target (D3D11).");
        return false;
    }

    backbuffer_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    m_backbuffer_size[0] = backbuffer_desc.Width;
    m_backbuffer_size[1] = backbuffer_desc.Height;

    // Create eye textures.
    if (FAILED(device->CreateTexture2D(&backbuffer_desc, nullptr, &m_left_eye_tex))) {
        spdlog::error("[VR] Failed to create left eye texture (D3D11).");
        return false;
    }

    if (FAILED(device->CreateTexture2D(&backbuffer_desc, nullptr, &m_right_eye_tex))) {
        spdlog::error("[VR] Failed to create right eye texture (D3D11).");
        return false;
    }

    if (!m_left_eye_rt.set(m_left_eye_tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM)) {
        spdlog::error("[VR] Failed to create left eye render target (D3D11).");
        return false;
    }

    if (!m_right_eye_rt.set(m_right_eye_tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM)) {
        spdlog::error("[VR] Failed to create right eye render target (D3D11).");
        return false;
    }

    // copy backbuffer into right eye
    // Get the context.
    ComPtr<ID3D11DeviceContext> context{};

    device->GetImmediateContext(&context);
    //context->CopyResource(m_right_eye_tex.Get(), backbuffer.Get());

    // Make depth stencils for both eyes.
    auto depthstencil = hook->get_last_depthstencil_used();

    if (depthstencil != nullptr) {
        D3D11_TEXTURE2D_DESC depthstencil_desc{};

        depthstencil->GetDesc(&depthstencil_desc);

        // Create eye depthstencils.
        device->CreateTexture2D(&depthstencil_desc, nullptr, &m_left_eye_depthstencil);
        device->CreateTexture2D(&depthstencil_desc, nullptr, &m_right_eye_depthstencil);

        // Copy the current depthstencil into the right eye.
        context->CopyResource(m_right_eye_depthstencil.Get(), depthstencil.Get());
    }

    m_sprite_batch = std::make_unique<DirectX::DX11::SpriteBatch>(context.Get());
    m_toneMap = std::make_unique<DirectX::DX11::ToneMapPostProcess>(device);

    spdlog::info("[VR] d3d11 textures have been setup");

    return true;
}

void D3D11Component::OpenXR::initialize(XrSessionCreateInfo& session_info) {
    std::scoped_lock _{this->mtx};

    auto& hook = g_framework->get_d3d11_hook();

    auto device = hook->get_device();

    this->binding.device = device;

    PFN_xrGetD3D11GraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(VR::get()->m_openxr->instance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)(&fn));

    if (fn == nullptr) {
        spdlog::error("[VR] xrGetD3D11GraphicsRequirementsKHR not found");
        return;
    }

    // get existing adapter from device
    ComPtr<IDXGIDevice> dxgi_device{};
    
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
        spdlog::error("[VR] failed to get DXGI device from D3D11 device");
        return;
    }
    
    ComPtr<IDXGIAdapter> adapter{};

    if (FAILED(dxgi_device->GetAdapter(&adapter))) {
        spdlog::error("[VR] failed to get DXGI adapter from DXGI device");
        return;
    }

    DXGI_ADAPTER_DESC desc{};

    if (FAILED(adapter->GetDesc(&desc))) {
        spdlog::error("[VR] failed to get DXGI adapter description");
        return;
    }
    
    XrGraphicsRequirementsD3D11KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    gr.adapterLuid = desc.AdapterLuid;
    gr.minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    fn(VR::get()->m_openxr->instance, VR::get()->m_openxr->system, &gr);

    session_info.next = &this->binding;
}

std::optional<std::string> D3D11Component::OpenXR::create_swapchains() {
    std::scoped_lock _{this->mtx};

    spdlog::info("[VR] Creating OpenXR swapchains for D3D11");

    this->destroy_swapchains();

    auto& hook = g_framework->get_d3d11_hook();
    auto device = hook->get_device();
    auto  lDxgiSwapchain = hook->get_swap_chain();

    // Get back buffer.
    ComPtr<ID3D11Texture2D> backbuffer{};

    lDxgiSwapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));

    if (backbuffer == nullptr) {
        spdlog::error("[VR] Failed to get back buffer.");
        return "Failed to get back buffer.";
    }

    // Get backbuffer description.
    D3D11_TEXTURE2D_DESC backbuffer_desc{};
    backbuffer->GetDesc(&backbuffer_desc);

    backbuffer_desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    backbuffer_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    auto& vr = VR::get();
    auto& openxr = vr->m_openxr;

    uint32_t ui_textture_size[2] = {backbuffer_desc.Width, backbuffer_desc.Height};
    backbuffer_desc.Width = vr->get_hmd_width();
    backbuffer_desc.Height = vr->get_hmd_height();

    this->contexts.clear();
    //TODO clean up this mess
    this->contexts.resize((size_t)runtimes::OpenXR::SwapchainIndex::END);
    auto create_swapchain = [&](uint32_t swapchainIndex, int format, int width, int height) -> std::optional<std::string> {
        spdlog::info("[VR] Creating swapchain for eye {}", swapchainIndex);
        spdlog::info("[VR] Width: {}", width);
        spdlog::info("[VR] Height: {}", height);
        spdlog::info("[VR] Format: {}", format);
        XrSwapchainCreateInfo swapchain_create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchain_create_info.arraySize = 1;
        swapchain_create_info.format = format;
        swapchain_create_info.width = width;
        swapchain_create_info.height = height;
        swapchain_create_info.mipCount = 1;
        swapchain_create_info.faceCount = 1;
        swapchain_create_info.sampleCount = backbuffer_desc.SampleDesc.Count;
        swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        auto& swapchain = vr->m_openxr->swapchains[swapchainIndex];
        swapchain.width = swapchain_create_info.width;
        swapchain.height = swapchain_create_info.height;
        if (xrCreateSwapchain(openxr->session, &swapchain_create_info, &swapchain.handle) != XR_SUCCESS) {
            spdlog::error("[VR] D3D11: Failed to create swapchain.");
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
        ctx.swapchain_index = swapchainIndex;

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        }

        result = xrEnumerateSwapchainImages(swapchain.handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)&ctx.textures[0]);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images after texture creation.");
            return "Failed to enumerate swapchain images after texture creation.";
        }
        return std::nullopt;
    };

    if(openxr->get_view_count() != 2) {
        spdlog::error("[VR] Unsupported view count: {}", openxr->get_view_count());
        return "Unsupported view count.";
    }

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

    spdlog::info("[VR] Successfully created OpenXR swapchains for D3D11");

    return std::nullopt;
}

void D3D11Component::OpenXR::destroy_swapchains() {
    std::scoped_lock _{this->mtx};

	if (this->contexts.empty()) {
        return;
    }

    spdlog::info("[VR] Destroying swapchains.");

    for (auto i = 0; i < this->contexts.size(); ++i) {
        auto& ctx = this->contexts[i];

        if(VR::get()->m_openxr->swapchains[i].handle) {
            auto result = xrDestroySwapchain(VR::get()->m_openxr->swapchains[i].handle);
            if (result != XR_SUCCESS) {
                spdlog::error("[VR] Failed to destroy swapchain {}.", i);
            } else {
                spdlog::info("[VR] Destroyed swapchain {}.", i);
            }
        }
        ctx.textures.clear();
        VR::get()->m_openxr->swapchains[ctx.swapchain_index] = {};
    }

    this->contexts.clear();
}

void D3D11Component::OpenXR::copy(uint32_t swapchain_idx, ID3D11Texture2D* resource) {
    std::scoped_lock _{this->mtx};

    auto& vr = VR::get();

    if (!vr->m_openxr->should_render()) {
        return;
    }

    if (!vr->m_openxr->frame_began) {
        spdlog::error("[VR] OpenXR: Frame not begun when trying to copy.");
        //return;
    }

    if (this->contexts[swapchain_idx].num_textures_acquired > 0) {
        spdlog::info("[VR] Already acquired textures for swapchain {}?", swapchain_idx);
    }

    auto device = g_framework->get_d3d11_hook()->get_device();
    
    // get immediate context
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);

    const auto& swapchain = vr->m_openxr->swapchains[swapchain_idx];
    auto& ctx = this->contexts[swapchain_idx];

    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

    uint32_t texture_index{};
    LOG_VERBOSE("Acquiring swapchain image for {}", swapchain_idx);
    auto result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
    } else {
        ctx.num_textures_acquired++;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        //wait_info.timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)).count();
        wait_info.timeout = XR_INFINITE_DURATION;

        LOG_VERBOSE("Waiting on swapchain image for {}", swapchain_idx);
        result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        } else {
            LOG_VERBOSE("Copying swapchain image {} for {}", texture_index, swapchain_idx);
            context->CopyResource(ctx.textures[texture_index].texture, resource);

            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

            LOG_VERBOSE("Releasing swapchain image for {}", swapchain_idx);
            auto result = xrReleaseSwapchainImage(swapchain.handle, &release_info);

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
