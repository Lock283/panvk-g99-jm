/*
 * T4.5.18 -- descriptor types beyond plain uniform buffers, Valhall v9 / JM.
 *
 * Phase 4.2 only ever used VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, one binding per
 * set, so what it supports is "uniform buffers across several sets work". This
 * test covers storage buffers, dynamic uniform and storage buffers, static and
 * dynamic bindings mixed in one set, several bindings in one set, and an array
 * of descriptors in one binding.
 *
 * Every value is k/255, so the UNORM8 output is exactly k and the comparison is
 * exact, with no tolerance.
 *
 * All data lives in one buffer split into 256-byte slots. Each slot holds a vec4
 * with the same value in all four lanes. A descriptor points at a slot; a
 * dynamic offset moves it to another slot.
 *
 * What each case rules out:
 *   static4 / static4_alt  same shader, different buffer contents. If the pixel
 *                          does not follow, values were folded at compile time.
 *   dyn4                   the descriptor's own slot holds a DECOY; the real
 *                          value is only reachable through the dynamic offset.
 *                          Ignored dynamic offsets show the decoy.
 *   dyn4_zero              negative control: same as dyn4 with every dynamic
 *                          offset 0, so the decoy MUST appear. Proves the decoy
 *                          is reachable and the offsets are what selects.
 *   mixed                  static and dynamic bindings interleaved, and the two
 *                          dynamic bindings get DIFFERENT offsets. The slot each
 *                          binding would reach with the other binding's offset
 *                          holds a TRAP value, so an offset applied to the wrong
 *                          binding shows the trap.
 *   array / array_alt      one binding, three uniform buffers, constant indices.
 *
 * Select with DESC_CASE. Image descriptors (samplers, sampled and storage
 * images) are deliberately not here: they belong to texture sampling, Phase 5.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "vulkan/vulkan_core.h"

#define CHECK(expr, msg) do { \
    VkResult _r = (expr); \
    if (_r != VK_SUCCESS) { \
        printf("FAILED: %s (VkResult=%d)\n", msg, _r); \
        return 1; \
    } \
    printf("%s ok\n", msg); \
    fflush(stdout); \
} while (0)

typedef PFN_vkVoidFunction (*PFN_icdGetInstanceProcAddr)(VkInstance, const char*);

static uint32_t find_memory_type(VkPhysicalDeviceMemoryProperties *mp,
                                  uint32_t type_bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (mp->memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    return UINT32_MAX;
}

static char *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAILED: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) {
        printf("FAILED: short read on %s\n", path); fclose(f); free(buf); return NULL;
    }
    fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

#ifndef PANVK_DEFAULT_ICD_SO
#define PANVK_DEFAULT_ICD_SO \
    "/data/data/com.termux/files/home/panvk-g57/mesa/build/src/panfrost/vulkan/libvulkan_panfrost.so"
#endif

#define NB 4
#define SLOT 256u
#define NSLOTS 16u
#define K(v) ((float)(v) / 255.0f)

struct dcase {
    const char *name;
    const char *fs_spv;
    uint32_t nbind;                  /* bindings in the set                    */
    uint32_t array_count;            /* >1: binding 0 is an array of this size */
    VkDescriptorType type[NB];
    uint32_t base_slot[NB];          /* slot the descriptor itself points at  */
    uint32_t dyn_slots[NB];          /* dynamic offset in slots, per binding  */
    uint8_t  slot_val[NSLOTS];       /* k for each slot, 0 = unused           */
    uint8_t  expect[4];              /* exact expected centre pixel           */
};

#define UBO  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
#define SSBO VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
#define UBOD VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
#define SSBD VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC

static int is_dyn(VkDescriptorType t) { return t == UBOD || t == SSBD; }

