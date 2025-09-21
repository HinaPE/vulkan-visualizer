#include "vv_camera.h"
#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <imgui.h>
#include <numbers>

namespace vv {

static constexpr float deg2rad(float d) noexcept { return d * std::numbers::pi_v<float> / 180.0f; }

float3 make_float3(float x, float y, float z) { return float3{x,y,z}; }
float3 operator+(const float3& a, const float3& b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
float3 operator-(const float3& a, const float3& b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
float3 operator*(const float3& a, float s) { return {a.x*s, a.y*s, a.z*s}; }
float3 operator/(const float3& a, float s) { return {a.x/s, a.y/s, a.z/s}; }
float  dot(const float3& a, const float3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
float3 cross(const float3& a, const float3& b) { return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x }; }
float  length(const float3& a) { return std::sqrt(dot(a,a)); }
float3 normalize(const float3& a) { float l = length(a); return l>0? a/l : float3{0,0,0}; }

float4x4 make_identity() { float4x4 r{}; r.m = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; return r; }

float4x4 mul(const float4x4& A, const float4x4& B) {
    float4x4 R{};
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            R.m[c*4 + r] = A.m[0*4 + r]*B.m[c*4 + 0] + A.m[1*4 + r]*B.m[c*4 + 1] + A.m[2*4 + r]*B.m[c*4 + 2] + A.m[3*4 + r]*B.m[c*4 + 3];
        }
    }
    return R;
}

float4x4 make_look_at(const float3& eye, const float3& center, const float3& up_) {
    const float3 f = normalize(center - eye);
    const float3 s = normalize(cross(f, normalize(up_)));
    const float3 u = cross(s, f);
    float4x4 M{};
    M.m = { s.x, u.x, -f.x, 0,
            s.y, u.y, -f.y, 0,
            s.z, u.z, -f.z, 0,
            -dot(s, eye), -dot(u, eye), dot(f, eye), 1 };
    return M;
}

float4x4 make_perspective(float fovy_rad, float aspect, float znear, float zfar) {
    const float f = 1.0f / std::tan(fovy_rad * 0.5f);
    float4x4 M{};
    M.m = { f/aspect, 0, 0, 0,
            0, f, 0, 0,
            0, 0, (zfar+znear)/(znear-zfar), -1,
            0, 0, (2* zfar* znear)/(znear-zfar), 0 };
    return M;
}

float4x4 make_ortho(float left, float right, float bottom, float top, float znear, float zfar) {
    float4x4 M{};
    M.m = { 2.0f/(right-left), 0, 0, 0,
            0, 2.0f/(top-bottom), 0, 0,
            0, 0, -2.0f/(zfar-znear), 0,
            -(right+left)/(right-left), -(top+bottom)/(top-bottom), -(zfar+znear)/(zfar-znear), 1 };
    return M;
}

bool project_to_screen(const float3& p_world, const float4x4& view, const float4x4& proj, int screen_w, int screen_h, float& out_x, float& out_y) {
    // Multiply by view then proj
    float x = p_world.x, y = p_world.y, z = p_world.z;
    auto mul_v = [](const float4x4& M, float x, float y, float z, float w, float& ox, float& oy, float& oz, float& ow){
        ox = M.m[0]*x + M.m[4]*y + M.m[8]*z + M.m[12]*w;
        oy = M.m[1]*x + M.m[5]*y + M.m[9]*z + M.m[13]*w;
        oz = M.m[2]*x + M.m[6]*y + M.m[10]*z + M.m[14]*w;
        ow = M.m[3]*x + M.m[7]*y + M.m[11]*z + M.m[15]*w;
    };
    float vx, vy, vz, vw; mul_v(view, x,y,z,1.0f, vx,vy,vz,vw);
    float cx, cy, cz, cw; mul_v(proj, vx,vy,vz,vw, cx,cy,cz,cw);
    if (cw <= 0.0f) return false;
    const float ndc_x = cx / cw; const float ndc_y = cy / cw; // ignore z
    out_x = (ndc_x * 0.5f + 0.5f) * static_cast<float>(screen_w);
    out_y = (1.0f - (ndc_y * 0.5f + 0.5f)) * static_cast<float>(screen_h); // flip Y for top-left origin
    return true;
}

CameraService::CameraService() {
    recompute_cached_();
}

void CameraService::recompute_cached_() {
    // Compute view based on mode
    if (state_.mode == CameraMode::Orbit) {
        const float yaw = deg2rad(state_.yaw_deg);
        const float pitch = deg2rad(state_.pitch_deg);
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        const float cy = std::cos(yaw),   sy = std::sin(yaw);
        float3 dir{ cp * cy, sp, cp * sy };
        float3 eye = state_.target - dir * state_.distance;
        view_ = make_look_at(eye, state_.target, {0,1,0});
    } else {
        const float yaw = deg2rad(state_.fly_yaw_deg);
        const float pitch = deg2rad(state_.fly_pitch_deg);
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        const float cy = std::cos(yaw),   sy = std::sin(yaw);
        float3 fwd{ cp * cy, sp, cp * sy };
        float3 eye = state_.eye;
        view_ = make_look_at(eye, eye + fwd, {0,1,0});
    }

    const float aspect = std::max(1, vp_w_) / static_cast<float>(std::max(1, vp_h_));
    if (state_.projection == CameraProjection::Perspective) {
        proj_ = make_perspective(deg2rad(std::max(1.0f, state_.fov_y_deg)), aspect, std::max(1e-5f, state_.znear), std::max(state_.znear+1e-4f, state_.zfar));
    } else {
        const float h = std::max(1e-4f, state_.ortho_height);
        const float w = h * aspect;
        proj_ = make_ortho(-w, w, -h, h, std::max(1e-5f, state_.znear), std::max(state_.znear+1e-4f, state_.zfar));
    }
}

void CameraService::update(double dt, int viewport_w, int viewport_h) {
    vp_w_ = std::max(1, viewport_w);
    vp_h_ = std::max(1, viewport_h);

    apply_inertia_(dt);

    // Fly movement
    if (state_.mode == CameraMode::Fly) {
        float speed = 2.0f; // meters per second
        if (key_shift_) speed *= 3.5f;
        if (key_ctrl_)  speed *= 0.25f;
        float move = speed * static_cast<float>(dt) * state_.units_per_meter; // scale by world units per meter
        const float yaw = deg2rad(state_.fly_yaw_deg);
        const float pitch = deg2rad(state_.fly_pitch_deg);
        float3 fwd{ std::cos(pitch)*std::cos(yaw), std::sin(pitch), std::cos(pitch)*std::sin(yaw) };
        float3 right = normalize(cross(fwd, {0,1,0}));
        float3 up = normalize(cross(right, fwd));
        if (key_w_) state_.eye = state_.eye + fwd * move;
        if (key_s_) state_.eye = state_.eye - fwd * move;
        if (key_a_) state_.eye = state_.eye - right * move;
        if (key_d_) state_.eye = state_.eye + right * move;
        if (key_q_) state_.eye = state_.eye - up * move;
        if (key_e_) state_.eye = state_.eye + up * move;
    }

    // Animation blending
    if (anim_active_) {
        anim_t_ += static_cast<float>(dt);
        float t = std::clamp(anim_t_ / std::max(1e-3f, anim_dur_), 0.0f, 1.0f);
        auto lerp = [](float a, float b, float t){ return a + (b-a)*t; };
        CameraState cur = anim_from_;
        cur.mode = anim_to_.mode; // adopt dest mode
        cur.projection = anim_to_.projection;
        cur.units_per_meter = lerp(anim_from_.units_per_meter, anim_to_.units_per_meter, t);
        cur.target = anim_from_.target * (1.0f - t) + anim_to_.target * t;
        cur.distance = lerp(anim_from_.distance, anim_to_.distance, t);
        // yaw/pitch shortest path wrap
        auto lerp_angle = [](float a, float b, float t){ float d = std::fmod(b - a + 540.0f, 360.0f) - 180.0f; return a + d * t; };
        cur.yaw_deg = lerp_angle(anim_from_.yaw_deg, anim_to_.yaw_deg, t);
        cur.pitch_deg = lerp_angle(anim_from_.pitch_deg, anim_to_.pitch_deg, t);
        cur.eye = anim_from_.eye * (1.0f - t) + anim_to_.eye * t;
        cur.fly_yaw_deg = lerp_angle(anim_from_.fly_yaw_deg, anim_to_.fly_yaw_deg, t);
        cur.fly_pitch_deg = lerp(anim_from_.fly_pitch_deg, anim_to_.fly_pitch_deg, t);
        cur.fov_y_deg = lerp(anim_from_.fov_y_deg, anim_to_.fov_y_deg, t);
        cur.ortho_height = lerp(anim_from_.ortho_height, anim_to_.ortho_height, t);
        cur.znear = lerp(anim_from_.znear, anim_to_.znear, t);
        cur.zfar  = lerp(anim_from_.zfar, anim_to_.zfar, t);
        state_ = cur;
        if (t >= 1.0f) { anim_active_ = false; }
    }

    recompute_cached_();
}

void CameraService::set_state(const CameraState& s) { state_ = s; recompute_cached_(); }
void CameraService::set_mode(CameraMode m) { state_.mode = m; recompute_cached_(); }
void CameraService::set_projection(CameraProjection p) { state_.projection = p; recompute_cached_(); }

float4x4 CameraService::view_matrix() const { return view_; }
float4x4 CameraService::proj_matrix() const { return proj_; }
float3   CameraService::eye_position() const {
    if (state_.mode == CameraMode::Orbit) {
        const float yaw = deg2rad(state_.yaw_deg);
        const float pitch = deg2rad(state_.pitch_deg);
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        const float cy = std::cos(yaw),   sy = std::sin(yaw);
        float3 dir{ cp * cy, sp, cp * sy };
        return state_.target - dir * state_.distance;
    } else {
        return state_.eye;
    }
}

void CameraService::fit(const BoundingBox& bbox, float padding) {
    if (!bbox.valid) return;
    float3 c = (bbox.min + bbox.max) * 0.5f;
    float3 e = (bbox.max - bbox.min) * 0.5f;
    const float radius = length(e) * padding;
    if (state_.projection == CameraProjection::Perspective) {
        const float fov = deg2rad(std::max(1.0f, state_.fov_y_deg));
        const float dist = std::max(radius / std::max(1e-4f, std::tan(fov*0.5f)), 1e-3f);
        state_.target = c;
        state_.distance = dist;
    } else {
        state_.target = c;
        state_.ortho_height = std::max(radius, 1e-3f);
    }
    recompute_cached_();
}

void CameraService::set_units_per_meter(float upm) { state_.units_per_meter = std::clamp(upm, 1e-6f, 1e6f); }

void CameraService::begin_orbit_(int mx, int my) { last_mx_ = mx; last_my_ = my; }
void CameraService::update_orbit_drag_(int mx, int my, bool pan) {
    const int dx = mx - last_mx_; const int dy = my - last_my_;
    last_mx_ = mx; last_my_ = my;
    if (!pan) {
        constexpr float sens = 0.25f; // deg per pixel
        state_.yaw_deg   += dx * sens; yaw_vel_ = dx * sens * 10.0f; // amplify for inertia
        state_.pitch_deg -= dy * sens; pitch_vel_ = -dy * sens * 10.0f; // invert Y: move up -> pitch up
        state_.pitch_deg = std::clamp(state_.pitch_deg, -89.5f, 89.5f);
    } else {
        // Pan in screen space mapped to world (approximate)
        float base = (state_.projection == CameraProjection::Orthographic) ? std::max(1e-4f, state_.ortho_height) : std::max(1e-4f, state_.distance);
        float pan_speed = base * 0.0015f * (key_shift_ ? 4.0f : 1.0f);
        float3 right = normalize(cross(normalize(float3{std::cos(deg2rad(state_.yaw_deg)), 0, std::sin(deg2rad(state_.yaw_deg))}), {0,1,0}));
        float3 up = float3{0,1,0};
        state_.target = state_.target - right * (dx * pan_speed) - up * (dy * pan_speed); // invert vertical to follow mouse
        pan_x_vel_ = -dx * pan_speed * 10.0f; pan_y_vel_ = -dy * pan_speed * 10.0f;
    }
}
void CameraService::end_orbit_() {}

void CameraService::begin_fly_(int mx, int my, const EngineContext*) { last_mx_ = mx; last_my_ = my; fly_capturing_ = true; }
void CameraService::update_fly_look_(int dx, int dy) {
    constexpr float sens = 0.15f; // deg per pixel
    state_.fly_yaw_deg   += dx * sens;
    state_.fly_pitch_deg -= dy * sens; // invert Y so up moves pitch up
    state_.fly_pitch_deg  = std::clamp(state_.fly_pitch_deg, -89.0f, 89.0f);
}
void CameraService::end_fly_(const EngineContext*) { fly_capturing_ = false; }

void CameraService::apply_inertia_(double dt) {
    const float damp = std::exp(-static_cast<float>(dt) * 6.0f);
    // Stop inertia while actively interacting (any mouse or nav)
    if (!(lmb_ || rmb_ || mmb_ || nav_orbiting_ || nav_panning_ || nav_dollying_) && state_.mode == CameraMode::Orbit) {
        state_.yaw_deg   += yaw_vel_ * static_cast<float>(dt);
        state_.pitch_deg += pitch_vel_ * static_cast<float>(dt);
        state_.pitch_deg = std::clamp(state_.pitch_deg, -89.5f, 89.5f);
        state_.target.x  += pan_x_vel_ * static_cast<float>(dt);
        state_.target.y  += pan_y_vel_ * static_cast<float>(dt);
        if (state_.projection == CameraProjection::Perspective) {
            state_.distance  *= (1.0f + zoom_vel_ * static_cast<float>(dt));
        } else {
            state_.ortho_height *= (1.0f + zoom_vel_ * static_cast<float>(dt));
            state_.ortho_height = std::clamp(state_.ortho_height, 1e-4f, 1e6f);
        }
        yaw_vel_ *= damp; pitch_vel_ *= damp; pan_x_vel_ *= damp; pan_y_vel_ *= damp; zoom_vel_ *= damp;
    }
}

void CameraService::home_view() {
    CameraState dst = state_;
    dst.mode = CameraMode::Orbit;
    dst.target = {0,0,0};
    dst.yaw_deg = -45.0f;
    dst.pitch_deg = 25.0f;
    // choose distance so that 1-unit object is nicely visible
    const float fov = deg2rad(std::max(1.0f, dst.fov_y_deg));
    const float target_radius = 1.0f;
    dst.distance = std::max(target_radius / std::max(1e-4f, std::tan(fov*0.5f)) * 2.2f, 0.1f);
    start_animation_to_(dst, 0.35f);
}

void CameraService::frame_scene(float padding) {
    if (!scene_bounds_.valid) { home_view(); return; }
    CameraState dst = state_;
    // Compute fitted distance/height but keep yaw/pitch
    float3 c = (scene_bounds_.min + scene_bounds_.max) * 0.5f;
    float3 e = (scene_bounds_.max - scene_bounds_.min) * 0.5f;
    const float radius = length(e) * padding;
    dst.target = c;
    if (dst.projection == CameraProjection::Perspective) {
        const float fov = deg2rad(std::max(1.0f, dst.fov_y_deg));
        dst.distance = std::max(radius / std::max(1e-4f, std::tan(fov*0.5f)), 1e-3f);
    } else {
        dst.ortho_height = std::max(radius, 1e-3f);
    }
    start_animation_to_(dst, 0.35f);
}

void CameraService::handle_event(const SDL_Event& e, const EngineContext* eng, const FrameContext*) {
    // ImGui capture guard
    ImGuiIO* io = nullptr;
    if (ImGui::GetCurrentContext()) io = &ImGui::GetIO();
    auto imgui_capturing_mouse = [&]() { return io && (io->WantCaptureMouse || io->WantCaptureKeyboard); };

    auto houdini_nav_active = [&]() { return (key_space_ || key_alt_) && state_.mode == CameraMode::Orbit; };

    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (imgui_capturing_mouse()) break;
        const int mx = e.button.x; const int my = e.button.y;
        if (e.button.button == SDL_BUTTON_LEFT)  { lmb_ = true; }
        if (e.button.button == SDL_BUTTON_MIDDLE){ mmb_ = true; }
        if (e.button.button == SDL_BUTTON_RIGHT) { rmb_ = true; }
        if (houdini_nav_active()) {
            // Space/Alt + LMB/MMB/RMB => orbit/pan/dolly
            last_mx_ = mx; last_my_ = my;
            nav_orbiting_ = (e.button.button == SDL_BUTTON_LEFT);
            nav_panning_  = (e.button.button == SDL_BUTTON_MIDDLE);
            nav_dollying_ = (e.button.button == SDL_BUTTON_RIGHT);
        } else {
            // No legacy orbit mapping. Only allow Fly-mode RMB look.
            if (state_.mode == CameraMode::Fly) {
                if (rmb_) begin_fly_(mx, my, eng);
            }
        }
        break; }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const int mx = e.button.x; const int my = e.button.y;
        if (e.button.button == SDL_BUTTON_LEFT)  { lmb_ = false; nav_orbiting_ = false; }
        if (e.button.button == SDL_BUTTON_MIDDLE){ mmb_ = false; nav_panning_  = false; }
        if (e.button.button == SDL_BUTTON_RIGHT) { rmb_ = false; nav_dollying_ = false; if (state_.mode == CameraMode::Fly) end_fly_(eng); }
        // Do not end_orbit_ based on legacy paths; Houdini drag uses update_orbit_drag_
        last_mx_ = mx; last_my_ = my; break; }
    case SDL_EVENT_MOUSE_MOTION: {
        if (imgui_capturing_mouse()) break;
        const int mx = e.motion.x; const int my = e.motion.y; const int dx = e.motion.xrel; const int dy = e.motion.yrel;
        if (houdini_nav_active()) {
            if (nav_orbiting_) {
                update_orbit_drag_(mx, my, false);
            } else if (nav_panning_) {
                update_orbit_drag_(mx, my, true);
            } else if (nav_dollying_) {
                // Dolly using vertical motion; exponential scale for stability
                const float factor = std::exp(dy * 0.01f * (key_shift_ ? 2.0f : 1.0f));
                if (state_.projection == CameraProjection::Perspective) {
                    state_.distance = std::clamp(state_.distance * factor, 1e-4f, 1e6f);
                } else {
                    state_.ortho_height = std::clamp(state_.ortho_height * factor, 1e-4f, 1e6f);
                }
                zoom_vel_ = (factor - 1.0f) * 4.0f;
                last_mx_ = mx; last_my_ = my;
            }
        } else {
            // Orbit mode without Houdini modifiers: ignore mouse motions.
            if (state_.mode == CameraMode::Fly) {
                if (rmb_) update_fly_look_(dx, dy);
            }
        }
        break; }
    case SDL_EVENT_MOUSE_WHEEL: {
        if (imgui_capturing_mouse()) break;
        const float scroll = static_cast<float>(e.wheel.y);
        if (state_.mode == CameraMode::Orbit) {
            float z = std::exp(-scroll * 0.1f * (key_shift_ ? 2.0f : 1.0f));
            if (state_.projection == CameraProjection::Perspective) {
                state_.distance = std::clamp(state_.distance * z, 1e-4f, 1e6f);
            } else {
                state_.ortho_height = std::clamp(state_.ortho_height * z, 1e-4f, 1e6f);
            }
            zoom_vel_ += (-scroll * 0.25f);
        } else {
            // Fly: adjust znear/zfar or speed indirectly (ignore for now)
        }
        break; }
    case SDL_EVENT_KEY_DOWN: {
        if (io && io->WantCaptureKeyboard) break;
        const SDL_Keycode k = e.key.key;
        if (k == SDLK_W) key_w_ = true; if (k == SDLK_A) key_a_ = true; if (k == SDLK_S) key_s_ = true; if (k == SDLK_D) key_d_ = true;
        if (k == SDLK_Q) key_q_ = true; if (k == SDLK_E) key_e_ = true;
        if (k == SDLK_LSHIFT || k == SDLK_RSHIFT) key_shift_ = true;
        if (k == SDLK_LCTRL  || k == SDLK_RCTRL)  key_ctrl_  = true;
        if (k == SDLK_SPACE) key_space_ = true;
        if (k == SDLK_LALT || k == SDLK_RALT) key_alt_ = true;
        // Houdini hotkeys
        if (k == SDLK_H) { home_view(); }
        if (k == SDLK_F) { frame_scene(); }
        break; }
    case SDL_EVENT_KEY_UP: {
        if (io && io->WantCaptureKeyboard) break;
        const SDL_Keycode k = e.key.key;
        if (k == SDLK_W) key_w_ = false; if (k == SDLK_A) key_a_ = false; if (k == SDLK_S) key_s_ = false; if (k == SDLK_D) key_d_ = false;
        if (k == SDLK_Q) key_q_ = false; if (k == SDLK_E) key_e_ = false;
        if (k == SDLK_LSHIFT || k == SDLK_RSHIFT) key_shift_ = false;
        if (k == SDLK_LCTRL  || k == SDLK_RCTRL)  key_ctrl_  = false;
        if (k == SDLK_SPACE) { key_space_ = false; nav_orbiting_ = nav_panning_ = nav_dollying_ = false; }
        if (k == SDLK_LALT || k == SDLK_RALT) { key_alt_ = false; nav_orbiting_ = nav_panning_ = nav_dollying_ = false; }
        break; }
    default: break;
    }
}

