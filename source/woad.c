#define COAL_SIMPLE_TYPE_NAMES
#define OBDN_SIMPLE_TYPE_NAMES
#include "woad.h"

#include <assert.h>
#include <coal/coal.h>
#include <hell/hell.h>
#include <hell/len.h>
#include <memory.h>
#include <obsidian/attribute.h>
#include <stdio.h>
#include <string.h>

typedef Obdn_Command               Command;
typedef Obdn_Image                 Image;
typedef Obdn_Light                 Light;
typedef Obdn_Material              Material;
typedef Obdn_BufferRegion          BufferRegion;
typedef Obdn_AccelerationStructure AccelerationStructure;
typedef Obdn_Primitive             Prim;

// quick hack
#define OBDN_S_MAX_MATERIALS 10

typedef Obdn_Mask AttrMask;

enum {
    POS_BIT    = 1 << 0,
    NORMAL_BIT = 1 << 1,
    UV_BIT     = 1 << 2,
    TAN_BIT    = 1 << 3,
};

#define POS_NOR_UV_TAN_MASK (POS_BIT | NORMAL_BIT | UV_BIT | TAN_BIT)
#define POS_NOR_UV_MASK (POS_BIT | NORMAL_BIT | UV_BIT)
#define POS_MASK (POS_BIT)

enum {
    PIPE_LAYOUT_MAIN,
};

enum { DESC_SET_MAIN, DESC_SET_DEFERRED, DESC_SET_COUNT };

enum {
    PIPELINE_GBUFFER_POS_NOR_UV,
    PIPELINE_GBUFFER_POS_NOR_UV_TAN,
    PIPELINE_GBUFFER_POS,
    GBUFFER_PIPELINE_COUNT
};

_Static_assert(GBUFFER_PIPELINE_COUNT < OBDN_MAX_PIPELINES,
               "GRAPHICS_PIPELINE_COUNT must be less than OBDN_MAX_PIPELINES");

#define MAX_PRIM_COUNT OBDN_S_MAX_PRIMS

typedef struct {
    Light* light;
    int    count;
} Lights;

// we may be able to use this space to update certain prim transforms
// faster than running through the whole list of them
// especially when dealing with many hundreds of prims.
// currently though this is not used.
typedef struct {
    Mat4 xform[16];
} Xforms;

typedef struct {
    Mat4 view;
    Mat4 proj;
    Mat4 camera;
} Camera;

#define MAX_FRAMES_IN_FLIGHT 2

static VkRenderPass renderpass;
static VkRenderPass gbufferRenderPass;
static VkRenderPass deferredRenderPass;
static uint32_t     graphic_queue_family_index;

static VkFramebuffer framebuffers[MAX_FRAMES_IN_FLIGHT];
static VkFramebuffer gbuffers[MAX_FRAMES_IN_FLIGHT];
static VkFramebuffer swapImageBuffer[MAX_FRAMES_IN_FLIGHT];

static VkPipeline gbufferPipelines[GBUFFER_PIPELINE_COUNT];
static VkPipeline defferedPipeline;

static VkPipeline              raytracePipeline;
static Obdn_ShaderBindingTable shaderBindingTable;

static BufferRegion cameraBuffers[MAX_FRAMES_IN_FLIGHT];
static BufferRegion xformsBuffers[MAX_FRAMES_IN_FLIGHT];
static BufferRegion lightsBuffers[MAX_FRAMES_IN_FLIGHT];
static BufferRegion materialsBuffers[MAX_FRAMES_IN_FLIGHT];

static const Obdn_Scene* scene;

static Obdn_Memory* memory;
static VkDevice     device;

static Obdn_PrimitiveList pipelinePrimLists[GBUFFER_PIPELINE_COUNT];

// raytrace stuff

static struct {
    Hell_Array             array;
    AccelerationStructure* elems;
} blasses;
static AccelerationStructure tlas;

// raytrace stuff

static VkDescriptorSetLayout descriptorSetLayouts[DESC_SET_COUNT];
static Obdn_R_Description    descriptions[MAX_FRAMES_IN_FLIGHT];

static VkPipelineLayout pipelineLayout;

static Image renderTargetDepth;
static Image imageWorldP;
static Image imageNormal;
static Image imageShadow;
static Image imageAlbedo;
static Image imageRoughness;

static Obdn_CommandPool cmdpool;
static VkCommandBuffer  cmdbuffers[MAX_FRAMES_IN_FLIGHT];

static const VkFormat formatImageP = VK_FORMAT_R32G32B32A32_SFLOAT;
static const VkFormat formatImageN = VK_FORMAT_R32G32B32A32_SFLOAT;
static const VkFormat formatImageShadow =
    VK_FORMAT_R16_UINT; // maximum of 16 lights.
static const VkFormat formatImageAlbedo    = VK_FORMAT_R8G8B8A8_UNORM;
static const VkFormat formatImageRoughness = VK_FORMAT_R16_UNORM;

// declarations for overview and navigation
static void initDescriptorSetsAndPipelineLayouts(void);
static void updateDescriptors(void);
static void updateLight(uint32_t frameIndex, uint32_t lightIndex);
static void updateCamera(uint32_t index);
static void syncScene(const uint32_t frameIndex);

void r_InitRenderer(const Obdn_Scene* scene_, VkImageLayout finalImageLayout,
                    bool openglStyle);
void        r_CleanUp(void);
uint8_t
r_GetMaxFramesInFlight(void)
{
    return MAX_FRAMES_IN_FLIGHT;
}

