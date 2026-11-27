#include "gl/gl.hpp"

#include <cmath>
#include <cstring>

#include "host/fail.hpp"

namespace voy::gl {

namespace {

// GL enums used here.
enum : std::uint32_t {
    GL_POINTS = 0, GL_LINES = 1, GL_LINE_LOOP = 2, GL_LINE_STRIP = 3, GL_TRIANGLES = 4, GL_TRIANGLE_STRIP = 5,
    GL_TRIANGLE_FAN = 6, GL_QUADS = 7, GL_QUAD_STRIP = 8, GL_POLYGON = 9,
    GL_FRONT = 0x0404, GL_BACK = 0x0405, GL_FRONT_AND_BACK = 0x0408,
    GL_CULL_FACE = 0x0B44, GL_LIGHTING = 0x0B50, GL_DEPTH_TEST = 0x0B71, GL_BLEND = 0x0BE2, GL_SCISSOR_TEST = 0x0C11,
    GL_TEXTURE_1D = 0x0DE0, GL_TEXTURE_2D = 0x0DE1, GL_NORMALIZE = 0x0BA1, GL_ALPHA_TEST = 0x0BC0, GL_FOG = 0x0B60,
    GL_DITHER = 0x0BD0, GL_COLOR_MATERIAL = 0x0B57, GL_POLYGON_OFFSET_FILL = 0x8037, GL_LIGHT0 = 0x4000,
    GL_LINE_SMOOTH = 0x0B20, GL_POINT_SMOOTH = 0x0B10, GL_POLYGON_SMOOTH = 0x0B41, GL_RESCALE_NORMAL = 0x803A,
    GL_MODELVIEW = 0x1700, GL_PROJECTION = 0x1701, GL_TEXTURE = 0x1702,
    GL_AMBIENT = 0x1200, GL_DIFFUSE = 0x1201, GL_SPECULAR = 0x1202, GL_POSITION = 0x1203, GL_SPOT_DIRECTION = 0x1204,
    GL_SPOT_EXPONENT = 0x1205, GL_SPOT_CUTOFF = 0x1206, GL_CONSTANT_ATTENUATION = 0x1207,
    GL_LINEAR_ATTENUATION = 0x1208, GL_QUADRATIC_ATTENUATION = 0x1209, GL_EMISSION = 0x1600, GL_SHININESS = 0x1601,
    GL_AMBIENT_AND_DIFFUSE = 0x1602, GL_LIGHT_MODEL_LOCAL_VIEWER = 0x0B51, GL_LIGHT_MODEL_TWO_SIDE = 0x0B52,
    GL_LIGHT_MODEL_AMBIENT = 0x0B53,
    GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x0100,
    GL_TEXTURE_ENV_MODE = 0x2200, GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_SHORT_5_6_5 = 0x8363, GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033,
    GL_UNSIGNED_SHORT_5_5_5_1 = 0x8034, GL_UNSIGNED_SHORT_4_4_4_4_REV = 0x8365, GL_UNSIGNED_SHORT_1_5_5_5_REV = 0x8366,
    GL_UNSIGNED_SHORT_5_6_5_REV = 0x8364,
    GL_ALPHA = 0x1906, GL_RGB = 0x1907, GL_RGBA = 0x1908, GL_LUMINANCE = 0x1909, GL_LUMINANCE_ALPHA = 0x190A,
    GL_BGR = 0x80E0, GL_BGRA = 0x80E1, GL_INTENSITY = 0x8049,
    GL_MODELVIEW_MATRIX = 0x0BA6, GL_PROJECTION_MATRIX = 0x0BA7, GL_VIEWPORT = 0x0BA2, GL_MAX_TEXTURE_SIZE = 0x0D33,
    GL_CURRENT_COLOR = 0x0B00, GL_DEPTH_RANGE = 0x0B70, GL_COLOR_CLEAR_VALUE = 0x0C22, GL_SCISSOR_BOX = 0x0C10,
    GL_TEXTURE_MATRIX = 0x0BA8, GL_MATRIX_MODE = 0x0BA0, GL_DEPTH_BITS = 0x0D56, GL_RED_BITS = 0x0D52,
    GL_GREEN_BITS = 0x0D53, GL_BLUE_BITS = 0x0D54, GL_ALPHA_BITS = 0x0D55,
};

using Mat = std::array<double, 16>;

Mat identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Mat mul(const Mat& a, const Mat& b) {
    Mat r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    return r;
}

void xform(const Mat& m, const double in[4], double out[4]) {
    for (int row = 0; row < 4; ++row)
        out[row] = m[row] * in[0] + m[4 + row] * in[1] + m[8 + row] * in[2] + m[12 + row] * in[3];
}

// The inverse transpose's upper 3x3 for normals (cofactors of the upper 3x3).
void normal_matrix(const Mat& m, double n[9]) {
    const double a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double D = -(b * i - c * h), E = a * i - c * g, F = -(a * h - b * g);
    const double G = b * f - c * e, H = -(a * f - c * d), I = a * e - b * d;
    double det = a * A + b * B + c * C;
    if (det == 0) det = 1;
    // (M^-1)^T = cofactor matrix / det
    n[0] = A / det; n[1] = B / det; n[2] = C / det;
    n[3] = D / det; n[4] = E / det; n[5] = F / det;
    n[6] = G / det; n[7] = H / det; n[8] = I / det;
}

std::uint64_t fnv(std::uint64_t h, const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

}  // namespace

bool DrawState::same_as(const DrawState& o) const {
    return tex == o.tex && env_mode == o.env_mode && blend == o.blend && blend_src == o.blend_src &&
           blend_dst == o.blend_dst && depth_test == o.depth_test && depth_func == o.depth_func &&
           depth_write == o.depth_write && color_mask == o.color_mask && scissor == o.scissor &&
           std::memcmp(scissor_box, o.scissor_box, sizeof scissor_box) == 0 &&
           std::memcmp(viewport, o.viewport, sizeof viewport) == 0 && polygon_offset == o.polygon_offset &&
           offset_factor == o.offset_factor && offset_units == o.offset_units && alpha_test == o.alpha_test &&
           alpha_func == o.alpha_func && alpha_ref == o.alpha_ref;
}

Context::Context() { reset(); }

void Context::reset() {
    modelview_.assign(1, identity());
    projection_.assign(1, identity());
    texture_.assign(1, identity());
    matrix_mode_ = GL_MODELVIEW;
    state_ = DrawState{};
    cull_ = lighting_ = normalize_ = flat_ = tex2d_ = tex1d_ = two_side_ = local_viewer_ = false;
    lights_ = {};
    lights_[0].diffuse = {1, 1, 1, 1};
    lights_[0].specular = {1, 1, 1, 1};
    material_[0] = material_[1] = MaterialState{};
    textures_.clear();
    bound_2d_ = bound_1d_ = 0;
    env_mode_ = 0x2100;
    color_ = {1, 1, 1, 1};
    frame_.reset();
}

Context::Mat& Context::top() {
    if (matrix_mode_ == GL_PROJECTION) return projection_.back();
    if (matrix_mode_ == GL_TEXTURE) return texture_.back();
    return modelview_.back();
}

void Context::mult(const Mat& m) { top() = mul(top(), m); }
void Context::LoadIdentity() { top() = identity(); }

void Context::PushMatrix() {
    auto& s = matrix_mode_ == GL_PROJECTION ? projection_ : matrix_mode_ == GL_TEXTURE ? texture_ : modelview_;
    if (s.size() >= 64) {
        host::note_once("gl", "matrix stack overflow");
        return;
    }
    s.push_back(s.back());
}

void Context::PopMatrix() {
    auto& s = matrix_mode_ == GL_PROJECTION ? projection_ : matrix_mode_ == GL_TEXTURE ? texture_ : modelview_;
    if (s.size() <= 1) {
        host::note_once("gl", "matrix stack underflow");
        return;
    }
    s.pop_back();
}

void Context::MultMatrix(const double m[16]) {
    Mat a;
    std::memcpy(a.data(), m, sizeof a);
    mult(a);
}

void Context::Translate(double x, double y, double z) {
    Mat m = identity();
    m[12] = x; m[13] = y; m[14] = z;
    mult(m);
}

void Context::Scale(double x, double y, double z) {
    Mat m = identity();
    m[0] = x; m[5] = y; m[10] = z;
    mult(m);
}

void Context::Rotate(double angle, double x, double y, double z) {
    const double len = std::sqrt(x * x + y * y + z * z);
    if (len == 0) return;
    x /= len; y /= len; z /= len;
    const double a = angle * M_PI / 180.0, c = std::cos(a), s = std::sin(a), t = 1 - c;
    Mat m = identity();
    m[0] = x * x * t + c;     m[4] = x * y * t - z * s; m[8] = x * z * t + y * s;
    m[1] = y * x * t + z * s; m[5] = y * y * t + c;     m[9] = y * z * t - x * s;
    m[2] = x * z * t - y * s; m[6] = y * z * t + x * s; m[10] = z * z * t + c;
    mult(m);
}

void Context::Frustum(double l, double r, double b, double t, double n, double f) {
    Mat m{};
    m[0] = 2 * n / (r - l);
    m[5] = 2 * n / (t - b);
    m[8] = (r + l) / (r - l);
    m[9] = (t + b) / (t - b);
    m[10] = -(f + n) / (f - n);
    m[11] = -1;
    m[14] = -2 * f * n / (f - n);
    mult(m);
}

void Context::Ortho(double l, double r, double b, double t, double n, double f) {
    Mat m = identity();
    m[0] = 2 / (r - l);
    m[5] = 2 / (t - b);
    m[10] = -2 / (f - n);
    m[12] = -(r + l) / (r - l);
    m[13] = -(t + b) / (t - b);
    m[14] = -(f + n) / (f - n);
    mult(m);
}

void Context::Perspective(double fovy, double aspect, double n, double f) {
    const double ymax = n * std::tan(fovy * M_PI / 360.0), xmax = ymax * aspect;
    Frustum(-xmax, xmax, -ymax, ymax, n, f);
}

void Context::LookAt(const double e[3], const double c[3], const double u[3]) {
    double fw[3] = {c[0] - e[0], c[1] - e[1], c[2] - e[2]};
    double l = std::sqrt(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
    if (l == 0) return;
    for (double& v : fw) v /= l;
    double s[3] = {fw[1] * u[2] - fw[2] * u[1], fw[2] * u[0] - fw[0] * u[2], fw[0] * u[1] - fw[1] * u[0]};
    l = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (l != 0)
        for (double& v : s) v /= l;
    double up[3] = {s[1] * fw[2] - s[2] * fw[1], s[2] * fw[0] - s[0] * fw[2], s[0] * fw[1] - s[1] * fw[0]};
    Mat m = identity();
    m[0] = s[0]; m[4] = s[1]; m[8] = s[2];
    m[1] = up[0]; m[5] = up[1]; m[9] = up[2];
    m[2] = -fw[0]; m[6] = -fw[1]; m[10] = -fw[2];
    mult(m);
    Translate(-e[0], -e[1], -e[2]);
}

void Context::Viewport(int x, int y, int w, int h) {
    state_.viewport[0] = x; state_.viewport[1] = y; state_.viewport[2] = w; state_.viewport[3] = h;
}

void Context::Scissor(int x, int y, int w, int h) {
    state_.scissor_box[0] = x; state_.scissor_box[1] = y; state_.scissor_box[2] = w; state_.scissor_box[3] = h;
}

void Context::Enable(std::uint32_t cap, bool on) {
    switch (cap) {
    case GL_CULL_FACE: cull_ = on; break;
    case GL_LIGHTING: lighting_ = on; break;
    case GL_DEPTH_TEST: state_.depth_test = on; break;
    case GL_BLEND: state_.blend = on; break;
    case GL_SCISSOR_TEST: state_.scissor = on; break;
    case GL_TEXTURE_2D: tex2d_ = on; break;
    case GL_TEXTURE_1D: tex1d_ = on; break;
    case GL_NORMALIZE: case GL_RESCALE_NORMAL: normalize_ = on; break;
    case GL_POLYGON_OFFSET_FILL: state_.polygon_offset = on; break;
    case GL_ALPHA_TEST: state_.alpha_test = on; break;
    case GL_DITHER: case GL_LINE_SMOOTH: case GL_POINT_SMOOTH: case GL_POLYGON_SMOOTH: break;
    default:
        if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + 8) {
            lights_[cap - GL_LIGHT0].on = on;
            break;
        }
        {
            char buf[48];
            std::snprintf(buf, sizeof buf, "glEnable / glDisable 0x%04X ignored", cap);
            host::note_once("gl", buf);
        }
    }
}

void Context::Light(std::uint32_t light, std::uint32_t pname, const float* v) {
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + 8) return;
    auto& L = lights_[light - GL_LIGHT0];
    const Mat& mv = modelview_.back();
    switch (pname) {
    case GL_AMBIENT: std::memcpy(L.ambient.data(), v, 16); break;
    case GL_DIFFUSE: std::memcpy(L.diffuse.data(), v, 16); break;
    case GL_SPECULAR: std::memcpy(L.specular.data(), v, 16); break;
    case GL_POSITION: {
        const double in[4] = {v[0], v[1], v[2], v[3]};
        double out[4];
        xform(mv, in, out);
        L.position = {float(out[0]), float(out[1]), float(out[2]), float(out[3])};
        break;
    }
    case GL_SPOT_DIRECTION: {
        const double in[4] = {v[0], v[1], v[2], 0};
        double out[4];
        xform(mv, in, out);
        L.spot_dir = {float(out[0]), float(out[1]), float(out[2])};
        break;
    }
    case GL_SPOT_EXPONENT: L.spot_exp = v[0]; break;
    case GL_SPOT_CUTOFF: L.spot_cutoff = v[0]; break;
    case GL_CONSTANT_ATTENUATION: L.att[0] = v[0]; break;
    case GL_LINEAR_ATTENUATION: L.att[1] = v[0]; break;
    case GL_QUADRATIC_ATTENUATION: L.att[2] = v[0]; break;
    default: host::note_once("gl", "glLightfv pname " + std::to_string(pname));
    }
}

void Context::LightModel(std::uint32_t pname, const float* v) {
    switch (pname) {
    case GL_LIGHT_MODEL_AMBIENT: std::memcpy(model_ambient_.data(), v, 16); break;
    case GL_LIGHT_MODEL_TWO_SIDE: two_side_ = v[0] != 0; break;
    case GL_LIGHT_MODEL_LOCAL_VIEWER: local_viewer_ = v[0] != 0; break;
    default: host::note_once("gl", "glLightModelfv pname " + std::to_string(pname));
    }
}

void Context::Material(std::uint32_t face, std::uint32_t pname, const float* v) {
    for (int k = 0; k < 2; ++k) {
        if (k == 0 && face == GL_BACK) continue;
        if (k == 1 && face == GL_FRONT) continue;
        auto& m = material_[k];
        switch (pname) {
        case GL_AMBIENT: std::memcpy(m.ambient.data(), v, 16); break;
        case GL_DIFFUSE: std::memcpy(m.diffuse.data(), v, 16); break;
        case GL_AMBIENT_AND_DIFFUSE: std::memcpy(m.ambient.data(), v, 16); std::memcpy(m.diffuse.data(), v, 16); break;
        case GL_SPECULAR: std::memcpy(m.specular.data(), v, 16); break;
        case GL_EMISSION: std::memcpy(m.emission.data(), v, 16); break;
        case GL_SHININESS: m.shininess = v[0]; break;
        default: host::note_once("gl", "glMaterialfv pname " + std::to_string(pname));
        }
    }
}

std::array<float, 4> Context::light_vertex(const std::array<float, 3>& eye, const std::array<float, 3>& n) const {
    const auto& m = material_[0];
    float r = m.emission[0] + m.ambient[0] * model_ambient_[0];
    float g = m.emission[1] + m.ambient[1] * model_ambient_[1];
    float b = m.emission[2] + m.ambient[2] * model_ambient_[2];
    for (const auto& L : lights_) {
        if (!L.on) continue;
        float lx, ly, lz, att = 1;
        if (L.position[3] == 0) {
            lx = L.position[0]; ly = L.position[1]; lz = L.position[2];
        } else {
            lx = L.position[0] / L.position[3] - eye[0];
            ly = L.position[1] / L.position[3] - eye[1];
            lz = L.position[2] / L.position[3] - eye[2];
            const float d = std::sqrt(lx * lx + ly * ly + lz * lz);
            att = 1.0f / (L.att[0] + L.att[1] * d + L.att[2] * d * d);
        }
        const float ll = std::sqrt(lx * lx + ly * ly + lz * lz);
        if (ll > 0) { lx /= ll; ly /= ll; lz /= ll; }
        if (L.spot_cutoff != 180) {
            const float sl = std::sqrt(L.spot_dir[0] * L.spot_dir[0] + L.spot_dir[1] * L.spot_dir[1] + L.spot_dir[2] * L.spot_dir[2]);
            const float c = sl > 0 ? -(lx * L.spot_dir[0] + ly * L.spot_dir[1] + lz * L.spot_dir[2]) / sl : 0;
            if (c < std::cos(L.spot_cutoff * float(M_PI) / 180)) att = 0;
            else att *= std::pow(std::max(c, 0.0f), L.spot_exp);
        }
        if (att == 0) continue;
        const float ndl = n[0] * lx + n[1] * ly + n[2] * lz;
        float rr = L.ambient[0] * m.ambient[0], gg = L.ambient[1] * m.ambient[1], bb = L.ambient[2] * m.ambient[2];
        if (ndl > 0) {
            rr += ndl * L.diffuse[0] * m.diffuse[0];
            gg += ndl * L.diffuse[1] * m.diffuse[1];
            bb += ndl * L.diffuse[2] * m.diffuse[2];
            float hx = lx, hy = ly, hz = lz + 1;
            if (local_viewer_) {
                const float el = std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
                if (el > 0) { hx = lx - eye[0] / el; hy = ly - eye[1] / el; hz = lz - eye[2] / el; }
            }
            const float hl = std::sqrt(hx * hx + hy * hy + hz * hz);
            const float ndh = hl > 0 ? (n[0] * hx + n[1] * hy + n[2] * hz) / hl : 0;
            if (ndh > 0) {
                const float sp = m.shininess > 0 ? std::pow(ndh, m.shininess) : 1;
                rr += sp * L.specular[0] * m.specular[0];
                gg += sp * L.specular[1] * m.specular[1];
                bb += sp * L.specular[2] * m.specular[2];
            }
        }
        r += att * rr; g += att * gg; b += att * bb;
    }
    auto clamp = [](float v) { return v < 0 ? 0.0f : v > 1 ? 1.0f : v; };
    return {clamp(r), clamp(g), clamp(b), clamp(m.diffuse[3])};
}

void Context::Begin(std::uint32_t mode) {
    begin_mode_ = mode;
    prim_.clear();
}

void Context::Vertex4(float x, float y, float z, float w) {
    if (begin_mode_ == 0xFFFFFFFF) {
        host::note_once("gl", "glVertex outside glBegin / glEnd");
        return;
    }
    const Mat& mv = modelview_.back();
    const double in[4] = {x, y, z, w};
    double eye[4], clip[4];
    xform(mv, in, eye);
    xform(projection_.back(), eye, clip);
    Pending v;
    v.cx = float(clip[0]);
    v.cy = float(clip[1]);
    v.cz = float(0.5 * (clip[2] + clip[3]));  // GL's -w..w depth to Metal's 0..w
    v.cw = float(clip[3]);
    if (lighting_) {
        double nm[9];
        normal_matrix(mv, nm);
        std::array<float, 3> n = {float(nm[0] * normal_[0] + nm[1] * normal_[1] + nm[2] * normal_[2]),
                                  float(nm[3] * normal_[0] + nm[4] * normal_[1] + nm[5] * normal_[2]),
                                  float(nm[6] * normal_[0] + nm[7] * normal_[1] + nm[8] * normal_[2])};
        const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl > 0) { n[0] /= nl; n[1] /= nl; n[2] /= nl; }
        const float ew = eye[3] != 0 ? float(eye[3]) : 1.0f;
        const auto col = light_vertex({float(eye[0] / ew), float(eye[1] / ew), float(eye[2] / ew)}, n);
        v.r = col[0]; v.g = col[1]; v.b = col[2]; v.a = col[3];
    } else {
        v.r = color_[0]; v.g = color_[1]; v.b = color_[2]; v.a = color_[3];
    }
    const Mat& tm = texture_.back();
    if (tm != identity()) {
        const double tin[4] = {texcoord_[0], texcoord_[1], 0, 1};
        double tout[4];
        xform(tm, tin, tout);
        const double q = tout[3] != 0 ? tout[3] : 1;
        v.s = float(tout[0] / q);
        v.t = float(tout[1] / q);
    } else {
        v.s = texcoord_[0];
        v.t = texcoord_[1];
    }
    prim_.push_back(v);
}

