// The OpenGL, GLU and GLUT entry points the programs import, bound to the
// emulated gl::Context, and the GLUT main loop the game hands control to.
#include <cstring>
#include <vector>

#include "gl/gl.hpp"
#include "host/fail.hpp"
#include "host/glut.hpp"
#include "host/machine.hpp"
#include "host/process.hpp"

namespace voy::host {

using namespace recomp;

namespace {

gl::Context& G(Cpu& c) { return proc(c).machine().gl(); }
const float* fptr(Cpu& c, std::uint32_t a) { return reinterpret_cast<const float*>(c.mem + a); }

void h_glBegin(Cpu& c) { G(c).Begin(Args{c}.u(0)); ret_void(c); }
void h_glEnd(Cpu& c) { G(c).End(); ret_void(c); }
void h_glVertex2f(Cpu& c) { Args a{c}; G(c).Vertex4(a.f(0), a.f(1), 0, 1); ret_void(c); }
void h_glVertex3f(Cpu& c) { Args a{c}; G(c).Vertex4(a.f(0), a.f(1), a.f(2), 1); ret_void(c); }
void h_glNormal3f(Cpu& c) { Args a{c}; G(c).Normal(a.f(0), a.f(1), a.f(2)); ret_void(c); }
void h_glTexCoord2f(Cpu& c) { Args a{c}; G(c).TexCoord(a.f(0), a.f(1)); ret_void(c); }
void h_glColor3f(Cpu& c) { Args a{c}; G(c).Color(a.f(0), a.f(1), a.f(2), 1); ret_void(c); }
void h_glColor4f(Cpu& c) { Args a{c}; G(c).Color(a.f(0), a.f(1), a.f(2), a.f(3)); ret_void(c); }
void h_glColor4ubv(Cpu& c) {
    const std::uint8_t* p = c.mem + Args{c}.u(0);
    G(c).Color(p[0] / 255.f, p[1] / 255.f, p[2] / 255.f, p[3] / 255.f);
    ret_void(c);
}
void h_glRectf(Cpu& c) { Args a{c}; G(c).Rect(a.f(0), a.f(1), a.f(2), a.f(3)); ret_void(c); }

void h_glMatrixMode(Cpu& c) { G(c).MatrixMode(Args{c}.u(0)); ret_void(c); }
void h_glLoadIdentity(Cpu& c) { G(c).LoadIdentity(); ret_void(c); }
void h_glPushMatrix(Cpu& c) { G(c).PushMatrix(); ret_void(c); }
void h_glPopMatrix(Cpu& c) { G(c).PopMatrix(); ret_void(c); }
void h_glMultMatrixd(Cpu& c) {
    double m[16];
    std::memcpy(m, c.mem + Args{c}.u(0), sizeof m);
    G(c).MultMatrix(m);
    ret_void(c);
}
void h_glMultMatrixf(Cpu& c) {
    const float* f = fptr(c, Args{c}.u(0));
    double m[16];
    for (int i = 0; i < 16; ++i) m[i] = f[i];
    G(c).MultMatrix(m);
    ret_void(c);
}
void h_glTranslatef(Cpu& c) { Args a{c}; G(c).Translate(a.f(0), a.f(1), a.f(2)); ret_void(c); }
void h_glTranslated(Cpu& c) { Args a{c}; G(c).Translate(a.d(0), a.d(2), a.d(4)); ret_void(c); }
void h_glRotatef(Cpu& c) { Args a{c}; G(c).Rotate(a.f(0), a.f(1), a.f(2), a.f(3)); ret_void(c); }
void h_glScalef(Cpu& c) { Args a{c}; G(c).Scale(a.f(0), a.f(1), a.f(2)); ret_void(c); }
void h_glFrustum(Cpu& c) { Args a{c}; G(c).Frustum(a.d(0), a.d(2), a.d(4), a.d(6), a.d(8), a.d(10)); ret_void(c); }
void h_glOrtho(Cpu& c) { Args a{c}; G(c).Ortho(a.d(0), a.d(2), a.d(4), a.d(6), a.d(8), a.d(10)); ret_void(c); }
void h_gluPerspective(Cpu& c) { Args a{c}; G(c).Perspective(a.d(0), a.d(2), a.d(4), a.d(6)); ret_void(c); }
void h_gluLookAt(Cpu& c) {
    Args a{c};
    const double e[3] = {a.d(0), a.d(2), a.d(4)}, ce[3] = {a.d(6), a.d(8), a.d(10)}, u[3] = {a.d(12), a.d(14), a.d(16)};
    G(c).LookAt(e, ce, u);
    ret_void(c);
}
void h_glViewport(Cpu& c) { Args a{c}; G(c).Viewport(a.i(0), a.i(1), a.i(2), a.i(3)); ret_void(c); }
void h_glScissor(Cpu& c) { Args a{c}; G(c).Scissor(a.i(0), a.i(1), a.i(2), a.i(3)); ret_void(c); }

void h_glEnable(Cpu& c) { G(c).Enable(Args{c}.u(0), true); ret_void(c); }
void h_glDisable(Cpu& c) { G(c).Enable(Args{c}.u(0), false); ret_void(c); }
void h_glClear(Cpu& c) { G(c).Clear(Args{c}.u(0)); ret_void(c); }
void h_glClearColor(Cpu& c) { Args a{c}; G(c).ClearColor(a.f(0), a.f(1), a.f(2), a.f(3)); ret_void(c); }
void h_glClearDepth(Cpu& c) { G(c).ClearDepth(Args{c}.d(0)); ret_void(c); }
void h_glColorMask(Cpu& c) {
    Args a{c};
    G(c).ColorMask(a.u(0) & 0xFF, a.u(1) & 0xFF, a.u(2) & 0xFF, a.u(3) & 0xFF);
    ret_void(c);
}
void h_glDepthMask(Cpu& c) { G(c).DepthMask(Args{c}.u(0) & 0xFF); ret_void(c); }
void h_glDepthFunc(Cpu& c) { G(c).DepthFunc(Args{c}.u(0)); ret_void(c); }
void h_glBlendFunc(Cpu& c) { Args a{c}; G(c).BlendFunc(a.u(0), a.u(1)); ret_void(c); }
void h_glCullFace(Cpu& c) { G(c).CullFace(Args{c}.u(0)); ret_void(c); }
void h_glShadeModel(Cpu& c) { G(c).ShadeModel(Args{c}.u(0)); ret_void(c); }
void h_glPolygonOffset(Cpu& c) { Args a{c}; G(c).PolygonOffset(a.f(0), a.f(1)); ret_void(c); }
void h_glLightfv(Cpu& c) { Args a{c}; G(c).Light(a.u(0), a.u(1), fptr(c, a.u(2))); ret_void(c); }
void h_glLightModelfv(Cpu& c) { Args a{c}; G(c).LightModel(a.u(0), fptr(c, a.u(1))); ret_void(c); }
void h_glMaterialfv(Cpu& c) { Args a{c}; G(c).Material(a.u(0), a.u(1), fptr(c, a.u(2))); ret_void(c); }

void h_glBindTexture(Cpu& c) { Args a{c}; G(c).BindTexture(a.u(0), a.u(1)); ret_void(c); }
void h_glDeleteTextures(Cpu& c) {
    Args a{c};
    std::vector<std::uint32_t> ids(std::size_t(std::max(0, a.i(0))));
    if (!ids.empty()) std::memcpy(ids.data(), c.mem + a.u(1), ids.size() * 4);
    G(c).DeleteTextures(int(ids.size()), ids.data());
    ret_void(c);
}
void h_glTexParameteri(Cpu& c) { Args a{c}; G(c).TexParameter(a.u(0), a.u(1), a.u(2)); ret_void(c); }
void h_glTexEnvf(Cpu& c) { Args a{c}; G(c).TexEnv(a.u(1), a.f(2)); ret_void(c); }
void h_glPixelStorei(Cpu& c) { Args a{c}; G(c).PixelStore(a.u(0), a.i(1)); ret_void(c); }
void bad_format(Cpu& c, const char* fn, std::uint32_t format, std::uint32_t type) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s format 0x%04X type 0x%04X", fn, format, type);
    note_once("gl", buf);
    (void)c;
}
void h_glTexImage2D(Cpu& c) {
    Args a{c};
    const std::uint32_t pixels = a.u(8);
    if (!G(c).TexImage(a.u(0), a.i(1), a.i(2), a.i(3), a.i(4), a.u(6), a.u(7), pixels ? c.mem + pixels : nullptr, false))
        bad_format(c, "glTexImage2D", a.u(6), a.u(7));
    ret_void(c);
}
void h_glTexImage1D(Cpu& c) {
    Args a{c};
    const std::uint32_t pixels = a.u(7);
    if (!G(c).TexImage(a.u(0), a.i(1), a.i(2), a.i(3), 1, a.u(5), a.u(6), pixels ? c.mem + pixels : nullptr, false))
        bad_format(c, "glTexImage1D", a.u(5), a.u(6));
    ret_void(c);
}
void h_glTexSubImage2D(Cpu& c) {
    Args a{c};
    if (!G(c).TexSubImage(a.u(0), a.i(1), a.i(2), a.i(3), a.i(4), a.i(5), a.u(6), a.u(7), c.mem + a.u(8)))
        bad_format(c, "glTexSubImage2D", a.u(6), a.u(7));
    ret_void(c);
}
void h_gluBuild2DMipmaps(Cpu& c) {
    Args a{c};
    if (!G(c).TexImage(a.u(0), 0, a.i(1), a.i(2), a.i(3), a.u(4), a.u(5), c.mem + a.u(6), true))
        bad_format(c, "gluBuild2DMipmaps", a.u(4), a.u(5));
    ret(c, 0);
}
void get_values(Cpu& c, bool integers) {
    Args a{c};
    float v[16];
    int n = 0;
    if (!G(c).GetFloat(a.u(0), v, &n)) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "glGet 0x%04X (zero returned)", a.u(0));
        note_once("gl", buf);
        v[0] = 0;
        n = 1;
    }
    for (int i = 0; i < n; ++i) {
        if (integers) st32(c.mem, a.u(1) + 4 * i, std::uint32_t(std::int32_t(v[i])));
        else stf32(c.mem, a.u(1) + 4 * i, v[i]);
    }
    ret_void(c);
}
void h_glGetFloatv(Cpu& c) { get_values(c, false); }
void h_glGetIntegerv(Cpu& c) { get_values(c, true); }
void h_glGetError(Cpu& c) { ret(c, 0); }
void h_void(Cpu& c) { ret_void(c); }
void h_swap(Cpu& c) {
    Process& p = proc(c);
    p.machine().gl().swap();
    p.machine().count_frame();
    ret_void(c);
}

