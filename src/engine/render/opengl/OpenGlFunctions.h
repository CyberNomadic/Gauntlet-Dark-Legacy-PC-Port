#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include "engine/core/Types.h"
#include "engine/platform/Window.h"

// OpenGL's own spellings for its types and constants, as its reference names them, so the
// device reads against the specification; the entry points are members in house style.
// NOLINTBEGIN(readability-identifier-naming)
namespace gdl::gl {

using GLenum = u32;
using GLboolean = u8;
using GLbitfield = u32;
using GLint = s32;
using GLsizei = s32;
using GLuint = u32;
using GLfloat = f32;
using GLdouble = f64;
using GLchar = char;
using GLubyte = u8;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;

inline constexpr GLenum GL_NO_ERROR = 0;
inline constexpr GLboolean GL_FALSE = 0;
inline constexpr GLboolean GL_TRUE = 1;
inline constexpr GLenum GL_VENDOR = 0x1F00;
inline constexpr GLenum GL_RENDERER = 0x1F01;
inline constexpr GLenum GL_VERSION = 0x1F02;
inline constexpr GLenum GL_EXTENSIONS = 0x1F03;
inline constexpr GLenum GL_NUM_EXTENSIONS = 0x821D;
inline constexpr GLenum GL_MAX_TEXTURE_SIZE = 0x0D33;
inline constexpr GLenum GL_MAX_SAMPLES = 0x8D57;
inline constexpr GLenum GL_DEPTH_TEST = 0x0B71;
inline constexpr GLenum GL_BLEND = 0x0BE2;
inline constexpr GLenum GL_CULL_FACE = 0x0B44;
inline constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
inline constexpr GLenum GL_STENCIL_TEST = 0x0B90;
inline constexpr GLenum GL_DITHER = 0x0BD0;
inline constexpr GLenum GL_MULTISAMPLE = 0x809D;
inline constexpr GLenum GL_SAMPLE_ALPHA_TO_COVERAGE = 0x809E;
inline constexpr GLenum GL_FRAMEBUFFER_SRGB = 0x8DB9;
inline constexpr GLbitfield GL_DEPTH_BUFFER_BIT = 0x0100;
inline constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x4000;
inline constexpr GLenum GL_GEQUAL = 0x0206;
inline constexpr GLenum GL_ALWAYS = 0x0207;
inline constexpr GLenum GL_ZERO = 0;
inline constexpr GLenum GL_ONE = 1;
inline constexpr GLenum GL_SRC_ALPHA = 0x0302;
inline constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
inline constexpr GLenum GL_BACK = 0x0405;
inline constexpr GLenum GL_CW = 0x0900;
inline constexpr GLenum GL_CCW = 0x0901;
inline constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
inline constexpr GLenum GL_STREAM_DRAW = 0x88E0;
inline constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
inline constexpr GLenum GL_FLOAT = 0x1406;
inline constexpr GLenum GL_TRIANGLES = 0x0004;
inline constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
inline constexpr GLenum GL_TEXTURE0 = 0x84C0;
inline constexpr GLenum GL_RGBA = 0x1908;
inline constexpr GLenum GL_RGBA8 = 0x8058;
inline constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
inline constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
inline constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
inline constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
inline constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
inline constexpr GLenum GL_TEXTURE_BASE_LEVEL = 0x813C;
inline constexpr GLenum GL_TEXTURE_MAX_LEVEL = 0x813D;
inline constexpr GLenum GL_NEAREST = 0x2600;
inline constexpr GLenum GL_LINEAR = 0x2601;
inline constexpr GLenum GL_NEAREST_MIPMAP_NEAREST = 0x2700;
inline constexpr GLenum GL_LINEAR_MIPMAP_LINEAR = 0x2703;
inline constexpr GLenum GL_REPEAT = 0x2901;
inline constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
inline constexpr GLenum GL_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FE;
inline constexpr GLenum GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FF;
inline constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
inline constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
inline constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
inline constexpr GLenum GL_LINK_STATUS = 0x8B82;
inline constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
inline constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
inline constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
inline constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
inline constexpr GLenum GL_RENDERBUFFER = 0x8D41;
inline constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
inline constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
inline constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
inline constexpr GLenum GL_DEPTH_COMPONENT24 = 0x81A6;

/** The OpenGL 3.3 core entry points the device calls, looked up through the window's
 * context: the game links no OpenGL library, so a machine without one still starts. */
struct OpenGlFunctions {
    using Lookup = std::function<OpenGlProcedure(const char*)>;

    /** Looks every entry point up; the name of the first one missing, or empty. */
    std::string load(const Lookup& lookup);