DrawState Context::current_draw_state() const {
    DrawState s = state_;
    s.env_mode = env_mode_;
    std::uint32_t id = tex2d_ ? bound_2d_ : tex1d_ ? bound_1d_ : 0;
    if (id) {
        auto it = textures_.find(id);
        if (it != textures_.end() && it->second && it->second->width > 0) s.tex = it->second;
    }
    return s;
}

void Context::begin_frame_if_needed() {
    if (!frame_) {
        frame_ = std::make_shared<Frame>();
        frame_->vertices.reserve(65536);
    }
}

void Context::emit(Prim prim, const Pending* v, int n) {
    begin_frame_if_needed();
    auto& cmds = frame_->commands;
    DrawState s = current_draw_state();
    if (cmds.empty() || cmds.back().kind != Command::Draw || cmds.back().prim != prim || !cmds.back().state.same_as(s) ||
        cmds.back().first + cmds.back().count != frame_->vertices.size()) {
        Command c;
        c.kind = Command::Draw;
        c.prim = prim;
        c.state = std::move(s);
        c.first = std::uint32_t(frame_->vertices.size());
        cmds.push_back(std::move(c));
    }
    for (int i = 0; i < n; ++i)
        frame_->vertices.push_back({v[i].cx, v[i].cy, v[i].cz, v[i].cw, v[i].r, v[i].g, v[i].b, v[i].a, v[i].s, v[i].t});
    cmds.back().count += std::uint32_t(n);
}