static void
initAttachments(uint32_t windowWidth, uint32_t windowHeight)
{
    renderTargetDepth = obdn_CreateImage(
        memory, windowWidth, windowHeight, VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT, VK_SAMPLE_COUNT_1_BIT, 1,
        OBDN_MEMORY_DEVICE_TYPE);

    imageWorldP = obdn_CreateImage(
        memory, windowWidth, windowHeight, formatImageP,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, 1,
        OBDN_MEMORY_DEVICE_TYPE);

    imageNormal = obdn_CreateImage(
        memory, windowWidth, windowHeight, formatImageN,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, 1,
        OBDN_MEMORY_DEVICE_TYPE);

    imageShadow =
        obdn_CreateImage(memory, windowWidth, windowHeight, formatImageShadow,
                         VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_SAMPLE_COUNT_1_BIT, 1, OBDN_MEMORY_DEVICE_TYPE);

    imageAlbedo = obdn_CreateImage(
        memory, windowWidth, windowHeight, formatImageAlbedo,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, 1,
        OBDN_MEMORY_DEVICE_TYPE);

    imageRoughness = obdn_CreateImage(
        memory, windowWidth, windowHeight, formatImageRoughness,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, 1,
        OBDN_MEMORY_DEVICE_TYPE);

    obdn_TransitionImageLayout(imageShadow.layout, VK_IMAGE_LAYOUT_GENERAL,
                               &imageShadow);
}