int main(void) {
    const char *cname = getenv("DESC_CASE");
    if (!cname || !cname[0]) cname = "static4";

    /* decoy = 20, traps = 230 / 240. None of them is an expected value. */
    struct dcase cases[] = {
        { "static4", "desc4.frag.spv", 4, 1, {UBO,SSBO,UBO,SSBO},
          {0,1,2,3}, {0,0,0,0},
          {[0]=200,[1]=150,[2]=100,[3]=50}, {200,150,100,50} },
        { "static4_alt", "desc4.frag.spv", 4, 1, {UBO,SSBO,UBO,SSBO},
          {0,1,2,3}, {0,0,0,0},
          {[0]=50,[1]=100,[2]=150,[3]=200}, {50,100,150,200} },
        { "dyn4", "desc4.frag.spv", 4, 1, {UBOD,SSBD,UBOD,SSBD},
          {0,1,2,3}, {4,4,4,4},
          {[0]=20,[1]=20,[2]=20,[3]=20,[4]=200,[5]=150,[6]=100,[7]=50},
          {200,150,100,50} },
        { "dyn4_zero", "desc4.frag.spv", 4, 1, {UBOD,SSBD,UBOD,SSBD},
          {0,1,2,3}, {0,0,0,0},
          {[0]=20,[1]=20,[2]=20,[3]=20,[4]=200,[5]=150,[6]=100,[7]=50},
          {20,20,20,20} },
        /* bindings 1 and 2 dynamic, offsets 4 and 6 slots.
         * correct:  b1 -> 1+4 = 5,  b2 -> 2+6 = 8
         * swapped:  b1 -> 1+6 = 7,  b2 -> 2+4 = 6   (traps) */
        { "mixed", "desc4.frag.spv", 4, 1, {UBO,SSBD,UBOD,SSBO},
          {0,1,2,3}, {0,4,6,0},
          {[0]=200,[1]=20,[2]=20,[3]=50,[5]=150,[8]=100,[6]=230,[7]=240},
          {200,150,100,50} },
        { "array", "descarr.frag.spv", 1, 3, {UBO},
          {0}, {0},
          {[0]=200,[1]=150,[2]=100}, {200,150,100,255} },
        { "array_alt", "descarr.frag.spv", 1, 3, {UBO},
          {0}, {0},
          {[0]=100,[1]=200,[2]=150}, {100,200,150,255} },
    };
    struct dcase *cfg = NULL;
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++)
        if (!strcmp(cname, cases[i].name)) cfg = &cases[i];
    if (!cfg) { printf("FAILED: unknown DESC_CASE=%s\n", cname); return 1; }

    static const char *tn[] = {[UBO]="UBO",[SSBO]="SSBO",[UBOD]="UBO_DYN",[SSBD]="SSBO_DYN"};
    printf("=== case: %s ===\n", cfg->name);
    for (uint32_t b = 0; b < cfg->nbind; b++)
        printf("binding %u      : %-8s x%u base slot %u dyn +%u slots\n", b,
               tn[cfg->type[b]], b == 0 ? cfg->array_count : 1,
               cfg->base_slot[b], cfg->dyn_slots[b]);
    printf("expected pixel : %u,%u,%u,%u (exact)\n",
           cfg->expect[0], cfg->expect[1], cfg->expect[2], cfg->expect[3]);
    fflush(stdout);

    const char *icd_path = getenv("PANVK_ICD_SO");
    if (!icd_path || !icd_path[0]) icd_path = PANVK_DEFAULT_ICD_SO;
    printf("loading ICD: %s\n", icd_path);
    void *lib = dlopen(icd_path, RTLD_NOW);
    if (!lib) { printf("dlopen failed: %s\n", dlerror()); return 1; }

    PFN_icdGetInstanceProcAddr icd_gpa =
        (PFN_icdGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    if (!icd_gpa) { printf("dlsym failed: %s\n", dlerror()); return 1; }

    PFN_vkCreateInstance CreateInstance =
        (PFN_vkCreateInstance)icd_gpa(NULL, "vkCreateInstance");

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo inst_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
    };
    VkInstance instance;
    CHECK(CreateInstance(&inst_info, NULL, &instance), "vkCreateInstance");

    #define IPROC(name) (PFN_vk##name)icd_gpa(instance, "vk" #name)
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = IPROC(GetDeviceProcAddr);
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = IPROC(EnumeratePhysicalDevices);
    PFN_vkCreateDevice CreateDevice = IPROC(CreateDevice);
    PFN_vkGetPhysicalDeviceMemoryProperties GetMemoryProperties = IPROC(GetPhysicalDeviceMemoryProperties);

    uint32_t count = 0;
    CHECK(EnumeratePhysicalDevices(instance, &count, NULL), "EnumeratePhysicalDevices(count)");
    if (count == 0) { printf("FAILED: no physical device\n"); return 1; }
    VkPhysicalDevice pdev;
    CHECK(EnumeratePhysicalDevices(instance, &count, &pdev), "EnumeratePhysicalDevices(fetch)");

    VkPhysicalDeviceMemoryProperties mem_props;
    GetMemoryProperties(pdev, &mem_props);

    VkPhysicalDeviceVulkan13Features features13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .dynamicRendering = VK_TRUE,
    };
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo dev_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features13,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
    };
    VkDevice device;
    CHECK(CreateDevice(pdev, &dev_info, NULL, &device), "vkCreateDevice");

    #define DPROC(name) (PFN_vk##name)GetDeviceProcAddr(device, "vk" #name)
    PFN_vkGetDeviceQueue GetDeviceQueue = DPROC(GetDeviceQueue);
    PFN_vkCreateCommandPool CreateCommandPool = DPROC(CreateCommandPool);
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = DPROC(AllocateCommandBuffers);
    PFN_vkBeginCommandBuffer BeginCommandBuffer = DPROC(BeginCommandBuffer);
    PFN_vkEndCommandBuffer EndCommandBuffer = DPROC(EndCommandBuffer);
    PFN_vkQueueSubmit QueueSubmit = DPROC(QueueSubmit);
    PFN_vkQueueWaitIdle QueueWaitIdle = DPROC(QueueWaitIdle);
    PFN_vkCreateImage CreateImage = DPROC(CreateImage);
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = DPROC(GetImageMemoryRequirements);
    PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout = DPROC(GetImageSubresourceLayout);
    PFN_vkAllocateMemory AllocateMemory = DPROC(AllocateMemory);
    PFN_vkBindImageMemory BindImageMemory = DPROC(BindImageMemory);
    PFN_vkCreateImageView CreateImageView = DPROC(CreateImageView);
    PFN_vkMapMemory MapMemory = DPROC(MapMemory);
    PFN_vkUnmapMemory UnmapMemory = DPROC(UnmapMemory);
    PFN_vkCreateShaderModule CreateShaderModule = DPROC(CreateShaderModule);
    PFN_vkCreatePipelineLayout CreatePipelineLayout = DPROC(CreatePipelineLayout);
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = DPROC(CreateGraphicsPipelines);
    PFN_vkCmdBeginRendering CmdBeginRendering = DPROC(CmdBeginRendering);
    PFN_vkCmdEndRendering CmdEndRendering = DPROC(CmdEndRendering);
    PFN_vkCmdBindPipeline CmdBindPipeline = DPROC(CmdBindPipeline);
    PFN_vkCmdSetViewport CmdSetViewport = DPROC(CmdSetViewport);
    PFN_vkCmdSetScissor CmdSetScissor = DPROC(CmdSetScissor);
    PFN_vkCmdDraw CmdDraw = DPROC(CmdDraw);
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = DPROC(CmdPipelineBarrier);
    /* descriptor-set specific */
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = DPROC(CreateDescriptorSetLayout);
    PFN_vkCreateDescriptorPool CreateDescriptorPool = DPROC(CreateDescriptorPool);
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets = DPROC(AllocateDescriptorSets);
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets = DPROC(UpdateDescriptorSets);
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = DPROC(CmdBindDescriptorSets);
    PFN_vkCreateBuffer CreateBuffer = DPROC(CreateBuffer);
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = DPROC(GetBufferMemoryRequirements);
    PFN_vkBindBufferMemory BindBufferMemory = DPROC(BindBufferMemory);

    if (!CreateDescriptorSetLayout || !CreateDescriptorPool || !AllocateDescriptorSets ||
        !UpdateDescriptorSets || !CmdBindDescriptorSets || !CreateBuffer ||
        !GetBufferMemoryRequirements || !BindBufferMemory) {
        printf("FAILED: a descriptor-set entry point is NULL -- "
               "NOT IMPLEMENTED in this ICD\n");
        return 1;
    }
    if (!CmdBeginRendering || !CmdDraw || !CreateGraphicsPipelines ||
        !GetImageSubresourceLayout || !MapMemory || !UnmapMemory) {
        printf("FAILED: essential proc addr is NULL\n");
        return 1;
    }

    VkQueue queue;
    GetDeviceQueue(device, 0, 0, &queue);

    const uint32_t W = 64, H = 64;
    VkFormat FORMAT = VK_FORMAT_R8G8B8A8_UNORM;

    /* ---------------- one buffer, 256-byte slots ---------------- */
    VkBuffer dbuf; VkDeviceMemory dmem;
    {
        VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = SLOT * NSLOTS,
            .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        CHECK(CreateBuffer(device, &bci, NULL, &dbuf), "vkCreateBuffer");
        VkMemoryRequirements mreq;
        GetBufferMemoryRequirements(device, dbuf, &mreq);
        uint32_t mt = find_memory_type(&mem_props, mreq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt == UINT32_MAX) { printf("FAILED: no host-visible memory\n"); return 1; }
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mreq.size, .memoryTypeIndex = mt,
        };
        CHECK(AllocateMemory(device, &mai, NULL, &dmem), "vkAllocateMemory");
        CHECK(BindBufferMemory(device, dbuf, dmem, 0), "vkBindBufferMemory");
        uint8_t *p = NULL;
        CHECK(MapMemory(device, dmem, 0, VK_WHOLE_SIZE, 0, (void **)&p), "vkMapMemory");
        memset(p, 0, SLOT * NSLOTS);
        for (uint32_t sl = 0; sl < NSLOTS; sl++) {
            if (!cfg->slot_val[sl]) continue;
            float *f = (float *)(p + sl * SLOT);
            for (int c = 0; c < 4; c++) f[c] = K(cfg->slot_val[sl]);
            printf("  slot %2u = %u/255\n", sl, cfg->slot_val[sl]);
        }
        UnmapMemory(device, dmem);
    }

    /* ---------------- set layout: one set, nbind bindings ---------------- */
    VkDescriptorSetLayoutBinding binds[NB];
    for (uint32_t b = 0; b < cfg->nbind; b++)
        binds[b] = (VkDescriptorSetLayoutBinding){
            .binding = b, .descriptorType = cfg->type[b],
            .descriptorCount = b == 0 ? cfg->array_count : 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = cfg->nbind, .pBindings = binds,
    };
    VkDescriptorSetLayout set_layout;
    CHECK(CreateDescriptorSetLayout(device, &dslci, NULL, &set_layout),
          "vkCreateDescriptorSetLayout");
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout,
    };
    VkPipelineLayout pipeline_layout;
    CHECK(CreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout),
          "vkCreatePipelineLayout");

    VkDescriptorPoolSize psz[4] = {
        { UBO, 8 }, { SSBO, 8 }, { UBOD, 8 }, { SSBD, 8 } };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 4, .pPoolSizes = psz,
    };
    VkDescriptorPool desc_pool;
    CHECK(CreateDescriptorPool(device, &dpci, NULL, &desc_pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = desc_pool, .descriptorSetCount = 1,
        .pSetLayouts = &set_layout,
    };
    VkDescriptorSet dset;
    CHECK(AllocateDescriptorSets(device, &dsai, &dset), "vkAllocateDescriptorSets");

    for (uint32_t b = 0; b < cfg->nbind; b++) {
        const uint32_t n = b == 0 ? cfg->array_count : 1;
        VkDescriptorBufferInfo dbi[4];
        for (uint32_t e = 0; e < n; e++)
            dbi[e] = (VkDescriptorBufferInfo){
                .buffer = dbuf, .offset = (cfg->base_slot[b] + e) * SLOT, .range = 16 };
        VkWriteDescriptorSet wds = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = dset, .dstBinding = b, .dstArrayElement = 0,
            .descriptorCount = n, .descriptorType = cfg->type[b],
            .pBufferInfo = dbi,
        };
        UpdateDescriptorSets(device, 1, &wds, 0, NULL);
    }
    printf("descriptor set written: %u bindings\n", cfg->nbind);
    fflush(stdout);

    /* ---------------- colour attachment: LINEAR + HOST_VISIBLE ---------------- */
    VkImageCreateInfo img_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = FORMAT,
        .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image;
    CHECK(CreateImage(device, &img_info, NULL, &image), "vkCreateImage");
    VkMemoryRequirements img_mreq;
    GetImageMemoryRequirements(device, image, &img_mreq);
    uint32_t img_mt = find_memory_type(&mem_props, img_mreq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (img_mt == UINT32_MAX) { printf("FAILED: no host-visible memory for image\n"); return 1; }
    VkMemoryAllocateInfo img_alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = img_mreq.size, .memoryTypeIndex = img_mt,
    };
    VkDeviceMemory img_mem;
    CHECK(AllocateMemory(device, &img_alloc, NULL, &img_mem), "vkAllocateMemory(image)");
    CHECK(BindImageMemory(device, image, img_mem, 0), "vkBindImageMemory");

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = FORMAT,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkImageView view;
    CHECK(CreateImageView(device, &view_info, NULL, &view), "vkCreateImageView");

    /* ---------------- shaders ---------------- */
    size_t vs_size = 0, fs_size = 0;
    char *vs_code = read_file("triangle.vert.spv", &vs_size);
    if (!vs_code) return 1;
    char *fs_code = read_file(cfg->fs_spv, &fs_size);
    if (!fs_code) return 1;
    printf("shaders: triangle.vert.spv (%zu B), %s (%zu B)\n",
           vs_size, cfg->fs_spv, fs_size);

    VkShaderModuleCreateInfo vs_mci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vs_size, .pCode = (uint32_t*)vs_code,
    };
    VkShaderModule vs_mod;
    CHECK(CreateShaderModule(device, &vs_mci, NULL, &vs_mod), "vkCreateShaderModule(vert)");
    VkShaderModuleCreateInfo fs_mci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fs_size, .pCode = (uint32_t*)fs_code,
    };
    VkShaderModule fs_mod;
    CHECK(CreateShaderModule(device, &fs_mci, NULL, &fs_mod), "vkCreateShaderModule(frag)");

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs_mod, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs_mod, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cb_att = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo cb_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cb_att };
    VkDynamicState dyn_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn_states };
    VkPipelineRenderingCreateInfo rendering_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &FORMAT };
    VkGraphicsPipelineCreateInfo pipe_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering_info,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi_info, .pInputAssemblyState = &ia_info,
        .pViewportState = &vp_info, .pRasterizationState = &rs_info,
        .pMultisampleState = &ms_info, .pColorBlendState = &cb_info,
        .pDynamicState = &dyn_info, .layout = pipeline_layout,
    };
    VkPipeline pipeline;
    CHECK(CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipe_info, NULL, &pipeline),
          "vkCreateGraphicsPipelines");

    /* ---------------- record ---------------- */
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
    VkCommandPool pool;
    CHECK(CreateCommandPool(device, &pool_info, NULL, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cb_alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1 };
    VkCommandBuffer cmdbuf;
    CHECK(AllocateCommandBuffers(device, &cb_alloc, &cmdbuf), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(BeginCommandBuffer(cmdbuf, &begin_info), "vkBeginCommandBuffer");

    VkImageMemoryBarrier to_attachment = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    CmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       0, 0, NULL, 0, NULL, 1, &to_attachment);

    VkRenderingAttachmentInfo color_att = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .color = { .float32 = {0.0f, 0.0f, 0.0f, 1.0f} } },
    };
    VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { {0,0}, {W,H} }, .layerCount = 1,
        .colorAttachmentCount = 1, .pColorAttachments = &color_att,
    };
    CmdBeginRendering(cmdbuf, &rendering);

    VkViewport viewport = { 0, 0, (float)W, (float)H, 0.0f, 1.0f };
    VkRect2D scissor = { {0,0}, {W,H} };
    CmdSetViewport(cmdbuf, 0, 1, &viewport);
    CmdSetScissor(cmdbuf, 0, 1, &scissor);
    CmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    /* Dynamic offsets are consumed in binding order, one per dynamic
     * descriptor. Only dynamic bindings contribute. */
    uint32_t dyn_off[NB], ndyn = 0;
    for (uint32_t b = 0; b < cfg->nbind; b++)
        if (is_dyn(cfg->type[b])) dyn_off[ndyn++] = cfg->dyn_slots[b] * SLOT;
    CmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline_layout, 0, 1, &dset, ndyn, dyn_off);
    printf("bound set 0 with %u dynamic offsets:", ndyn);
    for (uint32_t i = 0; i < ndyn; i++) printf(" %u", dyn_off[i]);
    printf("\n");
    fflush(stdout);

    CmdDraw(cmdbuf, 3, 1, 0, 0);
    CmdEndRendering(cmdbuf);
    CHECK(EndCommandBuffer(cmdbuf), "vkEndCommandBuffer");

    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmdbuf };
    CHECK(QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    CHECK(QueueWaitIdle(queue), "vkQueueWaitIdle");

    /* ---------------- readback ---------------- */
    VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout sl;
    GetImageSubresourceLayout(device, image, &sub, &sl);
    printf("rowPitch=%llu offset=%llu\n",
           (unsigned long long)sl.rowPitch, (unsigned long long)sl.offset);

    void *mapped = NULL;
    CHECK(MapMemory(device, img_mem, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory(image)");
    uint8_t *base = (uint8_t*)mapped + sl.offset;

    uint32_t non_black = 0;
    for (uint32_t y = 0; y < H; y++) {
        uint8_t *row = base + y * sl.rowPitch;
        for (uint32_t x = 0; x < W; x++) {
            uint8_t *px = row + x * 4;
            if (px[0] || px[1] || px[2]) non_black++;
        }
    }
    uint8_t *centre = base + 32 * sl.rowPitch + 32 * 4;
    uint8_t *corner = base + 0 * sl.rowPitch + 0 * 4;

    printf("\n--- result: %s ---\n", cfg->name);
    printf("non-black pixels : %u / %u\n", non_black, W*H);
    printf("centre (32,32)   = %u,%u,%u,%u\n", centre[0], centre[1], centre[2], centre[3]);
    printf("expected centre  = %u,%u,%u,%u\n",
           cfg->expect[0], cfg->expect[1], cfg->expect[2], cfg->expect[3]);
    int ok = non_black == 512;
    if (!ok) printf("MISMATCH: coverage %u, expected the 512-pixel triangle\n", non_black);
    static const char *chn = "RGBA";
    for (int c = 0; c < 4; c++) {
        if (centre[c] == cfg->expect[c]) continue;
        ok = 0;
        const char *why = "unexplained value";
        if (centre[c] == 20)  why = "DECOY: dynamic offset not applied";
        if (centre[c] == 230 || centre[c] == 240) why = "TRAP: dynamic offset applied to the wrong binding";
        if (centre[c] == 0)   why = "zero: descriptor read returned nothing";
        printf("MISMATCH: channel %c got %u expected %u -> %s\n",
               chn[c], centre[c], cfg->expect[c], why);
    }
    printf("DESCFP %s px=%u centre=%u,%u,%u,%u verdict=%s\n", cfg->name, non_black,
           centre[0], centre[1], centre[2], centre[3], ok ? "PASS" : "FAIL");
    /* Visual dump: RESTAB_PPM=<path> writes the framebuffer as binary PPM so the
     * result can be looked at as an image rather than only counted. */
    const char *ppm_path = getenv("DESC_PPM");
    if (ppm_path && ppm_path[0]) {
        FILE *pf = fopen(ppm_path, "wb");
        if (!pf) {
            printf("WARNING: cannot write %s\n", ppm_path);
        } else {
            fprintf(pf, "P6\n%u %u\n255\n", W, H);
            for (uint32_t y = 0; y < H; y++) {
                uint8_t *row = base + y * sl.rowPitch;
                for (uint32_t x = 0; x < W; x++) {
                    uint8_t *px = row + x * 4;
                    uint8_t rgb[3] = { px[0], px[1], px[2] };
                    fwrite(rgb, 1, 3, pf);
                }
            }
            fclose(pf);
            printf("VISUAL_DUMP: %s\n", ppm_path);
        }
    }

    UnmapMemory(device, img_mem);

    if (corner[0] || corner[1] || corner[2]) {
        printf("\nWARNING: corner is not clear, geometry may be wrong\n");
    }
    printf("\n%s\n", ok ? "SUCCESS: descriptor sets read correctly"
                        : "FAILED: centre pixel does not match");
    free(vs_code); free(fs_code);
    return ok ? 0 : 1;
}
