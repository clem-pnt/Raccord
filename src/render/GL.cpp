#include "GL.h"
#include <QOpenGLContext>
#include <QOpenGLVersionFunctionsFactory>
#include <QDebug>
#include <cstring>

namespace rc {

const char *kFullscreenVertex = R"(#version 330 core
uniform mat3 uTransform;
uniform int uUseTransform;
out vec2 vUv;
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vUv = p;
    vec2 ndc = p * 2.0 - 1.0;
    if (uUseTransform == 1) {
        vec3 q = uTransform * vec3(p, 1.0);
        ndc = q.xy;
    }
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)";

GLint Program::loc(GL *gl, const char *name)
{
    auto it = locations.find(name);
    if (it != locations.end())
        return it.value();
    GLint l = gl->f->glGetUniformLocation(id, name);
    locations.insert(name, l);
    return l;
}

GL::GL()
{
    f = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(QOpenGLContext::currentContext());
    if (!f) {
        m_error = "OpenGL 3.3 core profile is not available";
        return;
    }
    f->initializeOpenGLFunctions();
    f->glGenVertexArrays(1, &m_vao);
    bindState();
}

void GL::bindState()
{
    if (!f) return;
    f->glBindVertexArray(m_vao);
    f->glDisable(GL_DEPTH_TEST);
    f->glDisable(GL_BLEND);
    f->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    f->glPixelStorei(GL_PACK_ALIGNMENT, 1);
}

GL::~GL()
{
    if (!f)
        return;
    for (auto *p : m_programs) {
        f->glDeleteProgram(p->id);
        delete p;
    }
    for (auto &p : m_pool) {
        f->glDeleteFramebuffers(1, &p->s->fbo);
        f->glDeleteTextures(1, &p->s->tex);
    }
    if (m_vao)
        f->glDeleteVertexArrays(1, &m_vao);
}

GLuint GL::compile(GLenum type, const char *src, const QByteArray &name)
{
    GLuint sh = f->glCreateShader(type);
    f->glShaderSource(sh, 1, &src, nullptr);
    f->glCompileShader(sh);
    GLint ok = 0;
    f->glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        f->glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        m_error = QString("Shader %1: %2").arg(QString::fromUtf8(name), QString::fromUtf8(log));
        qWarning().noquote() << m_error;
        f->glDeleteShader(sh);
        return 0;
    }
    return sh;
}

Program *GL::program(const QByteArray &name, const char *fragmentSource)
{
    return program(name, kFullscreenVertex, fragmentSource);
}

Program *GL::program(const QByteArray &name, const char *vertexSource, const char *fragmentSource)
{
    auto it = m_programs.find(name);
    if (it != m_programs.end())
        return it.value();
    GLuint vs = compile(GL_VERTEX_SHADER, vertexSource, name + ".vert");
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragmentSource, name + ".frag");
    Program *p = new Program;
    if (vs && fs) {
        p->id = f->glCreateProgram();
        f->glAttachShader(p->id, vs);
        f->glAttachShader(p->id, fs);
        f->glLinkProgram(p->id);
        GLint ok = 0;
        f->glGetProgramiv(p->id, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[4096];
            f->glGetProgramInfoLog(p->id, sizeof log, nullptr, log);
            m_error = QString("Link %1: %2").arg(QString::fromUtf8(name), QString::fromUtf8(log));
            qWarning().noquote() << m_error;
            f->glDeleteProgram(p->id);
            p->id = 0;
        }
    }
    if (vs) f->glDeleteShader(vs);
    if (fs) f->glDeleteShader(fs);
    m_programs.insert(name, p);
    return p;
}

void GL::use(Program *p)
{
    m_current = p;
    f->glUseProgram(p ? p->id : 0);
    if (p && p->id)
        f->glUniform1i(p->loc(this, "uUseTransform"), 0);
}

void GL::set(const char *n, float v) { if (m_current) f->glUniform1f(m_current->loc(this, n), v); }
void GL::set(const char *n, int v) { if (m_current) f->glUniform1i(m_current->loc(this, n), v); }
void GL::set(const char *n, float x, float y) { if (m_current) f->glUniform2f(m_current->loc(this, n), x, y); }
void GL::set(const char *n, float x, float y, float z) { if (m_current) f->glUniform3f(m_current->loc(this, n), x, y, z); }
void GL::set(const char *n, float x, float y, float z, float w) { if (m_current) f->glUniform4f(m_current->loc(this, n), x, y, z, w); }
void GL::setMat3(const char *n, const float *m) { if (m_current) f->glUniformMatrix3fv(m_current->loc(this, n), 1, GL_FALSE, m); }