static void
initRenderPass(VkDevice device, VkFormat colorFormat, VkFormat depthFormat,
               VkImageLayout finalColorLayout, VkImageLayout finalDepthLayout)
{
    obdn_CreateRenderPass_ColorDepth(
        device, VK_IMAGE_LAYOUT_UNDEFINED, finalColorLayout,
        VK_IMAGE_LAYOUT_UNDEFINED, finalDepthLayout,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE,
        colorFormat, depthFormat, &renderpass);
    printf("Created renderpass 1...\n");

    // gbuffer renderpass
    {
        VkAttachmentDescription attachmentWorldP = {
            .flags          = 0,
            .format         = formatImageP,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout    = VK_IMAGE_LAYOUT_GENERAL};

        VkAttachmentDescription attachmentNormal = {
            .flags          = 0,
            .format         = formatImageN,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout    = VK_IMAGE_LAYOUT_GENERAL};

        VkAttachmentDescription attachmentAlbedo = {
            .flags          = 0,
            .format         = formatImageAlbedo,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout    = VK_IMAGE_LAYOUT_GENERAL};

        VkAttachmentDescription attachmentRoughness = {
            .flags          = 0,
            .format         = formatImageRoughness,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout    = VK_IMAGE_LAYOUT_GENERAL};

        VkAttachmentDescription attachmentDepth = {
            .flags          = 0,
            .format         = depthFormat,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkAttachmentReference refWorldP = {
            .attachment = 0,
            .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        VkAttachmentReference refNormal = {
            .attachment = 1,
            .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        VkAttachmentReference refAlbedo = {
            .attachment = 2,
            .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        VkAttachmentReference refRough = {
            .attachment = 3,
            .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        VkAttachmentReference refDepth = {
            .attachment = 4,
            .layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkAttachmentReference colorRefs[] = {refWorldP, refNormal, refAlbedo,
                                             refRough};

        VkSubpassDescription subpass = {.flags = 0,
                                        .pipelineBindPoint =
                                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        .inputAttachmentCount = 0,
                                        .pInputAttachments    = NULL,
                                        .colorAttachmentCount = LEN(colorRefs),
                                        .pColorAttachments    = colorRefs,
                                        .pResolveAttachments  = NULL,
                                        .pDepthStencilAttachment = &refDepth,
                                        .preserveAttachmentCount = 0,
                                        .pPreserveAttachments    = NULL};

        VkSubpassDependency dep1 = {
            .srcSubpass   = VK_SUBPASS_EXTERNAL,
            .dstSubpass   = 0,
            .srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            .dstStageMask =
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, // may not be
                                                            // necesary
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT};

        VkSubpassDependency dep2 = {
            .srcSubpass = 0,
            .dstSubpass = VK_SUBPASS_EXTERNAL,
            .srcStageMask =
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, // may not be
                                                            // necesary
            .dstStageMask    = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            .srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask   = 0,
            .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT};

        VkSubpassDependency deps[] = {dep1, dep2};

        VkAttachmentDescription attachments[] = {
            attachmentWorldP, attachmentNormal, attachmentAlbedo,
            attachmentRoughness, attachmentDepth};

        VkRenderPassCreateInfo rpiInfo = {
            .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .pNext           = NULL,
            .flags           = 0,
            .attachmentCount = LEN(attachments),
            .pAttachments    = attachments,
            .subpassCount    = 1,
            .pSubpasses      = &subpass,
            .dependencyCount = LEN(deps),
            .pDependencies   = deps};

        V_ASSERT(
            vkCreateRenderPass(device, &rpiInfo, NULL, &gbufferRenderPass));
    }
    printf("Created renderpass 2...\n");

    obdn_CreateRenderPass_Color(device, VK_IMAGE_LAYOUT_UNDEFINED,
                                finalColorLayout, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                colorFormat, &deferredRenderPass);
}

static void
initFramebuffers(const Obdn_Frame* frame)
{
    uint32_t windowWidth = frame->width;
    uint32_t windowHeight = frame->height;
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        {
            const VkImageView   attachments[] = {frame->aovs[0].view,
                                               frame->aovs[1].view};

            const VkFramebufferCreateInfo fbi = {
                .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .pNext           = NULL,
                .flags           = 0,
                .renderPass      = renderpass,
                .attachmentCount = 2,
                .pAttachments    = attachments,
                .width           = windowWidth,
                .height          = windowHeight,
                .layers          = 1,
            };

            V_ASSERT(vkCreateFramebuffer(device, &fbi, NULL, &framebuffers[i]));
        }
        {
            const VkImageView attachments[] = {
                imageWorldP.view, imageNormal.view, imageAlbedo.view,
                imageRoughness.view, renderTargetDepth.view};

            const VkFramebufferCreateInfo fbi = {
                .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .pNext           = NULL,
                .flags           = 0,
                .renderPass      = gbufferRenderPass,
                .attachmentCount = 5,
                .pAttachments    = attachments,
                .width           = windowWidth,
                .height          = windowHeight,
                .layers          = 1};

            V_ASSERT(vkCreateFramebuffer(device, &fbi, NULL, &gbuffers[i]));
        }
        {
            const VkFramebufferCreateInfo fbi = {
                .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .pNext           = NULL,
                .flags           = 0,
                .renderPass      = deferredRenderPass,
                .attachmentCount = 1,
                .pAttachments    = &frame->aovs[0].view,
                .width           = windowWidth,
                .height          = windowHeight,
                .layers          = 1,
            };

            V_ASSERT(
                vkCreateFramebuffer(device, &fbi, NULL, &swapImageBuffer[i]));
        }
    }
}

static void
initDescriptorSetsAndPipelineLayouts(void)
{
    Obdn_DescriptorBinding bindings0[] = {
        {// camera
         .descriptorCount = 1,
         .type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .stageFlags =
             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
        {// xforms
         .descriptorCount = 1,
         .type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT},
        {// lights
         .descriptorCount = 1,
         .type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .stageFlags =
             VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR},
        {                       // textures
         .descriptorCount = 10, // because this is an array of samplers. others
                                // are structs of arrays.
         .type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
         .bindingFlags    = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT},
        {
            // materials
            .descriptorCount = 1, // because this is an array of samplers.
                                  // others are structs of arrays.
            .type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
        }};

    Obdn_DescriptorBinding bindings1[] = {
        {
            // worldp storage image
            .descriptorCount = 1,
            .type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .stageFlags =
                VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        },
        {
            // normal storage image
            .descriptorCount = 1,
            .type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .stageFlags =
                VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        },
        {// albedo storage image
         .descriptorCount = 1,
         .type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT},
        {
            // shadow storage image
            .descriptorCount = 1,
            .type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .stageFlags =
                VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        },
        {// roughness storage image
         .descriptorCount = 1,
         .type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT},
        {
            // top level AS
            .descriptorCount = 1,
            .type            = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            .stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        }};

    Obdn_DescriptorSetInfo descriptorSets[] = {
        {.bindingCount = LEN(bindings0), .bindings = bindings0},
        {.bindingCount = LEN(bindings1), .bindings = bindings1}};

    obdn_CreateDescriptionsAndLayouts(device, LEN(descriptorSets),
                                      descriptorSets, descriptorSetLayouts,
                                      MAX_FRAMES_IN_FLIGHT, descriptions);

    const VkPushConstantRange pcPrimId = {
        .offset = 0,
        .size   = sizeof(Mat4) + sizeof(uint32_t) * 2, // prim id, material id
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT};

    // light count
    const VkPushConstantRange pcFrag = {
        .offset = sizeof(Mat4) + sizeof(uint32_t) * 2, // prim id, material id
        .size   = sizeof(uint32_t),
        .stageFlags =
            VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR};

    const VkPushConstantRange ranges[] = {pcPrimId, pcFrag};

    const Obdn_PipelineLayoutInfo pipeLayoutInfos[] = {
        {.descriptorSetCount   = LEN(descriptorSets),
         .descriptorSetLayouts = descriptorSetLayouts,
         .pushConstantCount    = LEN(ranges),
         .pushConstantsRanges  = ranges}};

    obdn_CreatePipelineLayouts(device, 1, pipeLayoutInfos, &pipelineLayout);
}

static void
initPipelines(bool openglStyle)
{
    const Obdn_GeoAttributeSize posAttrSizes[]          = {12};
    const Obdn_GeoAttributeSize posNormalUvAttrSizes[3] = {12, 12, 8};
    const Obdn_GeoAttributeSize tangetPrimAttrSizes[4]  = {12, 12, 8, 12};

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                      VK_DYNAMIC_STATE_SCISSOR};

    VkSpecializationMapEntry mapEntry = {
        .constantID = 0, .offset = 0, .size = 4};

    int sign = openglStyle ? -1 : 1;

    VkSpecializationInfo fragSpecInfo = {.dataSize      = 4,
                                         .mapEntryCount = 1,
                                         .pData         = &sign,
                                         .pMapEntries   = &mapEntry};

    const Obdn_GraphicsPipelineInfo gPipelineInfos[] = {
        {
            .renderPass      = gbufferRenderPass,
         .primitiveTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .layout          = pipelineLayout,
            .sampleCount     = VK_SAMPLE_COUNT_1_BIT,
            .frontFace       = VK_FRONT_FACE_CLOCKWISE,
            .attachmentCount = 4,
            .vertexDescription =
                obdn_GetVertexDescription(3, posNormalUvAttrSizes),
            .dynamicStateCount = LEN(dynamicStates),
            .pDynamicStates    = dynamicStates,
            .vertShader        = "woad/regular.vert.spv",
            .fragShader        = "woad/gbuffer.frag.spv",
        },
        {.renderPass        = gbufferRenderPass,
         .primitiveTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
         .layout            = pipelineLayout,
         .sampleCount       = VK_SAMPLE_COUNT_1_BIT,
         .frontFace         = VK_FRONT_FACE_CLOCKWISE,
         .attachmentCount   = 4,
         .dynamicStateCount = LEN(dynamicStates),
         .pDynamicStates    = dynamicStates,
         .vertexDescription = obdn_GetVertexDescription(4, tangetPrimAttrSizes),
         .vertShader        = "woad/tangent.vert.spv",
         .fragShader        = "woad/gbuffertan.frag.spv"},
        {.renderPass        = gbufferRenderPass,
         .primitiveTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
         .layout            = pipelineLayout,
         .sampleCount       = VK_SAMPLE_COUNT_1_BIT,
         .frontFace         = VK_FRONT_FACE_CLOCKWISE,
         .attachmentCount   = 4,
         .dynamicStateCount = LEN(dynamicStates),
         .pDynamicStates    = dynamicStates,
         .vertexDescription =
             obdn_GetVertexDescription(LEN(posAttrSizes), posAttrSizes),
         .pFragSpecializationInfo = &fragSpecInfo,
         .vertShader              = "woad/pos.vert.spv",
         .fragShader              = "woad/gbufferpos.frag.spv"}};

    const Obdn_GraphicsPipelineInfo defferedPipeInfo = {
        .renderPass        = deferredRenderPass,
         .primitiveTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .layout            = pipelineLayout,
        .sampleCount       = VK_SAMPLE_COUNT_1_BIT,
        .frontFace         = VK_FRONT_FACE_CLOCKWISE,
        .dynamicStateCount = LEN(dynamicStates),
        .pDynamicStates    = dynamicStates,
        .vertShader        = OBDN_FULL_SCREEN_VERT_SPV,
        .fragShader        = "woad/deferred.frag.spv"};

    const Obdn_RayTracePipelineInfo rtPipelineInfo = {
        .layout        = pipelineLayout,
        .raygenCount   = 1,
        .raygenShaders = (char*[]){"woad/shadow.rgen.spv"},
        .missCount     = 1,
        .missShaders   = (char*[]){"woad/shadow.rmiss.spv"},
        .chitCount     = 1,
        .chitShaders   = (char*[]){"woad/shadow.rchit.spv"}};

    assert(LEN(gPipelineInfos) == GBUFFER_PIPELINE_COUNT);

    obdn_CreateGraphicsPipelines(device, LEN(gPipelineInfos), gPipelineInfos,
                                 gbufferPipelines);
    obdn_CreateGraphicsPipelines(device, 1, &defferedPipeInfo,
                                 &defferedPipeline);
    obdn_CreateRayTracePipelines(device, memory, 1, &rtPipelineInfo,
                                 &raytracePipeline, &shaderBindingTable);
}

static void
updateGbufferDescriptors(void)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        VkDescriptorImageInfo worldPInfo = {.sampler   = imageWorldP.sampler,
                                            .imageView = imageWorldP.view,
                                            .imageLayout =
                                                VK_IMAGE_LAYOUT_GENERAL};

        VkDescriptorImageInfo normalInfo = {.sampler   = imageNormal.sampler,
                                            .imageView = imageNormal.view,
                                            .imageLayout =
                                                VK_IMAGE_LAYOUT_GENERAL};

        VkDescriptorImageInfo albedoInfo = {.sampler   = imageAlbedo.sampler,
                                            .imageView = imageAlbedo.view,
                                            .imageLayout =
                                                VK_IMAGE_LAYOUT_GENERAL};

        VkDescriptorImageInfo shadowInfo = {.sampler   = imageShadow.sampler,
                                            .imageView = imageShadow.view,
                                            .imageLayout =
                                                VK_IMAGE_LAYOUT_GENERAL};

        VkDescriptorImageInfo roughnessInfo = {
            .sampler     = imageRoughness.sampler,
            .imageView   = imageRoughness.view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL};

        VkWriteDescriptorSet writes[] = {
            {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstArrayElement = 0,
             .dstSet     = descriptions[i].descriptorSets[DESC_SET_DEFERRED],
             .dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .pImageInfo      = &worldPInfo},
            {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstArrayElement = 0,
             .dstSet     = descriptions[i].descriptorSets[DESC_SET_DEFERRED],
             .dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .pImageInfo      = &normalInfo},
            {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstArrayElement = 0,
             .dstSet     = descriptions[i].descriptorSets[DESC_SET_DEFERRED],
             .dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .pImageInfo      = &albedoInfo},
            {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstArrayElement = 0,
             .dstSet     = descriptions[i].descriptorSets[DESC_SET_DEFERRED],
             .dstBinding = 3,
             .descriptorCount = 1,
             .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .pImageInfo      = &shadowInfo},
            {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstArrayElement = 0,
             .dstSet     = descriptions[i].descriptorSets[DESC_SET_DEFERRED],
             .dstBinding = 4,
             .descriptorCount = 1,
             .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .pImageInfo      = &roughnessInfo}};

        vkUpdateDescriptorSets(device, LEN(writes), writes, 0, NULL);
    }
}

static void
updateASDescriptors(void)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        VkWriteDescriptorSetAccelerationStructureKHR asInfo = {
            .sType =
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
            .accelerationStructureCount = 1,
            .pAccelerationStructures    = &tlas.handle};

        VkWriteDescriptorSet writeDS = {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet     = descriptions[i].descriptorSets[DESC_SET_DEFERRED],
            .dstBinding = 5,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            .pNext           = &asInfo};

        vkUpdateDescriptorSets(device, 1, &writeDS, 0, NULL);
    }
}

static void
updateDescriptors(void)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        // camera creation
        cameraBuffers[i] = obdn_RequestBufferRegion(
            memory, sizeof(Camera), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            OBDN_MEMORY_HOST_GRAPHICS_TYPE);

        // xforms creation
        xformsBuffers[i] = obdn_RequestBufferRegion(
            memory, sizeof(Xforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            OBDN_MEMORY_HOST_GRAPHICS_TYPE);

        // lights creation
        lightsBuffers[i] = obdn_RequestBufferRegion(
            memory, sizeof(Lights), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            OBDN_MEMORY_HOST_GRAPHICS_TYPE);

        materialsBuffers[i] = obdn_RequestBufferRegion(
            memory, sizeof(Material) * OBDN_S_MAX_MATERIALS,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, OBDN_MEMORY_HOST_GRAPHICS_TYPE);

        VkDescriptorBufferInfo camInfo = {.buffer = cameraBuffers[i].buffer,
                                          .offset = cameraBuffers[i].offset,
                                          .range  = cameraBuffers[i].size};

        VkDescriptorBufferInfo xformInfo = {.buffer = xformsBuffers[i].buffer,
                                            .offset = xformsBuffers[i].offset,
                                            .range  = xformsBuffers[i].size};

        VkDescriptorBufferInfo lightInfo = {.buffer = lightsBuffers[i].buffer,
                                            .offset = lightsBuffers[i].offset,
                                            .range  = lightsBuffers[i].size};

        VkDescriptorBufferInfo materialInfo = {
            .buffer = materialsBuffers[i].buffer,
            .offset = materialsBuffers[i].offset,
            .range  = materialsBuffers[i].size};

        VkWriteDescriptorSet writes[] = {
            {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstArrayElement = 0,
             .dstSet          = descriptions[i].descriptorSets[DESC_SET_MAIN],
             .dstBinding      = 0,
             .descriptorCount = 1,
             .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
             .pBufferInfo     = &camInfo},
            {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstArrayElement = 0,
             .dstSet          = descriptions[i].descriptorSets[DESC_SET_MAIN],
             .dstBinding      = 1,
             .descriptorCount = 1,
             .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
             .pBufferInfo     = &xformInfo},
            {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstArrayElement = 0,
             .dstSet          = descriptions[i].descriptorSets[DESC_SET_MAIN],
             .dstBinding      = 2,
             .descriptorCount = 1,
             .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
             .pBufferInfo     = &lightInfo},
            {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstArrayElement = 0,
             .dstSet          = descriptions[i].descriptorSets[DESC_SET_MAIN],
             .dstBinding      = 4,
             .descriptorCount = 1,
             .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
             .pBufferInfo     = &materialInfo}};

        vkUpdateDescriptorSets(device, LEN(writes), writes, 0, NULL);
    }

    updateGbufferDescriptors();
}

