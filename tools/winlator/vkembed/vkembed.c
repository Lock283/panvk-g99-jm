// vkembed: native repro of the AIO-Graphics-Test Vulkan "embed" path
// (src/cube.c aio_vk_embed_*): Vulkan 1.0 instance, device with no extensions
// and no features, offscreen R8G8B8A8 + D32 render pass, the same two embed
// shaders, a 36-vertex colored cube from a vertex buffer, copy to a buffer.
//
// env: VKEMBED_FRAMES (60)  VKEMBED_W/H (640x400)
//      VKEMBED_THREADS=n    also create n extra copies of the pipeline in n
//                           threads at the same time (DXVK-style parallel compile)
//      VKEMBED_LIE=1        negative control: expect the clear color at the center
// Prints VKEMBED RESULT ok|FAIL.
#include <vulkan/vulkan.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "embed_vs.h"
#include "embed_fs.h"

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
   printf("VKEMBED %s failed: %d\nVKEMBED RESULT FAIL\n", #x, r_); exit(1); } } while (0)

static VkDevice dev;
static VkPhysicalDeviceMemoryProperties mp;
static VkGraphicsPipelineCreateInfo g_gp;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   printf("VKEMBED no memory type\nVKEMBED RESULT FAIL\n");
   exit(1);
}

static void mk_buffer(VkDeviceSize sz, VkBufferUsageFlags u, VkBuffer *b, VkDeviceMemory *m, void **p)
{
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz, .usage = u};
   CK(vkCreateBuffer(dev, &bi, NULL, b));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, *b, &mr);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
      .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
   CK(vkAllocateMemory(dev, &ai, NULL, m));
   CK(vkBindBufferMemory(dev, *b, *m, 0));
   CK(vkMapMemory(dev, *m, 0, sz, 0, p));
}

static void mk_image(VkFormat f, VkImageUsageFlags u, uint32_t w, uint32_t h, VkImage *img, VkImageView *v, VkImageAspectFlags asp)
{
   VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
      .format = f, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u};
   CK(vkCreateImage(dev, &ii, NULL, img));
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(dev, *img, &mr);
   VkDeviceMemory m;
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
      .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
   CK(vkAllocateMemory(dev, &ai, NULL, &m));
   CK(vkBindImageMemory(dev, *img, m, 0));
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f, .subresourceRange = {asp, 0, 1, 0, 1}};
   CK(vkCreateImageView(dev, &vi, NULL, v));
}

static void *compile_thread(void *arg)
{
   VkPipeline p;
   VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &g_gp, NULL, &p);
   *(VkResult *)arg = r;
   if (r == VK_SUCCESS) vkDestroyPipeline(dev, p, NULL);
   return NULL;
}