void Context::End() {
    flush_primitive();
    begin_mode_ = 0xFFFFFFFF;
}

void Context::flush_primitive() {
    const int n = int(prim_.size());
    const Pending* v = prim_.data();
    auto tri = [&](int a, int b, int c, int provoking) {
        Pending t[3] = {v[a], v[b], v[c]};
        if (cull_) {
            if (cull_face_ == GL_FRONT_AND_BACK) return;
            const double det = double(t[0].cx) * (double(t[1].cy) * t[2].cw - double(t[2].cy) * t[1].cw) -
                               double(t[0].cy) * (double(t[1].cx) * t[2].cw - double(t[2].cx) * t[1].cw) +
                               double(t[0].cw) * (double(t[1].cx) * t[2].cy - double(t[2].cx) * t[1].cy);
            const bool front = det > 0;  // counter-clockwise
            if (cull_face_ == GL_BACK && !front) return;
            if (cull_face_ == GL_FRONT && front) return;
        }
        if (flat_) {
            for (auto& x : t) { x.r = v[provoking].r; x.g = v[provoking].g; x.b = v[provoking].b; x.a = v[provoking].a; }
        }
        emit(Prim::Triangles, t, 3);
    };
    switch (begin_mode_) {
    case GL_TRIANGLES:
        for (int i = 0; i + 2 < n; i += 3) tri(i, i + 1, i + 2, i + 2);
        break;
    case GL_TRIANGLE_STRIP:
        for (int i = 0; i + 2 < n; ++i) {
            if (i & 1) tri(i + 1, i, i + 2, i + 2);
            else tri(i, i + 1, i + 2, i + 2);
        }
        break;
    case GL_TRIANGLE_FAN:
        for (int i = 1; i + 1 < n; ++i) tri(0, i, i + 1, i + 1);
        break;
    case GL_QUADS:
        for (int i = 0; i + 3 < n; i += 4) {
            tri(i, i + 1, i + 2, i + 3);
            tri(i, i + 2, i + 3, i + 3);
        }
        break;
    case GL_QUAD_STRIP:
        for (int i = 0; i + 3 < n; i += 2) {
            tri(i, i + 1, i + 3, i + 3);
            tri(i, i + 3, i + 2, i + 3);
        }
        break;
    case GL_POLYGON:
        for (int i = 1; i + 1 < n; ++i) tri(0, i, i + 1, 0);
        break;
    case GL_LINES:
        for (int i = 0; i + 1 < n; i += 2) emit(Prim::Lines, v + i, 2);
        break;
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
        for (int i = 0; i + 1 < n; ++i) emit(Prim::Lines, v + i, 2);
        if (begin_mode_ == GL_LINE_LOOP && n > 2) {
            Pending l[2] = {v[n - 1], v[0]};
            emit(Prim::Lines, l, 2);
        }
        break;
    case GL_POINTS:
        for (int i = 0; i < n; ++i) emit(Prim::Points, v + i, 1);
        break;
    default:
        host::note_once("gl", "glBegin mode " + std::to_string(begin_mode_));
    }
    prim_.clear();
}