static void
updateTexture(const uint32_t frameIndex, const Obdn_Image* img,
              const uint32_t texId)
{
    VkDescriptorImageInfo textureInfo = {.imageLayout = img->layout,
                                         .imageView   = img->view,
                                         .sampler     = img->sampler};

    VkWriteDescriptorSet write = {
        .sType      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext      = NULL,
        .dstSet     = descriptions[frameIndex].descriptorSets[DESC_SET_MAIN],
        .dstBinding = 3,
        .dstArrayElement = texId,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = &textureInfo};

    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

    printf("Updated Texture %d frame %d\n", texId, frameIndex);
}

static void
generateGBuffer(VkCommandBuffer cmdBuf, const uint32_t frameIndex, uint32_t windowWidth, uint32_t windowHeight)
{
    VkClearValue clearValueColor = {1.0f, 0.0f, 0.0f, 0.0f};
    VkClearValue clearValueMatid = {0};
    VkClearValue clearValueDepth = {1.0, 0};

    VkClearValue clears[] = {clearValueColor, clearValueColor, clearValueColor,
                             clearValueMatid, clearValueDepth};

    VkRenderPassBeginInfo rpassInfo = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .clearValueCount = LEN(clears),
        .pClearValues    = clears,
        .renderArea      = {{0, 0}, {windowWidth, windowHeight}},
        .renderPass      = gbufferRenderPass,
        .framebuffer     = gbuffers[frameIndex]};

    vkCmdBeginRenderPass(cmdBuf, &rpassInfo, VK_SUBPASS_CONTENTS_INLINE);

    for (int pipeId = 0; pipeId < GBUFFER_PIPELINE_COUNT; pipeId++)
    {
        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          gbufferPipelines[pipeId]);
        uint32_t primCount;
        const Obdn_PrimitiveHandle* prim_handles = obdn_GetPrimlistPrims(&pipelinePrimLists[pipeId], &primCount);
        printf("Pipeline %d, primCount %d\n", pipeId, primCount);
        for (int i = 0; i < primCount; i++)
        {
            Obdn_PrimitiveHandle prim_handle = prim_handles[i];
            const Obdn_Primitive* prim =
                obdn_SceneGetPrimitiveConst(scene, prim_handle);
            Obdn_MaterialHandle matId = prim->material;
            Obdn_Xform          xform = prim->xform;
            vkCmdPushConstants(cmdBuf, pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4),
                               xform.e);
            vkCmdPushConstants(cmdBuf, pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT, sizeof(Mat4),
                               sizeof(uint32_t), &prim_handle.id);
            vkCmdPushConstants(
                cmdBuf, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                sizeof(Mat4) + sizeof(uint32_t), sizeof(uint32_t), &matId);
            obdn_DrawGeo(cmdBuf, prim->geo);
        }
    }

    vkCmdEndRenderPass(cmdBuf);
}

