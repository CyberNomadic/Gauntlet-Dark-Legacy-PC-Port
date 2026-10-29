// What the two compilers spell differently, so the shader bodies are shared: the Vulkan
// build compiles them to SPIR-V with push constants and descriptor sets; the OpenGL
// device has the driver compile them as GLSL 3.30 with GDL_OPENGL defined, where the
// same constants are a uniform struct and the textures plain samplers.
#ifdef GDL_OPENGL
#define VARYING(slot)
#define PUSH_CONSTANTS_BEGIN(block) struct block {
#define PUSH_CONSTANTS_END(block, name) }; uniform block name;
#define SAMPLER_2D(slot, name) uniform sampler2D name;
#define SAMPLER_2DMS(slot, name) uniform sampler2DMS name;
#define VERTEX_INDEX gl_VertexID
#else
#define VARYING(slot) layout(location = slot)
#define PUSH_CONSTANTS_BEGIN(block) layout(push_constant) uniform block {
#define PUSH_CONSTANTS_END(block, name) } name;
#define SAMPLER_2D(slot, name) layout(set = slot, binding = 0) uniform sampler2D name;
#define SAMPLER_2DMS(slot, name) layout(set = slot, binding = 0) uniform sampler2DMS name;
#define VERTEX_INDEX gl_VertexIndex
#endif