int main(void)
{
   int frames = getenv("VKEMBED_FRAMES") ? atoi(getenv("VKEMBED_FRAMES")) : 60;
   uint32_t w = getenv("VKEMBED_W") ? atoi(getenv("VKEMBED_W")) : 640;
   uint32_t h = getenv("VKEMBED_H") ? atoi(getenv("VKEMBED_H")) : 400;
   int nthr = getenv("VKEMBED_THREADS") ? atoi(getenv("VKEMBED_THREADS")) : 0;
   int lie = getenv("VKEMBED_LIE") != NULL;

   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vkembed", .apiVersion = VK_API_VERSION_1_0};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst;
   CK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1;
   VkPhysicalDevice gpu;
   VkResult er = vkEnumeratePhysicalDevices(inst, &n, &gpu);
   if (n == 0 || (er != VK_SUCCESS && er != VK_INCOMPLETE)) { printf("VKEMBED no GPU\nVKEMBED RESULT FAIL\n"); return 1; }
   VkPhysicalDeviceProperties pp;
   vkGetPhysicalDeviceProperties(gpu, &pp);
   vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
   printf("VKEMBED device %s driver 0x%x\n", pp.deviceName, pp.driverVersion);

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
   CK(vkCreateDevice(gpu, &dci, NULL, &dev));
   VkQueue q;
   vkGetDeviceQueue(dev, 0, 0, &q);

   VkImage color, depth;
   VkImageView cv, dv;
   mk_image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, w, h, &color, &cv, VK_IMAGE_ASPECT_COLOR_BIT);
   mk_image(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, w, h, &depth, &dv, VK_IMAGE_ASPECT_DEPTH_BIT);

   VkAttachmentDescription att[2] = {
      {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = 1, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
       .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
      {.format = VK_FORMAT_D32_SFLOAT, .samples = 1, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
   VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref, .pDepthStencilAttachment = &dref};
   VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2, .pAttachments = att, .subpassCount = 1, .pSubpasses = &sub};
   VkRenderPass rp;
   CK(vkCreateRenderPass(dev, &rpi, NULL, &rp));
   VkImageView views[2] = {cv, dv};
   VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 2, .pAttachments = views, .width = w, .height = h, .layers = 1};
   VkFramebuffer fb;
   CK(vkCreateFramebuffer(dev, &fbi, NULL, &fb));

   // 36-vertex cube, one color per face (position + color, like AioVkeVertex)
   static const float fc[6][3] = {{1,0,0},{0,1,0},{0,0,1},{1,1,0},{1,0,1},{0,1,1}};
   static const int fi[6][4] = {{0,1,3,2},{4,6,7,5},{0,4,5,1},{2,3,7,6},{0,2,6,4},{1,5,7,3}};
   float verts[36][6];
   for (int f = 0, k = 0; f < 6; f++) {
      static const int tri[6] = {0,1,2,0,2,3};
      for (int t = 0; t < 6; t++, k++) {
         int c = fi[f][tri[t]];
         verts[k][0] = (c & 4) ? 1 : -1; verts[k][1] = (c & 2) ? 1 : -1; verts[k][2] = (c & 1) ? 1 : -1;
         memcpy(&verts[k][3], fc[f], sizeof(fc[f]));
      }
   }
   VkBuffer vbo, ubo, rb;
   VkDeviceMemory vm, um, rm;
   void *vp, *up, *rp_;
   mk_buffer(sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vbo, &vm, &vp);
   memcpy(vp, verts, sizeof(verts));
   mk_buffer(64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &ubo, &um, &up);
   mk_buffer((VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb, &rm, &rp_);

   VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL};
   VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
   VkDescriptorSetLayout dsl;
   CK(vkCreateDescriptorSetLayout(dev, &li, NULL, &dsl));
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
   VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
   VkDescriptorPool dp;
   CK(vkCreateDescriptorPool(dev, &pi, NULL, &dp));
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   VkDescriptorSet ds;
   CK(vkAllocateDescriptorSets(dev, &dai, &ds));
   VkDescriptorBufferInfo dbi = {ubo, 0, 64};
   VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &dbi};
   vkUpdateDescriptorSets(dev, 1, &wr, 0, NULL);

   VkShaderModuleCreateInfo vsi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(embed_vs), .pCode = embed_vs};
   VkShaderModuleCreateInfo fsi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(embed_fs), .pCode = embed_fs};
   VkShaderModule vs, fs;
   CK(vkCreateShaderModule(dev, &vsi, NULL, &vs));
   CK(vkCreateShaderModule(dev, &fsi, NULL, &fs));
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
   VkPipelineLayout pl;
   CK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
   static VkPipelineShaderStageCreateInfo st[2];
   st[0] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"};
   st[1] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"};
   static VkVertexInputBindingDescription vib = {0, 24, VK_VERTEX_INPUT_RATE_VERTEX};
   static VkVertexInputAttributeDescription via[2] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}};
   static VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vib, .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = via};
   static VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   static VkViewport vpt;
   static VkRect2D sc;
   vpt = (VkViewport){0, 0, (float)w, (float)h, 0, 1};
   sc = (VkRect2D){{0, 0}, {w, h}};
   static VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vpt, .scissorCount = 1, .pScissors = &sc};
   static VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
   static VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   static VkPipelineDepthStencilStateCreateInfo dss = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS};
   static VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   static VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
   g_gp = (VkGraphicsPipelineCreateInfo){.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
      .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
      .pMultisampleState = &ms, .pDepthStencilState = &dss, .pColorBlendState = &cb, .layout = pl, .renderPass = rp};

   pthread_t thr[64];
   VkResult tres[64];
   if (nthr > 64) nthr = 64;
   for (int i = 0; i < nthr; i++) pthread_create(&thr[i], NULL, compile_thread, &tres[i]);
   VkPipeline pipe;
   CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &g_gp, NULL, &pipe));
   int tfail = 0;
   for (int i = 0; i < nthr; i++) { pthread_join(thr[i], NULL); tfail += tres[i] != VK_SUCCESS; }
   printf("VKEMBED pipeline ok (+%d parallel, %d failed)\n", nthr, tfail);

   VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   VkCommandPool cp;
   CK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
   VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VkCommandBuffer cmd;
   CK(vkAllocateCommandBuffers(dev, &cai, &cmd));
   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   CK(vkCreateFence(dev, &fci, NULL, &fence));

   const float clr[3] = {0.10f, 0.12f, 0.16f};
   int bad = 0, checked = 0;
   for (int fr = 0; fr < frames; fr++) {
      // MVP like aio_vk_embed_render (column-major), small rotation per frame
      float a = fr * 0.05f, f = 1.0f / tanf(0.3f), asp = (float)w / h, zn = 0.1f, zf = 100.0f;
      float cy = cosf(a), sy = sinf(a), cx = cosf(0.5f), sx = sinf(0.5f);
      float m[16] = {cy, 0, -sy, 0,  sy * sx, cx, cy * sx, 0,  sy * cx, -sx, cy * cx, 0,  0, 0, -6.5f, 1};
      float p[16] = {f / asp, 0, 0, 0,  0, -f, 0, 0,  0, 0, zf / (zn - zf), -1,  0, 0, zn * zf / (zn - zf), 0};
      float r[16];
      for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) {
         float s = 0; for (int k = 0; k < 4; k++) s += p[k * 4 + rr] * m[c * 4 + k]; r[c * 4 + rr] = s; }
      memcpy(up, r, 64);
      VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      CK(vkResetCommandBuffer(cmd, 0));
      CK(vkBeginCommandBuffer(cmd, &bi));
      VkClearValue cl[2] = {{.color = {{clr[0], clr[1], clr[2], 1}}}, {.depthStencil = {1, 0}}};
      VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = {{0, 0}, {w, h}}, .clearValueCount = 2, .pClearValues = cl};
      vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
      VkDeviceSize off = 0;
      vkCmdBindVertexBuffers(cmd, 0, 1, &vbo, &off);
      vkCmdDraw(cmd, 36, 1, 0, 0);
      vkCmdEndRenderPass(cmd);
      VkBufferImageCopy reg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
      vkCmdCopyImageToBuffer(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &reg);
      CK(vkEndCommandBuffer(cmd));
      VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
      CK(vkResetFences(dev, 1, &fence));
      CK(vkQueueSubmit(q, 1, &si, fence));
      CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
      if (fr % 10 == 0 || fr == frames - 1) {
         const uint8_t *px = rp_;
         const uint8_t *ctr = px + ((h / 2) * w + w / 2) * 4;
         const uint8_t *cor = px;
         int ctr_is_clear = abs(ctr[0] - (int)(clr[0] * 255 + .5f)) < 3 && abs(ctr[1] - (int)(clr[1] * 255 + .5f)) < 3 && abs(ctr[2] - (int)(clr[2] * 255 + .5f)) < 3;
         int cor_is_clear = abs(cor[0] - (int)(clr[0] * 255 + .5f)) < 3 && abs(cor[1] - (int)(clr[1] * 255 + .5f)) < 3 && abs(cor[2] - (int)(clr[2] * 255 + .5f)) < 3;
         int ok = cor_is_clear && (lie ? ctr_is_clear : !ctr_is_clear);
         checked++;
         if (!ok) { bad++; printf("VKEMBED frame %d center %d,%d,%d corner %d,%d,%d\n", fr, ctr[0], ctr[1], ctr[2], cor[0], cor[1], cor[2]); }
      }
   }
   vkDeviceWaitIdle(dev);
   printf("VKEMBED frames=%d checked=%d bad=%d\nVKEMBED RESULT %s\n", frames, checked, bad, bad || tfail ? "FAIL" : "ok");
   return bad || tfail;
}