static void
shadowPass(VkCommandBuffer cmdBuf, const uint32_t frameIndex, uint32_t windowWidth, uint32_t windowHeight)
{
    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                      raytracePipeline);

    vkCmdTraceRaysKHR(
        cmdBuf, &shaderBindingTable.raygenTable, &shaderBindingTable.missTable,
        &shaderBindingTable.hitTable, &shaderBindingTable.callableTable,
        windowWidth, windowHeight, 1);
}

static void
deferredRender(VkCommandBuffer cmdBuf, const uint32_t frameIndex, uint32_t windowWidth, uint32_t windowHeight)
{
    VkClearValue clearValueColor = {1.0f, 0.0f, 0.0f, 0.0f};

    VkRenderPassBeginInfo rpassInfo = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .clearValueCount = 1,
        .pClearValues    = &clearValueColor,
        .renderArea      = {{0, 0}, {windowWidth, windowHeight}},
        .renderPass      = deferredRenderPass,
        .framebuffer     = swapImageBuffer[frameIndex]};

    vkCmdBeginRenderPass(cmdBuf, &rpassInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      defferedPipeline);

    vkCmdDraw(cmdBuf, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmdBuf);
}

static void
sortPipelinePrims(void)
{
    for (int i = 0; i < GBUFFER_PIPELINE_COUNT; i++)
    {
        obdn_ClearPrimList(&pipelinePrimLists[i]);
    }
    obint                 prim_count = 0;
    const Obdn_Primitive* prims = obdn_SceneGetPrimitives(scene, &prim_count);
    for (obint primId = 0; primId < prim_count; primId++)
    {
        AttrMask              attrMask = 0;
        const Obdn_Primitive* prim     = &prims[primId];
        const Obdn_Geometry*  geo      = prim->geo;
        for (int i = 0; i < geo->attrCount; i++)
        {
            const char* name = geo->attrNames[i];
            if (strcmp(name, POS_NAME) == 0)
                attrMask |= POS_BIT;
            if (strcmp(name, NORMAL_NAME) == 0)
                attrMask |= NORMAL_BIT;
            if (strcmp(name, UV_NAME) == 0)
                attrMask |= UV_BIT;
            if (strcmp(name, TANGENT_NAME) == 0)
                attrMask |= TAN_BIT;
        }
        Obdn_PrimitiveHandle h = obdn_CreatePrimitiveHandle(primId);
        if (attrMask == POS_NOR_UV_TAN_MASK)
            obdn_AddPrimToList(
                h, &pipelinePrimLists[PIPELINE_GBUFFER_POS_NOR_UV_TAN]);
        else if (attrMask == POS_NOR_UV_MASK)
            obdn_AddPrimToList(h,
                               &pipelinePrimLists[PIPELINE_GBUFFER_POS_NOR_UV]);
        else if (attrMask == POS_MASK)
            obdn_AddPrimToList(h, &pipelinePrimLists[PIPELINE_GBUFFER_POS]);
        else
        {
            printf("Attributes not supported!\n");
            assert(0);
        }
        // if (mat->textureAlbedo && mat->textureRoughness &&
        // mat->textureNormal)
        //     addPrimToPipelinePrimList(primId,
        //     &pipelinePrimLists[PIPELINE_TAN]);
        // else if (mat->textureAlbedo && mat->textureRoughness)
        //     addPrimToPipelinePrimList(primId,
        //     &pipelinePrimLists[PIPELINE_REG]);
        // else if (mat->textureAlbedo + mat->textureNormal + mat->textureNormal
        // == 0 )
        //     addPrimToPipelinePrimList(primId,
        //     &pipelinePrimLists[PIPELINE_NO_MAPS]);
        // else
        //     assert(0 && "currently prims must have albedo and roughness
        //     textures");
    }
}