// ---- GLUT ----
GlutState& glut(Cpu& c) { return glut_state(proc(c)); }
void h_glutInit(Cpu& c) { ret_void(c); }
void h_glutCreateWindow(Cpu& c) { ret(c, 1); }
void h_glutEnterGameMode(Cpu& c) { ret(c, 1); }
void h_glutDisplayFunc(Cpu& c) { glut(c).display = Args{c}.u(0); ret_void(c); }
void h_glutIdleFunc(Cpu& c) { glut(c).idle = Args{c}.u(0); ret_void(c); }
void h_glutKeyboardFunc(Cpu& c) { glut(c).keyboard = Args{c}.u(0); ret_void(c); }
void h_glutSpecialFunc(Cpu& c) { glut(c).special = Args{c}.u(0); ret_void(c); }
void h_glutMouseFunc(Cpu& c) { glut(c).mouse = Args{c}.u(0); ret_void(c); }
void h_glutReshapeFunc(Cpu& c) { glut(c).reshape = Args{c}.u(0); ret_void(c); }
void h_glutVisibilityFunc(Cpu& c) { glut(c).visibility = Args{c}.u(0); ret_void(c); }
void h_glutPostRedisplay(Cpu& c) { glut(c).redisplay = true; ret_void(c); }
void h_glutMainLoop(Cpu& c) { glut_main_loop(c); }

}  // namespace