void Context::Rect(float x1, float y1, float x2, float y2) {
    Begin(GL_POLYGON);
    Vertex4(x1, y1, 0, 1);
    Vertex4(x2, y1, 0, 1);
    Vertex4(x2, y2, 0, 1);
    Vertex4(x1, y2, 0, 1);
    End();
}

void Context::Clear(std::uint32_t mask) {
    begin_frame_if_needed();
    Command c;
    c.kind = Command::Clear;
    c.state = state_;
    c.state.tex.reset();
    c.clear_mask = mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    std::memcpy(c.clear_color, clear_color_.data(), 16);
    c.clear_depth = clear_depth_;
    frame_->commands.push_back(std::move(c));
}

void Context::swap() {
    begin_frame_if_needed();
    frame_->index = frame_index_++;
    std::uint64_t h = 1469598103934665603ull;
    for (const auto& c : frame_->commands) {
        h = fnv(h, &c.kind, 1);
        h = fnv(h, &c.prim, 1);
        h = fnv(h, &c.count, 4);
        h = fnv(h, &c.clear_mask, 4);
        const std::uint64_t ts = c.state.tex ? std::uint64_t(c.state.tex->width) << 32 | std::uint64_t(c.state.tex->height) : 0;
        h = fnv(h, &ts, 8);
    }
    if (!frame_->vertices.empty()) h = fnv(h, frame_->vertices.data(), frame_->vertices.size() * sizeof(Vertex));
    last_hash_ = h;
    std::shared_ptr<const Frame> done = std::move(frame_);
    frame_.reset();
    if (sink_) sink_->submit(std::move(done));
}