static void
updateRenderCommands(VkCommandBuffer cmdBuf, const uint32_t frameIndex, uint32_t windowWidth, uint32_t windowHeight)
{

#if 0
    VkViewport viewport = {
        .width = (float)windowWidth,
        .height = -(float)windowHeight,
        .minDepth = 0.0,
        .maxDepth = 1.0,
        .x = 0, .y = (float)windowHeight
    };
#else
    VkViewport viewport = {.width    = (float)windowWidth,
                           .height   = (float)windowHeight,
                           .minDepth = 0.0,
                           .maxDepth = 1.0,
                           .x        = 0,
                           .y        = 0};
#endif

    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, 0, 2,
                            descriptions[frameIndex].descriptorSets, 0, NULL);

    printf("Viewport: w %f, h %f\n", viewport.width, viewport.height);

    VkRect2D scissor = {.extent = {windowWidth, windowHeight},
                        .offset = {0, 0}};

    vkCmdSetViewport(cmdBuf, 0, 1, &viewport);
    vkCmdSetScissor(cmdBuf, 0, 1, &scissor);

    uint32_t light_count = obdn_SceneGetLightCount(scene);
    vkCmdPushConstants(
        cmdBuf, pipelineLayout,
        VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        sizeof(Mat4) + sizeof(uint32_t) * 2, sizeof(uint32_t), &light_count);

    generateGBuffer(cmdBuf, frameIndex, windowWidth, windowHeight);

    obdn_v_MemoryBarrier(cmdBuf, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT);

    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            pipelineLayout, 0, 2,
                            descriptions[frameIndex].descriptorSets, 0, NULL);

    shadowPass(cmdBuf, frameIndex, windowWidth, windowHeight);

    obdn_v_MemoryBarrier(cmdBuf, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, 0, 2,
                            descriptions[frameIndex].descriptorSets, 0, NULL);

    // viewport.height = windowHeight;
    // viewport.y = 0;

    // vkCmdSetViewport(cmdBuf, 0, 1, &viewport);

    deferredRender(cmdBuf, frameIndex, windowWidth, windowHeight);
}

