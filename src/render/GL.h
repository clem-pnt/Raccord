#pragma once
// OpenGL 3.3 core helpers: shader cache, pooled render surfaces, textures.
#include <QOpenGLFunctions_3_3_Core>
#include <QString>
#include <QHash>
#include <QSize>
#include <QImage>
#include <functional>
#include <memory>
#include <vector>

namespace rc {

class GL;

// A colour texture with its framebuffer, returned to the pool when released.
struct Surface {
    GLuint fbo = 0;
    GLuint tex = 0;
    int w = 0, h = 0;
    GLenum format = 0;
};
using SurfaceRef = std::shared_ptr<Surface>;

struct Program {
    GLuint id = 0;
    QHash<QByteArray, GLint> locations;
    GLint loc(GL *gl, const char *name);
};

class GL {
public:
    // Must be called with a current OpenGL 3.3+ context.
    GL();
    ~GL();
    QOpenGLFunctions_3_3_Core *f = nullptr;
    // Re-applies the state the renderer relies on (after QPainter or other GL users).
    void bindState();

    // Program from a fragment shader source (shared full screen vertex shader).
    Program *program(const QByteArray &name, const char *fragmentSource);
    Program *program(const QByteArray &name, const char *vertexSource, const char *fragmentSource);
    void use(Program *p);

    // Uniform setters on the current program.
    void set(const char *name, float v);
    void set(const char *name, int v);
    void set(const char *name, float x, float y);
    void set(const char *name, float x, float y, float z);
    void set(const char *name, float x, float y, float z, float w);
    void setMat3(const char *name, const float *m);
    void bindTexture(const char *name, int unit, GLuint tex, GLenum target = GL_TEXTURE_2D);

    // Render surfaces (RGBA16F by default).
    SurfaceRef surface(int w, int h, GLenum internalFormat = GL_RGBA16F);
    void target(const SurfaceRef &s);       // bind as draw target + viewport
    void clear(float r = 0, float g = 0, float b = 0, float a = 0);
    // Draws the unit quad with an identity full screen transform (or custom 3x3 matrix).
    void drawQuad(const float *mat3 = nullptr);
    // Copy src into dst (with bilinear scaling).
    void blit(const SurfaceRef &src, const SurfaceRef &dst);

    // Uploads a QImage as an RGBA8 texture (owned by caller).
    GLuint uploadImage(const QImage &img, GLuint reuse = 0);
    void deleteTexture(GLuint t);
    QImage readback(const SurfaceRef &s, bool flip = false);
    // Reads RGBA16 (16 bit unsigned) pixels into `out` (w*h*4 uint16), rows top-first.
    void readback16(const SurfaceRef &s, std::vector<uint16_t> &out);
    void readbackFloat(const SurfaceRef &s, std::vector<float> &out);
    // Replaces the content of a surface with w*h RGBA floats (same row order as readbackFloat).
    void uploadFloat(const SurfaceRef &s, const float *data);

    QString lastError() const { return m_error; }
    GLuint vao() const { return m_vao; }
    void trimPool();

private:
    GLuint compile(GLenum type, const char *src, const QByteArray &name);
    QHash<QByteArray, Program *> m_programs;
    Program *m_current = nullptr;
    GLuint m_vao = 0;
    struct Pooled { std::unique_ptr<Surface> s; bool inUse = false; int idle = 0; };
    std::vector<std::shared_ptr<Pooled>> m_pool;
    QString m_error;
};

extern const char *kFullscreenVertex;

} // namespace rc