// ---- textures ----

std::shared_ptr<gl::TexImage>& Context::tex_slot(std::uint32_t id) {
    auto& t = textures_[id];
    if (!t) t = std::make_shared<gl::TexImage>();
    return t;
}

// A texture the game is about to change: copied first when a frame still holds it.
std::shared_ptr<gl::TexImage> Context::writable(std::uint32_t id) {
    auto& t = tex_slot(id);
    if (t.use_count() > 1) t = std::make_shared<gl::TexImage>(*t);
    t->serial = serial_++;
    return t;
}

void Context::BindTexture(std::uint32_t target, std::uint32_t id) {
    if (target == GL_TEXTURE_1D) bound_1d_ = id;
    else bound_2d_ = id;
    if (id) tex_slot(id);
}

void Context::DeleteTextures(int n, const std::uint32_t* ids) {
    for (int i = 0; i < n; ++i) {
        textures_.erase(ids[i]);
        if (bound_2d_ == ids[i]) bound_2d_ = 0;
        if (bound_1d_ == ids[i]) bound_1d_ = 0;
    }
}

void Context::TexParameter(std::uint32_t target, std::uint32_t pname, std::uint32_t value) {
    const std::uint32_t id = target == GL_TEXTURE_1D ? bound_1d_ : bound_2d_;
    auto t = writable(id);
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: t->min_filter = value; break;
    case GL_TEXTURE_MAG_FILTER: t->mag_filter = value; break;
    case GL_TEXTURE_WRAP_S: t->wrap_s = value; break;
    case GL_TEXTURE_WRAP_T: t->wrap_t = value; break;
    default: host::note_once("gl", "glTexParameteri pname " + std::to_string(pname));
    }
}