void CameraService::add_bookmark(std::string name) { bookmarks_.push_back(CameraBookmarksEntry{std::move(name), state_}); }
bool CameraService::remove_bookmark(std::string_view name) {
    auto it = std::remove_if(bookmarks_.begin(), bookmarks_.end(), [&](const auto& b){ return b.name == name; });
    if (it != bookmarks_.end()) { bookmarks_.erase(it, bookmarks_.end()); return true; }
    return false;
}

void CameraService::start_animation_to_(const CameraState& dst, float duration_sec) {
    anim_from_ = state_;
    anim_to_ = dst;
    anim_dur_ = std::max(0.01f, duration_sec);
    anim_t_ = 0.0f;
    anim_active_ = true;
}

bool CameraService::recall_bookmark(std::string_view name, bool animate, float duration_sec) {
    for (const auto& b : bookmarks_) if (b.name == name) {
        if (animate) start_animation_to_(b.state, duration_sec); else set_state(b.state);
        return true;
    }
    return false;
}

bool CameraService::save_to_file(const std::string& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"mode\":" << (state_.mode==CameraMode::Orbit?0:1) << ",\n";
    oss << "  \"projection\":" << (state_.projection==CameraProjection::Perspective?0:1) << ",\n";
    oss << "  \"units_per_meter\":" << state_.units_per_meter << ",\n";
    oss << "  \"target\":[" << state_.target.x << "," << state_.target.y << "," << state_.target.z << "],\n";
    oss << "  \"distance\":" << state_.distance << ",\n";
    oss << "  \"yaw_deg\":" << state_.yaw_deg << ",\n";
    oss << "  \"pitch_deg\":" << state_.pitch_deg << ",\n";
    oss << "  \"eye\":[" << state_.eye.x << "," << state_.eye.y << "," << state_.eye.z << "],\n";
    oss << "  \"fly_yaw_deg\":" << state_.fly_yaw_deg << ",\n";
    oss << "  \"fly_pitch_deg\":" << state_.fly_pitch_deg << ",\n";
    oss << "  \"fov_y_deg\":" << state_.fov_y_deg << ",\n";
    oss << "  \"ortho_height\":" << state_.ortho_height << ",\n";
    oss << "  \"znear\":" << state_.znear << ",\n";
    oss << "  \"zfar\":" << state_.zfar << "\n";
    oss << "}\n";
    f << oss.str();
    return true;
}

