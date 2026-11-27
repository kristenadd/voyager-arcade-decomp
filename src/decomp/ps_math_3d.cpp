// ps_math_3d.c: vector and matrix helpers (leaf functions first).
#include <cmath>

#include "decomp/decomp.hpp"

namespace voy::decomp {

using host::Args;
using host::ret_fp;
using host::ret_void;
using recomp::ld32;
using recomp::st32;

// float dot_product(Vector3f* v1, Vector3f* v2)  ps_math_3d.c:459
VOY_DECOMP(trek, dot_product) {
    Args a{c};
    const Vec3 &p = V3(c, a.u(0)), &q = V3(c, a.u(1));
    ret_fp(c, double(p.z) * q.z + double(p.y) * q.y + double(p.x) * q.x);
}

// float vector_magnitude(Vector3f* v1)
VOY_DECOMP(trek, vector_magnitude) {
    const Vec3& v = V3(c, Args{c}.u(0));
    ret_fp(c, std::sqrt(double(v.z) * v.z + double(v.y) * v.y + double(v.x) * v.x));
}

// void stuff_a_to_b(Vector3f* dest, Vector3f* a, Vector3f* b): dest = b - a
VOY_DECOMP(trek, stuff_a_to_b) {
    Args a{c};
    Vec3& d = V3(c, a.u(0));
    const Vec3 p = V3(c, a.u(1)), q = V3(c, a.u(2));
    d.x = q.x - p.x;
    d.y = q.y - p.y;
    d.z = q.z - p.z;
    ret_void(c);
}

// void accumulate_xyz(Vector3f* acc, Vector3f* delta)
VOY_DECOMP(trek, accumulate_xyz) {
    Args a{c};
    Vec3& d = V3(c, a.u(0));
    const Vec3 s = V3(c, a.u(1));
    d.x += s.x;
    d.y += s.y;
    d.z += s.z;
    ret_void(c);
}

// void stuff_scaled_vector(Vector3f* dest, float scale, Vector3f* a)
VOY_DECOMP(trek, stuff_scaled_vector) {
    Args a{c};
    const float k = a.f(1);
    const Vec3 s = V3(c, a.u(2));
    Vec3& d = V3(c, a.u(0));
    d.x = k * s.x;
    d.y = k * s.y;
    d.z = k * s.z;
    ret_void(c);
}

// void stuff_cross_product(Vector3f* out, Vector3f* v1, Vector3f* v2)
VOY_DECOMP(trek, stuff_cross_product) {
    Args a{c};
    const Vec3 p = V3(c, a.u(1)), q = V3(c, a.u(2));
    Vec3& d = V3(c, a.u(0));
    d.x = float(double(p.y) * q.z - double(p.z) * q.y);
    d.y = float(double(p.z) * q.x - double(p.x) * q.z);
    d.z = float(double(p.x) * q.y - double(p.y) * q.x);
    ret_void(c);
}

// float d_squared(Vector3f* V1, Vector3f* V2)
VOY_DECOMP(trek, d_squared) {
    Args a{c};
    const Vec3 &p = V3(c, a.u(0)), &q = V3(c, a.u(1));
    const double dx = double(p.x) - q.x, dy = double(p.y) - q.y, dz = double(p.z) - q.z;
    ret_fp(c, dz * dz + dy * dy + dx * dx);
}

// float dist_a_to_b(Vector3f* p1, Vector3f* p2)
VOY_DECOMP(trek, dist_a_to_b) {
    Args a{c};
    const Vec3 &p = V3(c, a.u(0)), &q = V3(c, a.u(1));
    const double dx = double(p.x) - q.x, dy = double(p.y) - q.y, dz = double(p.z) - q.z;
    ret_fp(c, std::sqrt(dz * dz + dy * dy + dx * dx));
}

// void stuff_xyz(Vector3f* XYZ, float x, float y, float z): copied as bits
VOY_DECOMP(trek, stuff_xyz) {
    Args a{c};
    const std::uint32_t d = a.u(0);
    st32(c.mem, d, a.u(1));
    st32(c.mem, d + 4, a.u(2));
    st32(c.mem, d + 8, a.u(3));
    ret_void(c);
}

// void zero_xyz(Vector3f* a)
VOY_DECOMP(trek, zero_xyz) {
    const std::uint32_t d = Args{c}.u(0);
    st32(c.mem, d + 8, 0);
    st32(c.mem, d + 4, 0);
    st32(c.mem, d, 0);
    ret_void(c);
}

// void copyxyz(Vector3f* dst, Vector3f* src): copied as bits
VOY_DECOMP(trek, copyxyz) {
    Args a{c};
    const std::uint32_t d = a.u(0), s = a.u(1);
    const std::uint32_t x = ld32(c.mem, s), y = ld32(c.mem, s + 4), z = ld32(c.mem, s + 8);
    st32(c.mem, d, x);
    st32(c.mem, d + 4, y);
    st32(c.mem, d + 8, z);
    ret_void(c);
}

// void stuff_sum_xyz3(Vector3f* sum, Vector3f* a, Vector3f* b, Vector3f* c)
VOY_DECOMP(trek, stuff_sum_xyz3) {
    Args a{c};
    const Vec3 p = V3(c, a.u(1)), q = V3(c, a.u(2)), r = V3(c, a.u(3));
    Vec3& d = V3(c, a.u(0));
    d.x = float(double(p.x) + q.x + r.x);
    d.y = float(double(p.y) + q.y + r.y);
    d.z = float(double(p.z) + q.z + r.z);
    ret_void(c);
}

// void stuff_sum_xyz4(Vector3f* sum, Vector3f* a, Vector3f* b, Vector3f* c, Vector3f* d)
VOY_DECOMP(trek, stuff_sum_xyz4) {
    Args a{c};
    const Vec3 p = V3(c, a.u(1)), q = V3(c, a.u(2)), r = V3(c, a.u(3)), s = V3(c, a.u(4));
    Vec3& d = V3(c, a.u(0));
    d.x = float(double(p.x) + q.x + r.x + s.x);
    d.y = float(double(p.y) + q.y + r.y + s.y);
    d.z = float(double(p.z) + q.z + r.z + s.z);
    ret_void(c);
}

// float absval(float x) and float abs_float(float x): -x below zero
VOY_DECOMP(trek, absval) {
    const double x = Args{c}.f(0);
    ret_fp(c, x < 0 ? -x : x);
}
VOY_DECOMP(trek, abs_float) {
    const double x = Args{c}.f(0);
    ret_fp(c, x < 0 ? -x : x);
}

// void stuff_mtrx_identity(float m[16])
VOY_DECOMP(trek, stuff_mtrx_identity) {
    float* m = F(c, Args{c}.u(0));
    for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    ret_void(c);
}

// void stuff_normalized_vector(Vector3f* dest, Vector3f* v1)
VOY_DECOMP(trek, stuff_normalized_vector) {
    Args a{c};
    const std::uint32_t dest = a.u(0);
    const Vec3 v = V3(c, a.u(1));
    const double len = std::sqrt(double(v.z) * v.z + double(v.y) * v.y + double(v.x) * v.x);
    if (len == 0) {
        host::Process& p = host::proc(c);
        recomp::call_guest(c, p.sym("warning"), {p.host_string("CANT NORMALIZE NULL VECTOR")});
        Vec3& d = V3(c, dest);
        d.y = 0;
        d.x = 0;
        d.z = 1;
    } else {
        const double inv = 1.0 / len;
        Vec3& d = V3(c, dest);
        d.x = float(v.x * inv);
        d.y = float(v.y * inv);
        d.z = float(inv * v.z);
    }
    ret_void(c);
}

}
