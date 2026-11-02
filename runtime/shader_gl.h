/* Shared by the shader modules (shader.c, shader_glsl.c): the OpenGL functions they call, loaded from the
 * renderer's context, and the RetroArch GLSL preset runner. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define GL_GLEXT_PROTOTYPES /* prototypes only, for the pointer types below: nothing is linked */
#include <SDL3/SDL_opengl.h>

#define SHADER_GL_FUNCS(X)                                                                                         \
    X(GetString) X(GetIntegerv) X(GetError) X(Viewport) X(Clear) X(ClearColor) X(Enable) X(Disable) X(PixelStorei)   \
    X(GenTextures) X(DeleteTextures) X(BindTexture) X(TexParameteri) X(TexImage2D) X(DrawArrays) X(ActiveTexture)  \
    X(GenerateMipmap) X(GenFramebuffers) X(DeleteFramebuffers) X(BindFramebuffer) X(FramebufferTexture2D)          \
    X(CheckFramebufferStatus) X(BlitFramebuffer) X(CreateShader) X(ShaderSource) X(CompileShader) X(GetShaderiv)   \
    X(GetShaderInfoLog) X(DeleteShader) X(CreateProgram) X(AttachShader) X(LinkProgram) X(GetProgramiv)            \
    X(GetProgramInfoLog) X(DeleteProgram) X(UseProgram) X(GetActiveAttrib) X(GetAttribLocation)                    \
    X(GetUniformLocation) X(Uniform1i) X(Uniform1f) X(Uniform2f) X(Uniform4f) X(UniformMatrix4fv)                  \
    X(VertexAttribPointer) X(VertexAttrib4f) X(EnableVertexAttribArray) X(DisableVertexAttribArray) X(GenBuffers)  \
    X(DeleteBuffers) X(BindBuffer) X(BufferData) X(GenVertexArrays) X(DeleteVertexArrays) X(BindVertexArray)       \
    X(GenSamplers) X(DeleteSamplers) X(BindSampler) X(SamplerParameteri)

typedef struct ShaderGl {
#define X(f) __typeof__(&gl##f) f;
    SHADER_GL_FUNCS(X)
#undef X
} ShaderGl;
extern ShaderGl sgl; /* loaded by shader_init */

/* A RetroArch GLSL shader preset (.glslp), or a single .glsl shader, compiled for the current context. */
typedef struct GlslChain GlslChain;
/* NULL on failure, with the reason in err. */
GlslChain *glsl_chain_load(const char *path, char *err, size_t n);
void glsl_chain_free(GlslChain *c);
/* Runs the chain on texture `in` (w x h, image top at t = 0) into texture `out` (out_w x out_h, the same way up). */
void glsl_chain_run(GlslChain *c, GLuint in, int w, int h, GLuint out, int out_w, int out_h, uint64_t frame);