static bool json_seek(std::string_view s, std::string_view key, size_t& pos){ size_t k = s.find(key, pos); if(k==std::string::npos) return false; pos = k + key.size(); return true; }
static float json_read_number(std::string_view s, size_t& pos){ while(pos<s.size() && (s[pos]==' '||s[pos]==':'||s[pos]==','||s[pos]=='['||s[pos]=='\n' || s[pos]=='\r' || s[pos]=='\t')) ++pos; size_t start=pos; while(pos<s.size() && (std::isdigit((unsigned char)s[pos])||s[pos]=='-'||s[pos]=='+'||s[pos]=='.' || s[pos]=='e' || s[pos]=='E')) ++pos; return std::atof(std::string(s.substr(start, pos-start)).c_str()); }

bool CameraService::load_from_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary); if (!f) return false; std::stringstream buf; buf << f.rdbuf(); std::string s = buf.str(); size_t p = 0;
    CameraState ns = state_;
    if (json_seek(s, "\"mode\"", p)) { float v = json_read_number(s, p); ns.mode = (static_cast<int>(v)==0)? CameraMode::Orbit : CameraMode::Fly; }
    if (json_seek(s, "\"projection\"", p)) { float v = json_read_number(s, p); ns.projection = (static_cast<int>(v)==0)? CameraProjection::Perspective : CameraProjection::Orthographic; }
    if (json_seek(s, "\"units_per_meter\"", p)) ns.units_per_meter = json_read_number(s, p);
    if (json_seek(s, "\"target\"", p)) { ns.target.x = json_read_number(s,p); ns.target.y = json_read_number(s,p); ns.target.z = json_read_number(s,p); }
    if (json_seek(s, "\"distance\"", p)) ns.distance = json_read_number(s,p);
    if (json_seek(s, "\"yaw_deg\"", p)) ns.yaw_deg = json_read_number(s,p);
    if (json_seek(s, "\"pitch_deg\"", p)) ns.pitch_deg = json_read_number(s,p);
    if (json_seek(s, "\"eye\"", p)) { ns.eye.x = json_read_number(s,p); ns.eye.y = json_read_number(s,p); ns.eye.z = json_read_number(s,p); }
    if (json_seek(s, "\"fly_yaw_deg\"", p)) ns.fly_yaw_deg = json_read_number(s,p);
    if (json_seek(s, "\"fly_pitch_deg\"", p)) ns.fly_pitch_deg = json_read_number(s,p);
    if (json_seek(s, "\"fov_y_deg\"", p)) ns.fov_y_deg = json_read_number(s,p);
    if (json_seek(s, "\"ortho_height\"", p)) ns.ortho_height = json_read_number(s,p);
    if (json_seek(s, "\"znear\"", p)) ns.znear = json_read_number(s,p);
    if (json_seek(s, "\"zfar\"", p)) ns.zfar = json_read_number(s,p);
    set_state(ns);
    return true;
}