void Context::TexEnv(std::uint32_t pname, float value) {
    if (pname == GL_TEXTURE_ENV_MODE) env_mode_ = std::uint32_t(value);
    else host::note_once("gl", "glTexEnvf pname " + std::to_string(pname));
}

void Context::PixelStore(std::uint32_t pname, int value) {
    if (pname == GL_UNPACK_ALIGNMENT) unpack_alignment_ = value > 0 ? value : 1;
}

namespace {

int base_format(int internal, std::uint32_t format) {
    switch (internal) {
    case 1: case GL_LUMINANCE: case 0x8040: return 3;
    case 2: case GL_LUMINANCE_ALPHA: return 4;
    case 3: case GL_RGB: case 0x8051: case 0x804F: case 0x8050: case 0x8052: case 0x8053: return 1;
    case 4: case GL_RGBA: case 0x8058: case 0x8056: case 0x8057: case 0x8059: case 0x805A: return 0;
    case GL_ALPHA: case 0x803C: return 2;
    case GL_INTENSITY: case 0x804B: return 5;
    default: break;
    }
    if (format == GL_ALPHA) return 2;
    if (format == GL_LUMINANCE) return 3;
    if (format == GL_RGB || format == GL_BGR) return 1;
    return 0;
}

// Converts one row of client pixels to RGBA8. False for an unsupported layout.
bool convert_row(std::uint32_t format, std::uint32_t type, const std::uint8_t* src, std::uint8_t* dst, int w) {
    if (type == GL_UNSIGNED_BYTE) {
        for (int x = 0; x < w; ++x, dst += 4) {
            switch (format) {
            case GL_RGBA: dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3]; src += 4; break;
            case GL_BGRA: dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3]; src += 4; break;
            case GL_RGB: dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255; src += 3; break;
            case GL_BGR: dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = 255; src += 3; break;
            case GL_LUMINANCE: dst[0] = dst[1] = dst[2] = src[0]; dst[3] = 255; src += 1; break;
            case GL_ALPHA: dst[0] = dst[1] = dst[2] = 255; dst[3] = src[0]; src += 1; break;
            case GL_LUMINANCE_ALPHA: dst[0] = dst[1] = dst[2] = src[0]; dst[3] = src[1]; src += 2; break;
            default: return false;
            }
        }
        return true;
    }
    if (type == GL_UNSIGNED_SHORT_4_4_4_4_REV || type == GL_UNSIGNED_SHORT_1_5_5_5_REV || type == GL_UNSIGNED_SHORT_5_6_5_REV) {
        // Packed little-endian shorts, components from the low bits up: with
        // BGRA, 4444_REV is A R G B from the top; 1555_REV is A then R G B.
        for (int x = 0; x < w; ++x, dst += 4, src += 2) {
            const std::uint16_t v = std::uint16_t(src[0] | src[1] << 8);
            std::uint8_t c0, c1, c2, a;  // low, mid, high components and alpha
            if (type == GL_UNSIGNED_SHORT_4_4_4_4_REV) {
                c0 = std::uint8_t((v & 15) * 17); c1 = std::uint8_t(((v >> 4) & 15) * 17);
                c2 = std::uint8_t(((v >> 8) & 15) * 17); a = std::uint8_t((v >> 12) * 17);
            } else if (type == GL_UNSIGNED_SHORT_1_5_5_5_REV) {
                c0 = std::uint8_t((v & 31) * 255 / 31); c1 = std::uint8_t(((v >> 5) & 31) * 255 / 31);
                c2 = std::uint8_t(((v >> 10) & 31) * 255 / 31); a = (v >> 15) ? 255 : 0;
            } else {
                c0 = std::uint8_t((v & 31) * 255 / 31); c1 = std::uint8_t(((v >> 5) & 63) * 255 / 63);
                c2 = std::uint8_t((v >> 11) * 255 / 31); a = 255;
            }
            // c0 is the first component of the format: R for RGBA / RGB, B for BGRA.
            if (format == GL_BGRA || format == GL_BGR) { dst[0] = c2; dst[1] = c1; dst[2] = c0; }
            else { dst[0] = c0; dst[1] = c1; dst[2] = c2; }
            dst[3] = a;
        }
        return true;
    }
    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) {
        for (int x = 0; x < w; ++x, dst += 4, src += 2) {
            const std::uint16_t v = std::uint16_t(src[0] | src[1] << 8);
            if (type == GL_UNSIGNED_SHORT_5_6_5) {
                dst[0] = std::uint8_t((v >> 11) * 255 / 31); dst[1] = std::uint8_t(((v >> 5) & 63) * 255 / 63);
                dst[2] = std::uint8_t((v & 31) * 255 / 31); dst[3] = 255;
            } else if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
                dst[0] = std::uint8_t((v >> 12) * 17); dst[1] = std::uint8_t(((v >> 8) & 15) * 17);
                dst[2] = std::uint8_t(((v >> 4) & 15) * 17); dst[3] = std::uint8_t((v & 15) * 17);
            } else {
                dst[0] = std::uint8_t((v >> 11) * 255 / 31); dst[1] = std::uint8_t(((v >> 6) & 31) * 255 / 31);
                dst[2] = std::uint8_t(((v >> 1) & 31) * 255 / 31); dst[3] = (v & 1) ? 255 : 0;
            }
        }
        return format == GL_RGB || format == GL_RGBA;
    }
    return false;
}

