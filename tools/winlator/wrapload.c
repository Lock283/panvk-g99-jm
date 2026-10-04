/* wrapload.c -- load Winlator's libvulkan_wrapper.so the way the Vulkan
 * loader would (ICD entry points), with the same ADRENOTOOLS_* env the
 * launcher sets, so the wrapper loads our driver through adrenotools.
 * Prints device, version, extension count, and whether the extensions and
 * features a DXVK device needs are present; then creates a device with them
 * (like DxvkAdapter::createDevice) and runs one queue submit + wait idle.
 *
 * usage: wrapload <libvulkan_wrapper.so> [dxvk-profile: 1103|2x]
 * env:   WRAPLOAD_LIST=1 to print all device extensions.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

typedef VkResult (*PFN_negotiate)(uint32_t *);

static VkExtensionProperties *g_ext;
static uint32_t g_next;

static int has_ext(const char *n)
{
   for (uint32_t i = 0; i < g_next; i++)
      if (!strcmp(g_ext[i].extensionName, n))
         return 1;
   return 0;
}

int main(int argc, char **argv)
{
   if (argc < 2) { fprintf(stderr, "usage: %s libvulkan_wrapper.so [1103|2x]\n", argv[0]); return 2; }
   const char *profile = argc > 2 ? argv[2] : "1103";
   void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("RESULT dlopen-failed %s\n", dlerror()); return 1; }
   PFN_negotiate neg = (PFN_negotiate)dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa) { printf("RESULT no-icd-entry\n"); return 1; }
   if (neg) { uint32_t v = 5; VkResult r = neg(&v); printf("NEGOTIATE r=%d version=%u\n", r, v); }

   PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "wrapload", 1, "DXVK", 1,
                             VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app };
   VkInstance inst;
   VkResult r = ci(&ici, NULL, &inst);
   if (r) { printf("RESULT create-instance-failed %d\n", r); return 1; }

#define GI(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
   GI(vkEnumeratePhysicalDevices); GI(vkGetPhysicalDeviceProperties);
   GI(vkEnumerateDeviceExtensionProperties); GI(vkGetPhysicalDeviceFeatures2);
   GI(vkGetPhysicalDeviceQueueFamilyProperties); GI(vkCreateDevice); GI(vkDestroyInstance);
   GI(vkGetDeviceProcAddr);

   uint32_t n = 0;
   vkEnumeratePhysicalDevices(inst, &n, NULL);
   printf("DEVICES %u\n", n);
   if (!n) { printf("RESULT no-device\n"); return 1; }
   VkPhysicalDevice pd;
   n = 1;
   vkEnumeratePhysicalDevices(inst, &n, &pd);
   VkPhysicalDeviceProperties p;
   vkGetPhysicalDeviceProperties(pd, &p);
   printf("RENDERER %s vendor=0x%x device=0x%x\n", p.deviceName, p.vendorID, p.deviceID);
   printf("VERSION %d.%d.%d driver=0x%x\n", VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
          VK_VERSION_PATCH(p.apiVersion), p.driverVersion);

   vkEnumerateDeviceExtensionProperties(pd, NULL, &g_next, NULL);
   g_ext = calloc(g_next, sizeof(*g_ext));
   vkEnumerateDeviceExtensionProperties(pd, NULL, &g_next, g_ext);
   printf("EXTENSIONS %u\n", g_next);
   if (getenv("WRAPLOAD_LIST"))
      for (uint32_t i = 0; i < g_next; i++) printf("  %s\n", g_ext[i].extensionName);

   /* Device extensions DXVK marks Required (dxvk_extensions.h). */
   const char *req_1103[] = { "VK_KHR_swapchain", "VK_KHR_create_renderpass2", "VK_KHR_depth_stencil_resolve",
                              "VK_KHR_draw_indirect_count", "VK_KHR_driver_properties",
                              "VK_KHR_image_format_list", "VK_KHR_sampler_mirror_clamp_to_edge",
                              "VK_KHR_shader_draw_parameters", NULL };
   const char *req_2x[] = { "VK_KHR_swapchain", "VK_EXT_robustness2", "VK_KHR_pipeline_library",
                            "VK_EXT_extended_dynamic_state", "VK_KHR_maintenance5", NULL };
   const char **req = !strcmp(profile, "2x") ? req_2x : req_1103;
   int missing = 0;
   const char *en[32]; uint32_t nen = 0;
   for (int i = 0; req[i]; i++) {
      int h = has_ext(req[i]);
      printf("REQ_EXT %-40s %s\n", req[i], h ? "yes" : "MISSING");
      if (!h) missing++; else en[nen++] = req[i];
   }

   /* Core features DXVK's D3D11 FL11_0 path needs (d3d11_device.cpp GetDeviceFeatures). */
   VkPhysicalDeviceRobustness2FeaturesEXT rb2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
   VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &rb2 };
   vkGetPhysicalDeviceFeatures2(pd, &f2);
   VkPhysicalDeviceFeatures *f = &f2.features;