    GLenum (*getError)() = nullptr;
    const GLubyte* (*getString)(GLenum name) = nullptr;
    const GLubyte* (*getStringi)(GLenum name, GLuint index) = nullptr;
    void (*getIntegerv)(GLenum name, GLint* data) = nullptr;
    void (*getFloatv)(GLenum name, GLfloat* data) = nullptr;
    void (*enable)(GLenum capability) = nullptr;
    void (*disable)(GLenum capability) = nullptr;
    void (*viewport)(GLint x, GLint y, GLsizei width, GLsizei height) = nullptr;
    void (*clearColor)(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) = nullptr;
    void (*clearDepth)(GLdouble depth) = nullptr;
    void (*clear)(GLbitfield mask) = nullptr;
    void (*depthFunc)(GLenum function) = nullptr;
    void (*depthMask)(GLboolean flag) = nullptr;
    void (*blendFuncSeparate)(GLenum sourceRgb, GLenum destinationRgb, GLenum sourceAlpha,
                              GLenum destinationAlpha) = nullptr;
    void (*cullFace)(GLenum mode) = nullptr;
    void (*frontFace)(GLenum mode) = nullptr;
    void (*finish)() = nullptr;
    void (*pixelStorei)(GLenum name, GLint value) = nullptr;
    void (*genBuffers)(GLsizei count, GLuint* buffers) = nullptr;
    void (*deleteBuffers)(GLsizei count, const GLuint* buffers) = nullptr;
    void (*bindBuffer)(GLenum target, GLuint buffer) = nullptr;
    void (*bufferData)(GLenum target, GLsizeiptr size, const void* data, GLenum usage) = nullptr;
    void (*bufferSubData)(GLenum target, GLintptr offset, GLsizeiptr size,
                          const void* data) = nullptr;
    void (*genVertexArrays)(GLsizei count, GLuint* arrays) = nullptr;
    void (*deleteVertexArrays)(GLsizei count, const GLuint* arrays) = nullptr;
    void (*bindVertexArray)(GLuint array) = nullptr;
    void (*enableVertexAttribArray)(GLuint index) = nullptr;
    void (*vertexAttribPointer)(GLuint index, GLint size, GLenum type, GLboolean normalized,
                                GLsizei stride, const void* offset) = nullptr;
    void (*drawArrays)(GLenum mode, GLint first, GLsizei count) = nullptr;
    void (*genTextures)(GLsizei count, GLuint* textures) = nullptr;
    void (*deleteTextures)(GLsizei count, const GLuint* textures) = nullptr;
    void (*bindTexture)(GLenum target, GLuint texture) = nullptr;
    void (*activeTexture)(GLenum unit) = nullptr;
    void (*texImage2D)(GLenum target, GLint level, GLint internalFormat, GLsizei width,
                       GLsizei height, GLint border, GLenum format, GLenum type,
                       const void* pixels) = nullptr;
    void (*texSubImage2D)(GLenum target, GLint level, GLint x, GLint y, GLsizei width,
                          GLsizei height, GLenum format, GLenum type, const void* pixels) = nullptr;
    void (*texParameteri)(GLenum target, GLenum name, GLint value) = nullptr;
    void (*generateMipmap)(GLenum target) = nullptr;
    void (*genSamplers)(GLsizei count, GLuint* samplers) = nullptr;
    void (*deleteSamplers)(GLsizei count, const GLuint* samplers) = nullptr;
    void (*bindSampler)(GLuint unit, GLuint sampler) = nullptr;
    void (*samplerParameteri)(GLuint sampler, GLenum name, GLint value) = nullptr;
    void (*samplerParameterf)(GLuint sampler, GLenum name, GLfloat value) = nullptr;
    GLuint (*createShader)(GLenum type) = nullptr;
    void (*shaderSource)(GLuint shader, GLsizei count, const GLchar* const* strings,
                         const GLint* lengths) = nullptr;
    void (*compileShader)(GLuint shader) = nullptr;
    void (*getShaderiv)(GLuint shader, GLenum name, GLint* value) = nullptr;
    void (*getShaderInfoLog)(GLuint shader, GLsizei capacity, GLsizei* length,
                             GLchar* text) = nullptr;
    void (*deleteShader)(GLuint shader) = nullptr;
    GLuint (*createProgram)() = nullptr;
    void (*attachShader)(GLuint program, GLuint shader) = nullptr;
    void (*linkProgram)(GLuint program) = nullptr;
    void (*getProgramiv)(GLuint program, GLenum name, GLint* value) = nullptr;
    void (*getProgramInfoLog)(GLuint program, GLsizei capacity, GLsizei* length,
                              GLchar* text) = nullptr;
    void (*deleteProgram)(GLuint program) = nullptr;
    void (*useProgram)(GLuint program) = nullptr;
    GLint (*getUniformLocation)(GLuint program, const GLchar* name) = nullptr;
    void (*uniform1i)(GLint location, GLint value) = nullptr;
    void (*uniform4fv)(GLint location, GLsizei count, const GLfloat* value) = nullptr;
    void (*uniformMatrix4fv)(GLint location, GLsizei count, GLboolean transpose,
                             const GLfloat* value) = nullptr;
    void (*genFramebuffers)(GLsizei count, GLuint* framebuffers) = nullptr;
    void (*deleteFramebuffers)(GLsizei count, const GLuint* framebuffers) = nullptr;
    void (*bindFramebuffer)(GLenum target, GLuint framebuffer) = nullptr;
    void (*framebufferRenderbuffer)(GLenum target, GLenum attachment, GLenum renderbufferTarget,
                                    GLuint renderbuffer) = nullptr;
    GLenum (*checkFramebufferStatus)(GLenum target) = nullptr;
    void (*blitFramebuffer)(GLint sourceX0, GLint sourceY0, GLint sourceX1, GLint sourceY1,
                            GLint destinationX0, GLint destinationY0, GLint destinationX1,
                            GLint destinationY1, GLbitfield mask, GLenum filter) = nullptr;
    void (*genRenderbuffers)(GLsizei count, GLuint* renderbuffers) = nullptr;
    void (*deleteRenderbuffers)(GLsizei count, const GLuint* renderbuffers) = nullptr;
    void (*bindRenderbuffer)(GLenum target, GLuint renderbuffer) = nullptr;
    void (*renderbufferStorageMultisample)(GLenum target, GLsizei samples, GLenum internalFormat,
                                           GLsizei width, GLsizei height) = nullptr;
};

} // namespace gdl::gl
// NOLINTEND(readability-identifier-naming)