void GL::bindTexture(const char *name, int unit, GLuint tex, GLenum targetType)
{
    f->glActiveTexture(GL_TEXTURE0 + unit);
    f->glBindTexture(targetType, tex);
    if (m_current)
        f->glUniform1i(m_current->loc(this, name), unit);
}

SurfaceRef GL::surface(int w, int h, GLenum internalFormat)
{
    w = std::max(1, w);
    h = std::max(1, h);
    for (auto &p : m_pool) {
        if (!p->inUse && p->s->w == w && p->s->h == h && p->s->format == internalFormat) {
            p->inUse = true;
            p->idle = 0;
            std::weak_ptr<Pooled> wp = p;
            return SurfaceRef(p->s.get(), [wp](Surface *) {
                if (auto sp = wp.lock())
                    sp->inUse = false;
            });
        }
    }
    auto p = std::make_shared<Pooled>();
    p->s = std::make_unique<Surface>();
    Surface *s = p->s.get();
    s->w = w;
    s->h = h;
    s->format = internalFormat;
    f->glGenTextures(1, &s->tex);
    f->glBindTexture(GL_TEXTURE_2D, s->tex);
    GLenum type = internalFormat == GL_RGBA8 ? GL_UNSIGNED_BYTE : GL_FLOAT;
    f->glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, w, h, 0, GL_RGBA, type, nullptr);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    f->glGenFramebuffers(1, &s->fbo);
    f->glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    p->inUse = true;
    m_pool.push_back(p);
    std::weak_ptr<Pooled> wp = p;
    return SurfaceRef(s, [wp](Surface *) {
        if (auto sp = wp.lock())
            sp->inUse = false;
    });
}

void GL::trimPool()
{
    // Frees surfaces unused for many frames (e.g. after a resolution change).
    for (auto it = m_pool.begin(); it != m_pool.end();) {
        auto &p = *it;
        if (!p->inUse && ++p->idle > 120) {
            f->glDeleteFramebuffers(1, &p->s->fbo);
            f->glDeleteTextures(1, &p->s->tex);
            it = m_pool.erase(it);
        } else {
            ++it;
        }
    }
}

void GL::target(const SurfaceRef &s)
{
    f->glBindFramebuffer(GL_FRAMEBUFFER, s ? s->fbo : 0);
    if (s)
        f->glViewport(0, 0, s->w, s->h);
}

void GL::clear(float r, float g, float b, float a)
{
    f->glClearColor(r, g, b, a);
    f->glClear(GL_COLOR_BUFFER_BIT);
}

void GL::drawQuad(const float *mat3)
{
    if (mat3 && m_current) {
        f->glUniform1i(m_current->loc(this, "uUseTransform"), 1);
        f->glUniformMatrix3fv(m_current->loc(this, "uTransform"), 1, GL_FALSE, mat3);
    } else if (m_current) {
        f->glUniform1i(m_current->loc(this, "uUseTransform"), 0);
    }
    f->glBindVertexArray(m_vao);
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void GL::blit(const SurfaceRef &src, const SurfaceRef &dst)
{
    static const char *fs = R"(#version 330 core
in vec2 vUv; out vec4 o; uniform sampler2D uTex;
void main(){ o = texture(uTex, vUv); })";
    use(program("blit", fs));
    target(dst);
    bindTexture("uTex", 0, src->tex);
    drawQuad();
}

GLuint GL::uploadImage(const QImage &src, GLuint reuse)
{
    QImage img = src.format() == QImage::Format_RGBA8888_Premultiplied ? src : src.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    GLuint t = reuse;
    if (!t)
        f->glGenTextures(1, &t);
    f->glBindTexture(GL_TEXTURE_2D, t);
    f->glPixelStorei(GL_UNPACK_ROW_LENGTH, img.bytesPerLine() / 4);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width(), img.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
    f->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

void GL::deleteTexture(GLuint t)
{
    if (t)
        f->glDeleteTextures(1, &t);
}

QImage GL::readback(const SurfaceRef &s, bool flip)
{
    QImage img(s->w, s->h, QImage::Format_RGBA8888_Premultiplied);
    f->glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
    f->glReadPixels(0, 0, s->w, s->h, GL_RGBA, GL_UNSIGNED_BYTE, img.bits());
    return flip ? img.mirrored(false, true) : img;
}

void GL::readback16(const SurfaceRef &s, std::vector<uint16_t> &out)
{
    out.resize(size_t(s->w) * s->h * 4);
    f->glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
    f->glReadPixels(0, 0, s->w, s->h, GL_RGBA, GL_UNSIGNED_SHORT, out.data());
}

void GL::readbackFloat(const SurfaceRef &s, std::vector<float> &out)
{
    out.resize(size_t(s->w) * s->h * 4);
    f->glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
    f->glReadPixels(0, 0, s->w, s->h, GL_RGBA, GL_FLOAT, out.data());
}

} // namespace rc