#define FEAT(x) printf("FEAT %-36s %s\n", #x, f->x ? "yes" : "no")
   FEAT(robustBufferAccess); FEAT(fullDrawIndexUint32); FEAT(imageCubeArray); FEAT(independentBlend);
   FEAT(geometryShader); FEAT(tessellationShader); FEAT(sampleRateShading); FEAT(dualSrcBlend);
   FEAT(logicOp); FEAT(multiDrawIndirect); FEAT(drawIndirectFirstInstance); FEAT(depthClamp);
   FEAT(depthBiasClamp); FEAT(fillModeNonSolid); FEAT(depthBounds); FEAT(multiViewport);
   FEAT(samplerAnisotropy); FEAT(textureCompressionBC); FEAT(occlusionQueryPrecise);
   FEAT(pipelineStatisticsQuery); FEAT(vertexPipelineStoresAndAtomics); FEAT(fragmentStoresAndAtomics);
   FEAT(shaderImageGatherExtended); FEAT(shaderStorageImageExtendedFormats);
   FEAT(shaderStorageImageWriteWithoutFormat); FEAT(shaderClipDistance); FEAT(shaderCullDistance);
   FEAT(shaderFloat64); FEAT(shaderInt64); FEAT(variableMultisampleRate); FEAT(shaderResourceMinLod);
   printf("FEAT %-36s %s\n", "robustBufferAccess2", rb2.robustBufferAccess2 ? "yes" : "no");
   printf("FEAT %-36s %s\n", "nullDescriptor", rb2.nullDescriptor ? "yes" : "no");

   /* DXVK always requires robustBufferAccess; 2.x additionally nullDescriptor. */
   VkPhysicalDeviceFeatures want = { 0 };
   want.robustBufferAccess = f->robustBufferAccess;
   want.fullDrawIndexUint32 = f->fullDrawIndexUint32;
   want.imageCubeArray = f->imageCubeArray;
   want.independentBlend = f->independentBlend;
   want.depthClamp = f->depthClamp;
   want.depthBiasClamp = f->depthBiasClamp;
   want.fillModeNonSolid = f->fillModeNonSolid;
   want.samplerAnisotropy = f->samplerAnisotropy;
   want.shaderImageGatherExtended = f->shaderImageGatherExtended;
   want.multiDrawIndirect = f->multiDrawIndirect;
   want.drawIndirectFirstInstance = f->drawIndirectFirstInstance;
   want.textureCompressionBC = f->textureCompressionBC;
   if (!f->robustBufferAccess) missing++;

   uint32_t nq = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
   VkQueueFamilyProperties qf[8];
   if (nq > 8) nq = 8;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
   printf("QUEUE_FAMILIES %u (fam0 flags=0x%x count=%u)\n", nq, qf[0].queueFlags, qf[0].queueCount);

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
   VkPhysicalDeviceRobustness2FeaturesEXT rb2w = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
   rb2w.nullDescriptor = rb2.nullDescriptor;
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, has_ext("VK_EXT_robustness2") ? &rb2w : NULL,
                              0, 1, &qci, 0, NULL, nen, en, &want };
   if (rb2w.nullDescriptor && !has_ext("VK_EXT_robustness2")) dci.pNext = NULL;
   if (dci.pNext) {
      int listed = 0;
      for (uint32_t i = 0; i < nen; i++) if (!strcmp(en[i], "VK_EXT_robustness2")) listed = 1;
      if (!listed) en[nen++] = "VK_EXT_robustness2", dci.enabledExtensionCount = nen;
   }
   VkDevice dev;
   r = vkCreateDevice(pd, &dci, NULL, &dev);
   printf("CREATE_DEVICE r=%d (exts=%u)\n", r, nen);
   if (r) { printf("RESULT create-device-failed\n"); return 1; }
   PFN_vkGetDeviceQueue gdq = (PFN_vkGetDeviceQueue)vkGetDeviceProcAddr(dev, "vkGetDeviceQueue");
   PFN_vkQueueSubmit qs = (PFN_vkQueueSubmit)vkGetDeviceProcAddr(dev, "vkQueueSubmit");
   PFN_vkQueueWaitIdle qwi = (PFN_vkQueueWaitIdle)vkGetDeviceProcAddr(dev, "vkQueueWaitIdle");
   PFN_vkDestroyDevice dd = (PFN_vkDestroyDevice)vkGetDeviceProcAddr(dev, "vkDestroyDevice");
   VkQueue q;
   gdq(dev, 0, 0, &q);
   r = qs(q, 0, NULL, VK_NULL_HANDLE);
   VkResult r2 = qwi(q);
   printf("SUBMIT r=%d WAITIDLE r=%d\n", r, r2);
   dd(dev, NULL);
   vkDestroyInstance(inst, NULL);
   printf("MISSING_REQUIRED %d\n", missing);
   printf("RESULT %s\n", (r || r2) ? "submit-failed" : (missing ? "device-ok-but-missing-required" : "ok"));
   return (r || r2 || missing) ? 1 : 0;
}