int bytes_per_pixel(std::uint32_t format, std::uint32_t type) {
    if (type != GL_UNSIGNED_BYTE) return 2;
    switch (format) {
    case GL_RGBA: case GL_BGRA: return 4;
    case GL_RGB: case GL_BGR: return 3;
    case GL_LUMINANCE_ALPHA: return 2;
    default: return 1;
    }
}

}  // namespace

bool Context::TexImage(std::uint32_t target, int level, int internal, int w, int h, std::uint32_t format, std::uint32_t type,
                       const std::uint8_t* pixels, bool build_mipmaps) {
    if (level != 0) return true;  // mipmaps are generated from level 0
    const std::uint32_t id = target == GL_TEXTURE_1D ? bound_1d_ : bound_2d_;
    auto t = writable(id);
    t->width = w;
    t->height = h;
    t->format = base_format(internal, format);
    t->mipmaps = t->mipmaps || build_mipmaps;
    t->rgba.assign(std::size_t(w) * h * 4, 255);
    if (!pixels) return true;
    const int bpp = bytes_per_pixel(format, type);
    const int stride = ((w * bpp + unpack_alignment_ - 1) / unpack_alignment_) * unpack_alignment_;
    for (int y = 0; y < h; ++y)
        if (!convert_row(format, type, pixels + std::size_t(y) * stride, t->rgba.data() + std::size_t(y) * w * 4, w))
            return false;
    return true;
}

