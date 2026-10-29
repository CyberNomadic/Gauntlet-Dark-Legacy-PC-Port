#include "engine/render/opengl/OpenGlFunctions.h"

#include <bit>

namespace gdl::gl {

namespace {

/** Looks `name` up and keeps it in `into`; false when the context lacks it. */
template <class Function>
bool lookUp(const OpenGlFunctions::Lookup& lookup, const char* name, Function*& into) {
    const OpenGlProcedure found = lookup(name);
    if (found == nullptr) {
        return false;
    }
    into = std::bit_cast<Function*>(found);
    return true;
}

} // namespace

std::string OpenGlFunctions::load(const Lookup& lookup) {
    const auto take = [&](const char* name, auto& into) -> const char* {
        return lookUp(lookup, name, into) ? nullptr : name;
    };
    for (const char* missing :
         {take("glGetError", getError),
          take("glGetString", getString),
          take("glGetStringi", getStringi),
          take("glGetIntegerv", getIntegerv),
          take("glGetFloatv", getFloatv),
          take("glEnable", enable),
          take("glDisable", disable),
          take("glViewport", viewport),
          take("glClearColor", clearColor),
          take("glClearDepth", clearDepth),
          take("glClear", clear),
          take("glDepthFunc", depthFunc),
          take("glDepthMask", depthMask),
          take("glBlendFuncSeparate", blendFuncSeparate),
          take("glCullFace", cullFace),
          take("glFrontFace", frontFace),
          take("glFinish", finish),
          take("glPixelStorei", pixelStorei),
          take("glGenBuffers", genBuffers),
          take("glDeleteBuffers", deleteBuffers),
          take("glBindBuffer", bindBuffer),
          take("glBufferData", bufferData),
          take("glBufferSubData", bufferSubData),
          take("glGenVertexArrays", genVertexArrays),
          take("glDeleteVertexArrays", deleteVertexArrays),
          take("glBindVertexArray", bindVertexArray),
          take("glEnableVertexAttribArray", enableVertexAttribArray),
          take("glVertexAttribPointer", vertexAttribPointer),
          take("glDrawArrays", drawArrays),
          take("glGenTextures", genTextures),
          take("glDeleteTextures", deleteTextures),
          take("glBindTexture", bindTexture),
          take("glActiveTexture", activeTexture),
          take("glTexImage2D", texImage2D),
          take("glTexSubImage2D", texSubImage2D),
          take("glTexParameteri", texParameteri),
          take("glGenerateMipmap", generateMipmap),
          take("glGenSamplers", genSamplers),
          take("glDeleteSamplers", deleteSamplers),
          take("glBindSampler", bindSampler),
          take("glSamplerParameteri", samplerParameteri),
          take("glSamplerParameterf", samplerParameterf),
          take("glCreateShader", createShader),
          take("glShaderSource", shaderSource),
          take("glCompileShader", compileShader),
          take("glGetShaderiv", getShaderiv),
          take("glGetShaderInfoLog", getShaderInfoLog),
          take("glDeleteShader", deleteShader),
          take("glCreateProgram", createProgram),
          take("glAttachShader", attachShader),
          take("glLinkProgram", linkProgram),
          take("glGetProgramiv", getProgramiv),
          take("glGetProgramInfoLog", getProgramInfoLog),
          take("glDeleteProgram", deleteProgram),
          take("glUseProgram", useProgram),
          take("glGetUniformLocation", getUniformLocation),
          take("glUniform1i", uniform1i),
          take("glUniform4fv", uniform4fv),
          take("glUniformMatrix4fv", uniformMatrix4fv),
          take("glGenFramebuffers", genFramebuffers),
          take("glDeleteFramebuffers", deleteFramebuffers),
          take("glBindFramebuffer", bindFramebuffer),
          take("glFramebufferRenderbuffer", framebufferRenderbuffer),
          take("glCheckFramebufferStatus", checkFramebufferStatus),
          take("glBlitFramebuffer", blitFramebuffer),
          take("glGenRenderbuffers", genRenderbuffers),
          take("glDeleteRenderbuffers", deleteRenderbuffers),
          take("glBindRenderbuffer", bindRenderbuffer),
          take("glRenderbufferStorageMultisample", renderbufferStorageMultisample)}) {
        if (missing != nullptr) {
            return missing;
        }
    }
    return {};
}

} // namespace gdl::gl
