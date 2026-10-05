#pragma once
#include <glm/ext/matrix_float4x4.hpp>
#include <openxr/openxr.h>
#include <openvr.h>

namespace GlobalPool
{
    static const int CONSTANTS_HISTORY_SIZE = 10;

    struct Constants
    {
        glm::mat4     projection{ 1.0 };
        glm::mat4     finalView{ 1.0f };
//        glm::mat4     cameraView{1.0f};
        struct OpenXR {
            XrPosef pose{};
            XrFovf  fov{};
            XrPosef view_pose[2]{};
            XrFovf  view_fov[2]{};
        } openxr;

        struct OpenVR {
            vr::HmdMatrix34_t pose{};
        } openvr;
    };

    extern Constants g_constants[CONSTANTS_HISTORY_SIZE];

    glm::mat4 get_correction_matrix(int frame, int past_frame);

    inline void submit_projection(glm::mat4& projection, int frame)
    {
        g_constants[frame % CONSTANTS_HISTORY_SIZE].projection = projection;
    }

    inline void submit_final_view(glm::mat4& finalView, int frame)
    {
        g_constants[frame % CONSTANTS_HISTORY_SIZE].finalView = finalView;
    }
//
//    inline void submit_camera_view(glm::mat4& cameraView, int frame)
//    {
//        g_constants[frame % CONSTANTS_HISTORY_SIZE].cameraView = cameraView;
//    }
//
//    inline const auto& get_camera_view(int frame)
//    {
//        return g_constants[frame % CONSTANTS_HISTORY_SIZE].cameraView;
//    }

    inline auto& get_projection(int frame)
    {
        return g_constants[frame % CONSTANTS_HISTORY_SIZE].projection;
    }

    inline const auto& get_final_view(int frame)
    {
        return g_constants[frame % CONSTANTS_HISTORY_SIZE].finalView;
    }

    inline void submit_openxr_pose(XrPosef& pose, int frame) {
        g_constants[frame % CONSTANTS_HISTORY_SIZE].openxr.pose = pose;
    }

    inline const auto& get_openxr_pose(int frame) {
        return g_constants[frame % CONSTANTS_HISTORY_SIZE].openxr.pose;
    }

    inline void submit_openxr_fov(XrFovf& fov, int frame) {
        g_constants[frame % CONSTANTS_HISTORY_SIZE].openxr.fov = fov;
    }

    inline const auto& get_openxr_fov(int frame) {
        return g_constants[frame % CONSTANTS_HISTORY_SIZE].openxr.fov;
    }

    inline void submit_openxr_view_pair(const XrView& left, const XrView& right, int frame) {
        auto& xr = g_constants[frame % CONSTANTS_HISTORY_SIZE].openxr;
        xr.view_pose[0] = left.pose;
        xr.view_pose[1] = right.pose;
        xr.view_fov[0] = left.fov;
        xr.view_fov[1] = right.fov;
    }

    inline const auto& get_xr_constants(int frame) {
        return g_constants[frame % CONSTANTS_HISTORY_SIZE].openxr;
    }

    inline void submit_openvr_pose(const vr::HmdMatrix34_t& pose, int frame) {
        g_constants[frame % CONSTANTS_HISTORY_SIZE].openvr.pose = pose;
    }

    inline const auto& get_openvr_pose(int frame) {
        return g_constants[frame % CONSTANTS_HISTORY_SIZE].openvr.pose;
    }

//    XrPosef calculate_xr_pose_compensation(int reference_frame, int current_frame);
}; // namespace GlobalPool