bool Context::TexSubImage(std::uint32_t target, int level, int x0, int y0, int w, int h, std::uint32_t format,
                          std::uint32_t type, const std::uint8_t* pixels) {
    if (level != 0) return true;
    const std::uint32_t id = target == GL_TEXTURE_1D ? bound_1d_ : bound_2d_;
    auto t = writable(id);
    if (x0 < 0 || y0 < 0 || x0 + w > t->width || y0 + h > t->height) {
        host::note_once("gl", "glTexSubImage2D outside the texture");
        return true;
    }
    const int bpp = bytes_per_pixel(format, type);
    const int stride = ((w * bpp + unpack_alignment_ - 1) / unpack_alignment_) * unpack_alignment_;
    for (int y = 0; y < h; ++y)
        if (!convert_row(format, type, pixels + std::size_t(y) * stride,
                         t->rgba.data() + (std::size_t(y0 + y) * t->width + x0) * 4, w))
            return false;
    return true;
}

bool Context::GetFloat(std::uint32_t pname, float* out, int* count) {
    auto put_mat = [&](const Mat& m) {
        for (int i = 0; i < 16; ++i) out[i] = float(m[i]);
        *count = 16;
    };
    switch (pname) {
    case GL_MODELVIEW_MATRIX: put_mat(modelview_.back()); return true;
    case GL_PROJECTION_MATRIX: put_mat(projection_.back()); return true;
    case GL_TEXTURE_MATRIX: put_mat(texture_.back()); return true;
    case GL_VIEWPORT: for (int i = 0; i < 4; ++i) out[i] = float(state_.viewport[i]); *count = 4; return true;
    case GL_SCISSOR_BOX: for (int i = 0; i < 4; ++i) out[i] = float(state_.scissor_box[i]); *count = 4; return true;
    case GL_MAX_TEXTURE_SIZE: out[0] = 2048; *count = 1; return true;
    case GL_CURRENT_COLOR: for (int i = 0; i < 4; ++i) out[i] = color_[i]; *count = 4; return true;
    case GL_COLOR_CLEAR_VALUE: for (int i = 0; i < 4; ++i) out[i] = clear_color_[i]; *count = 4; return true;
    case GL_DEPTH_RANGE: out[0] = 0; out[1] = 1; *count = 2; return true;
    case GL_MATRIX_MODE: out[0] = float(matrix_mode_); *count = 1; return true;
    case GL_DEPTH_BITS: out[0] = 24; *count = 1; return true;
    case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: out[0] = 8; *count = 1; return true;
    default: return false;
    }
}

}