void register_gl_imports() {
    register_imports({
        {"glBegin", h_glBegin}, {"glEnd", h_glEnd}, {"glVertex2f", h_glVertex2f}, {"glVertex3f", h_glVertex3f},
        {"glNormal3f", h_glNormal3f}, {"glTexCoord2f", h_glTexCoord2f}, {"glColor3f", h_glColor3f},
        {"glColor4f", h_glColor4f}, {"glColor4ubv", h_glColor4ubv}, {"glRectf", h_glRectf},
        {"glMatrixMode", h_glMatrixMode}, {"glLoadIdentity", h_glLoadIdentity}, {"glPushMatrix", h_glPushMatrix},
        {"glPopMatrix", h_glPopMatrix}, {"glMultMatrixd", h_glMultMatrixd}, {"glMultMatrixf", h_glMultMatrixf},
        {"glTranslatef", h_glTranslatef}, {"glTranslated", h_glTranslated}, {"glRotatef", h_glRotatef},
        {"glScalef", h_glScalef}, {"glFrustum", h_glFrustum}, {"glOrtho", h_glOrtho},
        {"gluPerspective", h_gluPerspective}, {"gluLookAt", h_gluLookAt}, {"glViewport", h_glViewport},
        {"glScissor", h_glScissor}, {"glEnable", h_glEnable}, {"glDisable", h_glDisable}, {"glClear", h_glClear},
        {"glClearColor", h_glClearColor}, {"glClearDepth", h_glClearDepth}, {"glColorMask", h_glColorMask},
        {"glDepthMask", h_glDepthMask}, {"glDepthFunc", h_glDepthFunc}, {"glBlendFunc", h_glBlendFunc},
        {"glCullFace", h_glCullFace}, {"glShadeModel", h_glShadeModel}, {"glPolygonOffset", h_glPolygonOffset},
        {"glLightfv", h_glLightfv}, {"glLightModelfv", h_glLightModelfv}, {"glMaterialfv", h_glMaterialfv},
        {"glBindTexture", h_glBindTexture}, {"glDeleteTextures", h_glDeleteTextures},
        {"glTexParameteri", h_glTexParameteri}, {"glTexEnvf", h_glTexEnvf}, {"glPixelStorei", h_glPixelStorei},
        {"glTexImage2D", h_glTexImage2D}, {"glTexImage1D", h_glTexImage1D}, {"glTexSubImage2D", h_glTexSubImage2D},
        {"gluBuild2DMipmaps", h_gluBuild2DMipmaps}, {"glGetFloatv", h_glGetFloatv}, {"glGetIntegerv", h_glGetIntegerv},
        {"glGetError", h_glGetError}, {"glFlush", h_void}, {"glXSwapBuffers", h_swap}, {"glutSwapBuffers", h_swap},
        {"glutInit", h_glutInit}, {"glutInitDisplayMode", h_void}, {"glutInitWindowPosition", h_void},
        {"glutInitWindowSize", h_void}, {"glutCreateWindow", h_glutCreateWindow},
        {"glutEnterGameMode", h_glutEnterGameMode}, {"glutLeaveGameMode", h_void}, {"glutSetCursor", h_void},
        {"glutDisplayFunc", h_glutDisplayFunc}, {"glutIdleFunc", h_glutIdleFunc},
        {"glutKeyboardFunc", h_glutKeyboardFunc}, {"glutSpecialFunc", h_glutSpecialFunc},
        {"glutMouseFunc", h_glutMouseFunc}, {"glutReshapeFunc", h_glutReshapeFunc},
        {"glutVisibilityFunc", h_glutVisibilityFunc}, {"glutPostRedisplay", h_glutPostRedisplay},
        {"glutMainLoop", h_glutMainLoop},
    });
}

}