static void
cleanUpSwapchainDependent(void)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkDestroyFramebuffer(device, framebuffers[i], NULL);
        vkDestroyFramebuffer(device, gbuffers[i], NULL);
        vkDestroyFramebuffer(device, swapImageBuffer[i], NULL);
    }
    obdn_FreeImage(&renderTargetDepth);
    obdn_FreeImage(&imageWorldP);
    obdn_FreeImage(&imageNormal);
    obdn_FreeImage(&imageShadow);
    obdn_FreeImage(&imageRoughness);
    obdn_FreeImage(&imageAlbedo);
}

static void
onSwapchainRecreate(const Obdn_Frame* fb)
{
    vkDeviceWaitIdle(device);
    cleanUpSwapchainDependent();
    initAttachments(fb->width, fb->height);
    initFramebuffers(fb);
    updateGbufferDescriptors();
}

static void
updateCamera(uint32_t index)
{
    Camera* uboCam = (Camera*)cameraBuffers[index].hostData;
    uboCam->view   = obdn_SceneGetCameraView(scene);
    uboCam->proj   = obdn_SceneGetCameraProjection(scene);
    // printf("Proj:\n");
    // coal_PrintMat4(&proj);
    // printf("View:\n");
    // coal_PrintMat4(&view);
    uboCam->camera = obdn_SceneGetCameraXform(scene);
}

static void
updateFastXforms(uint32_t frameIndex, uint32_t primIndex)
{
    assert(primIndex < 16);
    Xforms* xforms = (Xforms*)xformsBuffers[frameIndex].hostData;
    // coal_Copy_Mat4(scene->xforms[primIndex], xforms->xform[primIndex].x);
}

static void
updateLight(uint32_t frameIndex, uint32_t lightIndex)
{
    Lights*           lights = (Lights*)lightsBuffers[frameIndex].hostData;
    const Obdn_Light* l =
        Obdn_SceneGetLight(scene, obdn_CreateLightHandle(lightIndex));
    lights->light[lightIndex] = *l;
}

static void
updateMaterials(uint32_t frameIndex)
{
    obint                matcount  = 0;
    const Obdn_Material* materials = obdn_SceneGetMaterials(scene, &matcount);
    memcpy(materialsBuffers[frameIndex].hostData, materials,
           sizeof(Material) * matcount);
}

static void
buildAccelerationStructures(void)
{
    Hell_Array xforms;
    hell_CreateArray(8, sizeof(Coal_Mat4), NULL, NULL, &xforms);
    obint                 prim_count = 0;
    const Obdn_Primitive* prims = obdn_SceneGetPrimitives(scene, &prim_count);
    for (int i = 0; i < blasses.array.count; i++)
    {
        AccelerationStructure* blas = &blasses.elems[i];
        obdn_DestroyAccelerationStruct(device, blas);
    }
    if (tlas.bufferRegion.size != 0)
        obdn_DestroyAccelerationStruct(device, &tlas);
    hell_ArrayClear(&blasses.array);
    if (prim_count > 0)
    {
        for (int i = 0; i < prim_count; i++)
        {
            AccelerationStructure blas = {};
            obdn_BuildBlas(memory, prims[i].geo, &blas);
            hell_ArrayPush(&blasses.array, &blas);
            hell_ArrayPush(&xforms, &prims[i].xform);
        }
        obdn_BuildTlas(memory, prim_count, blasses.elems, xforms.elems, &tlas);
    }
    printf(">>>>> Built acceleration structures\n");
}