void CameraService::imgui_panel(bool* p_open) {
    if (!ImGui::Begin("Camera", p_open)) { ImGui::End(); return; }
    imgui_panel_contents();
    ImGui::End();
}

void CameraService::imgui_panel_contents() {
    int mode = (state_.mode == CameraMode::Orbit) ? 0 : 1;
    if (ImGui::RadioButton("Orbit", mode==0)) { mode=0; set_mode(CameraMode::Orbit); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Fly", mode==1))   { mode=1; set_mode(CameraMode::Fly); }

    int proj = (state_.projection == CameraProjection::Perspective) ? 0 : 1;
    if (ImGui::RadioButton("Perspective", proj==0)) { proj=0; set_projection(CameraProjection::Perspective); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Orthographic", proj==1)) { proj=1; set_projection(CameraProjection::Orthographic); }

    ImGui::SeparatorText("Params");
    ImGui::DragFloat("FOV Y (deg)", &state_.fov_y_deg, 0.1f, 10.0f, 120.0f);
    ImGui::DragFloat("Ortho Half-Height", &state_.ortho_height, 0.01f, 0.0001f, 1e5f, "%.3f", ImGuiSliderFlags_Logarithmic);
    ImGui::DragFloat("Near", &state_.znear, 0.001f, 1e-5f, state_.zfar-1e-3f, "%.5f", ImGuiSliderFlags_Logarithmic);
    ImGui::DragFloat("Far", &state_.zfar, 0.1f, state_.znear+1e-3f, 1e7f, "%.3f", ImGuiSliderFlags_Logarithmic);
    ImGui::DragFloat("Units per meter", &state_.units_per_meter, 0.0001f, 1e-6f, 1e6f, "%.6f", ImGuiSliderFlags_Logarithmic);

    if (state_.mode == CameraMode::Orbit) {
        ImGui::DragFloat3("Target", &state_.target.x, 0.01f);
        ImGui::DragFloat("Distance", &state_.distance, 0.01f, 1e-4f, 1e6f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::DragFloat("Yaw", &state_.yaw_deg, 0.1f);
        ImGui::DragFloat("Pitch", &state_.pitch_deg, 0.1f);
    } else {
        ImGui::DragFloat3("Eye", &state_.eye.x, 0.01f);
        ImGui::DragFloat("Yaw", &state_.fly_yaw_deg, 0.1f);
        ImGui::DragFloat("Pitch", &state_.fly_pitch_deg, 0.1f);
    }

    ImGui::SeparatorText("Bookmarks");
    static char namebuf[128] = {0};
    ImGui::InputText("Name", namebuf, sizeof(namebuf));
    ImGui::SameLine(); if (ImGui::Button("Add")) { add_bookmark(namebuf); namebuf[0] = '\0'; }
    for (size_t i = 0; i < bookmarks_.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextUnformatted(bookmarks_[i].name.c_str());
        ImGui::SameLine(); if (ImGui::Button("Recall")) { recall_bookmark(bookmarks_[i].name, true, 0.6f); }
        ImGui::SameLine(); if (ImGui::Button("X")) { remove_bookmark(bookmarks_[i].name); ImGui::PopID(); break; }
        ImGui::PopID();
    }

    ImGui::SeparatorText("IO");
    static char pathbuf[260] = "camera.json";
    ImGui::InputText("File", pathbuf, sizeof(pathbuf));
    if (ImGui::Button("Save")) { (void)save_to_file(pathbuf); }
    ImGui::SameLine(); if (ImGui::Button("Load")) { (void)load_from_file(pathbuf); }

    // Quick actions
    if (ImGui::Button("Home (H)")) home_view();
    ImGui::SameLine(); if (ImGui::Button("Frame (F)")) frame_scene();

    recompute_cached_();
}

} // namespace vv