void
woad_Render(const Obdn_Scene* scene, const Obdn_Frame* fb, VkCommandBuffer cmdbuf)
{
    static uint8_t cameraNeedUpdate    = MAX_FRAMES_IN_FLIGHT;
    // static uint8_t xformsNeedUpdate    = MAX_FRAMES_IN_FLIGHT;
    static uint8_t lightsNeedUpdate    = MAX_FRAMES_IN_FLIGHT;
    static uint8_t texturesNeedUpdate  = MAX_FRAMES_IN_FLIGHT;
    static uint8_t materialsNeedUpdate = MAX_FRAMES_IN_FLIGHT;
    static uint8_t framesNeedUpdate    = MAX_FRAMES_IN_FLIGHT;

    int frameIndex = fb->index;
    Obdn_SceneDirtyFlags scene_dirt = obdn_SceneGetDirt(scene);
    if (scene_dirt)
    {
        if (scene_dirt & OBDN_SCENE_CAMERA_VIEW_BIT)
            cameraNeedUpdate = MAX_FRAMES_IN_FLIGHT;
        if (scene_dirt & OBDN_SCENE_CAMERA_PROJ_BIT)
            cameraNeedUpdate = MAX_FRAMES_IN_FLIGHT;
        if (scene_dirt & OBDN_SCENE_LIGHTS_BIT)
        {
            lightsNeedUpdate = MAX_FRAMES_IN_FLIGHT;
            framesNeedUpdate = MAX_FRAMES_IN_FLIGHT;
        }
        if (scene_dirt & OBDN_SCENE_XFORMS_BIT)
            framesNeedUpdate = MAX_FRAMES_IN_FLIGHT;
        if (scene_dirt & OBDN_SCENE_MATERIALS_BIT)
            materialsNeedUpdate = MAX_FRAMES_IN_FLIGHT;
        if (scene_dirt & OBDN_SCENE_TEXTURES_BIT)
        {
            texturesNeedUpdate = framesNeedUpdate = MAX_FRAMES_IN_FLIGHT;
        }
        if (scene_dirt & OBDN_SCENE_PRIMS_BIT)
        {
            printf("TANTO: PRIMS DIRTY\n");
            sortPipelinePrims();
            buildAccelerationStructures();
            updateASDescriptors();
            framesNeedUpdate = MAX_FRAMES_IN_FLIGHT;
        }
        if (fb->dirty)
        {
            onSwapchainRecreate(fb);
            framesNeedUpdate = MAX_FRAMES_IN_FLIGHT;
        }
    }
    if (cameraNeedUpdate)
    {
        updateCamera(frameIndex);
        cameraNeedUpdate--;
    }
    if (lightsNeedUpdate)
    {
        obint light_count = obdn_SceneGetLightCount(scene);
        for (int i = 0; i < light_count; i++)
            updateLight(frameIndex, i);
        lightsNeedUpdate--;
        printf("Tanto: lights sync\n");
        obdn_PrintLightInfo(scene);
    }
    if (materialsNeedUpdate)
    {
        updateMaterials(frameIndex);
        materialsNeedUpdate--;
    }
    if (texturesNeedUpdate) // TODO update all tex
    {
        printf("texturesNeedUpdate %d\n", texturesNeedUpdate);
        obint               tex_count = 0;
        const Obdn_Texture* tex = obdn_SceneGetTextures(scene, &tex_count);
        // remember, 1 is the first valid texture index
        for (int i = 1; i <= tex_count; i++)
        {
            updateTexture(frameIndex, tex[i].devImage, i);
        }
        texturesNeedUpdate--;
    }
    if (framesNeedUpdate)
    {
        updateRenderCommands(cmdbuf, frameIndex, fb->width, fb->height);
        framesNeedUpdate--;
    }
}

void
woad_Init(Obdn_Instance* instance, Obdn_Memory* memory_,
                  VkImageLayout finalColorLayout,
                  VkImageLayout finalDepthLayout, uint32_t fbCount,
                  const Obdn_Frame fbs[/*fbCount*/])
{
    hell_Print("Creating Woad renderer...\n");
    for (int i = 0; i < GBUFFER_PIPELINE_COUNT; i++)
    {
        pipelinePrimLists[i] = obdn_CreatePrimList(8);
    }

    device = obdn_GetDevice(instance);
    graphic_queue_family_index =
        obdn_GetQueueFamilyIndex(instance, OBDN_V_QUEUE_GRAPHICS_TYPE);
    memory = memory_;

    initAttachments(fbs[0].width, fbs[0].height);
    hell_Print(">> Woad: attachments initialized. \n");
    initRenderPass(device, fbs[0].aovs[0].format, fbs[0].aovs[1].format,
                   finalColorLayout, finalDepthLayout);
    hell_Print(">> Woad: renderpasses initialized. \n");
    for (int i = 0; i < fbCount; i++) 
    {
        initFramebuffers(&fbs[i]);
    }
    hell_Print(">> Woad: framebuffers initialized. \n");
    initDescriptorSetsAndPipelineLayouts();
    hell_Print(
        ">> Woad: descriptor sets and pipeline layouts initialized. \n");
    updateDescriptors();
    hell_Print(">> Woad: descriptors updated. \n");
    initPipelines(false);
    hell_Print(">> Woad: pipelines initialized. \n");
    hell_Print(">> Woad: initialization complete. \n");
}

void
woad_Cleanup(void)
{
    cleanUpSwapchainDependent();
    for (int i = 0; i < GBUFFER_PIPELINE_COUNT; i++)
    {
        vkDestroyPipeline(device, gbufferPipelines[i], NULL);
    }
    vkDestroyPipeline(device, defferedPipeline, NULL);
    vkDestroyPipeline(device, raytracePipeline, NULL);
    for (int i = 0; i < blasses.array.count; i++)
    {
        AccelerationStructure* blas = &blasses.elems[i];
        if (blas->bufferRegion.size != 0)
            obdn_DestroyAccelerationStruct(device, blas);
    }
    if (tlas.bufferRegion.size != 0)
        obdn_DestroyAccelerationStruct(device, &tlas);
    obdn_DestroyShaderBindingTable(&shaderBindingTable);
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        obdn_DestroyDescription(device, &descriptions[i]);
        vkDestroyDescriptorSetLayout(device, descriptorSetLayouts[i], NULL);
        obdn_FreeBufferRegion(&cameraBuffers[i]);
        obdn_FreeBufferRegion(&xformsBuffers[i]);
        obdn_FreeBufferRegion(&lightsBuffers[i]);
        obdn_FreeBufferRegion(&materialsBuffers[i]);
    }
    vkDestroyRenderPass(device, renderpass, NULL);
    vkDestroyRenderPass(device, gbufferRenderPass, NULL);
    vkDestroyRenderPass(device, deferredRenderPass, NULL);
    vkDestroyPipelineLayout(device, pipelineLayout, NULL);
}
