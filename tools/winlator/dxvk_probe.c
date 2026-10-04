/* dxvk_probe.c -- would the DXVK builds bundled with Winlator create a D3D11
 * / D3D9 device on this Vulkan implementation? Loads Winlator's
 * libvulkan_wrapper.so (ICD entry), reads what the wrapper exposes on top of
 * our driver, and applies the feature/extension rules from the DXVK sources:
 *
 *   dxvk 1.10.3        doitsujin/dxvk v1.10.3  d3d11_device.cpp GetDeviceFeatures,
 *                      d3d9_device.cpp GetDeviceFeatures, dxvk_extensions.h
 *   sarek 1.11.0       pythonlover02/DXVK-Sarek v1.11.0 (Winlator "1.11.1-sarek",
 *                      its d3d11.dll reports v1.11.0)
 *   sarek 1.12.0       DXVK-Sarek v1.12.0 (Winlator "1.12.1-sarek", reports v1.12.0)
 *   dxvk 2.3.1         doitsujin/dxvk v2.3.1
 *
 * DXVK enables "required" features with VK_TRUE and then checks them with
 * DxvkAdapter::checkFeatureSupport; a missing one makes D3D11CreateDevice fail
 * for that feature level. Features copied from "supported" are optional.
 * For the profiles that can work, it then creates a device with every
 * optional DXVK extension that is present plus the supported features, the way
 * DxvkAdapter::createDevice does, and runs an empty submit.
 *
 * usage: dxvk_probe <libvulkan_wrapper.so>
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

typedef VkResult (*PFN_negotiate)(uint32_t *);
static VkExtensionProperties *ext; static uint32_t next_;
static int has(const char *n) { for (uint32_t i = 0; i < next_; i++) if (!strcmp(ext[i].extensionName, n)) return 1; return 0; }

static VkPhysicalDeviceFeatures2 f2;
static VkPhysicalDeviceVulkan11Features f11;
static VkPhysicalDeviceVulkan12Features f12;
static VkPhysicalDeviceVulkan13Features f13;
static VkPhysicalDeviceTransformFeedbackFeaturesEXT ftf;
static VkPhysicalDeviceRobustness2FeaturesEXT frb2;

struct req { const char *name; int *val; int fl; }; /* fl: 0x9100 etc, 0 = always */
#define C(x) &f2.features.x

static int feat_val(const char *n)
{
#define F(x) if (!strcmp(n, #x)) return f2.features.x;
   F(geometryShader) F(robustBufferAccess) F(shaderStorageImageWriteWithoutFormat) F(depthClamp)
   F(depthBiasClamp) F(fillModeNonSolid) F(sampleRateShading) F(shaderClipDistance) F(shaderCullDistance)
   F(textureCompressionBC) F(occlusionQueryPrecise) F(independentBlend) F(multiViewport)
   F(fullDrawIndexUint32) F(shaderImageGatherExtended) F(dualSrcBlend) F(imageCubeArray)
   F(drawIndirectFirstInstance) F(fragmentStoresAndAtomics) F(multiDrawIndirect) F(tessellationShader)
   F(logicOp) F(variableMultisampleRate) F(vertexPipelineStoresAndAtomics)
#undef F
   if (!strcmp(n, "shaderDrawParameters")) return f11.shaderDrawParameters;
   if (!strcmp(n, "hostQueryReset")) return f12.hostQueryReset;
   if (!strcmp(n, "samplerMirrorClampToEdge")) return f12.samplerMirrorClampToEdge;
   if (!strcmp(n, "shaderDemoteToHelperInvocation")) return f13.shaderDemoteToHelperInvocation;
   if (!strcmp(n, "transformFeedback")) return ftf.transformFeedback;
   if (!strcmp(n, "geometryStreams")) return ftf.geometryStreams;
   if (!strcmp(n, "nullDescriptor")) return frb2.nullDescriptor;
   fprintf(stderr, "unknown feature %s\n", n); exit(3);
}

/* Required (VK_TRUE) features per D3D11 feature level, from the sources. */
struct rule { const char *f; unsigned fl; };
static const struct rule d3d11_1103[] = {
   {"geometryShader",0},{"robustBufferAccess",0},{"shaderStorageImageWriteWithoutFormat",0},{"shaderDrawParameters",0},
   {"depthClamp",0x9100},{"depthBiasClamp",0x9100},{"fillModeNonSolid",0x9100},{"sampleRateShading",0x9100},
   {"shaderClipDistance",0x9100},{"shaderCullDistance",0x9100},{"textureCompressionBC",0x9100},{"hostQueryReset",0x9100},
   {"occlusionQueryPrecise",0x9200},{"independentBlend",0x9300},{"multiViewport",0x9300},
   {"fullDrawIndexUint32",0xa000},{"shaderImageGatherExtended",0xa000},{"transformFeedback",0xa000},{"geometryStreams",0xa000},
   {"dualSrcBlend",0xa100},{"imageCubeArray",0xa100},
   {"drawIndirectFirstInstance",0xb000},{"fragmentStoresAndAtomics",0xb000},{"multiDrawIndirect",0xb000},{"tessellationShader",0xb000},
   {"logicOp",0xb100},{"variableMultisampleRate",0xb100},{"vertexPipelineStoresAndAtomics",0xb100},{0,0}};
/* sarek v1.11.0 (exact VK_TRUE list of its GetDeviceFeatures). */
static const struct rule d3d11_s1110[] = {
   {"geometryShader",0},{"robustBufferAccess",0},{"shaderStorageImageWriteWithoutFormat",0},{"shaderDrawParameters",0},
   {"sampleRateShading",0x9100},{"hostQueryReset",0x9100},{"occlusionQueryPrecise",0x9200},{"independentBlend",0x9300},
   {"fullDrawIndexUint32",0xa000},{"shaderImageGatherExtended",0xa000},{"imageCubeArray",0xa100},
   {"drawIndirectFirstInstance",0xb000},{"fragmentStoresAndAtomics",0xb000},{"multiDrawIndirect",0xb000},
   {"tessellationShader",0xb000},{"variableMultisampleRate",0xb100},{"vertexPipelineStoresAndAtomics",0xb100},{0,0}};
/* sarek v1.12.0: nothing is VK_TRUE in GetDeviceFeatures. */
static const struct rule d3d11_s1120[] = {{0,0}};
/* dxvk v2.3.1: all unconditional. */
static const struct rule d3d11_231[] = {
   {"depthBiasClamp",0},{"depthClamp",0},{"dualSrcBlend",0},{"fillModeNonSolid",0},{"fullDrawIndexUint32",0},
   {"geometryShader",0},{"imageCubeArray",0},{"independentBlend",0},{"multiViewport",0},{"occlusionQueryPrecise",0},
   {"sampleRateShading",0},{"shaderClipDistance",0},{"shaderCullDistance",0},{"shaderImageGatherExtended",0},
   {"textureCompressionBC",0},{"shaderDrawParameters",0},{"samplerMirrorClampToEdge",0},
   {"shaderDemoteToHelperInvocation",0},{"transformFeedback",0},{"geometryStreams",0},{0,0}};
/* D3D9 GetDeviceFeatures VK_TRUE lists (no feature levels, rule fl=0). */
static const struct rule d3d9_1103[] = {
   {"geometryShader",0},{"robustBufferAccess",0},{"shaderStorageImageWriteWithoutFormat",0},{"imageCubeArray",0},
   {"depthClamp",0},{"depthBiasClamp",0},{"fillModeNonSolid",0},{"sampleRateShading",0},{"shaderClipDistance",0},
   {"shaderCullDistance",0},{"textureCompressionBC",0},{"hostQueryReset",0},{"occlusionQueryPrecise",0},
   {"multiViewport",0},{"independentBlend",0},{"fullDrawIndexUint32",0},{0,0}};
static const struct rule d3d9_s1110[] = {
   {"geometryShader",0},{"robustBufferAccess",0},{"shaderStorageImageWriteWithoutFormat",0},{"imageCubeArray",0},
   {"sampleRateShading",0},{"hostQueryReset",0},{"occlusionQueryPrecise",0},{"independentBlend",0},
   {"fullDrawIndexUint32",0},{0,0}};
static const struct rule d3d9_s1120[] = {{0,0}};

static const unsigned fls[] = {0xb100,0xb000,0xa100,0xa000,0x9300,0x9200,0x9100};

static unsigned max_fl(const struct rule *r, char *missing, size_t msz)
{
   missing[0] = 0;
   for (unsigned i = 0; i < sizeof(fls)/sizeof(fls[0]); i++) {
      int ok = 1;
      for (const struct rule *x = r; x->f; x++)
         if (x->fl <= fls[i] && !feat_val(x->f)) {
            ok = 0;
            if (i == 0 && strlen(missing) + strlen(x->f) + 2 < msz) { strcat(missing, x->f); strcat(missing, " "); }
         }
      if (ok) return fls[i];
   }
   return 0;
}

int main(int argc, char **argv)
{
   if (argc < 2) { fprintf(stderr, "usage: %s libvulkan_wrapper.so\n", argv[0]); return 2; }
   void *lib = dlopen(argv[1], RTLD_NOW);
   if (!lib) { printf("RESULT dlopen %s\n", dlerror()); return 1; }
   PFN_negotiate neg = (PFN_negotiate)dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa) gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
   if (neg) { uint32_t v = 5; neg(&v); }
   PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "dxvk_probe", 1, "DXVK", 1, VK_API_VERSION_1_3 };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app };
   VkInstance inst;
   if (ci(&ici, NULL, &inst)) { printf("RESULT create-instance-failed\n"); return 1; }
#define GI(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
   GI(vkEnumeratePhysicalDevices); GI(vkGetPhysicalDeviceProperties); GI(vkEnumerateDeviceExtensionProperties);
   GI(vkGetPhysicalDeviceFeatures2); GI(vkCreateDevice); GI(vkGetDeviceProcAddr); GI(vkDestroyInstance);
   uint32_t n = 1; VkPhysicalDevice pd;
   if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("RESULT no-device\n"); return 1; }
   VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p);
   printf("DEVICE %s api %u.%u.%u\n", p.deviceName, VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
   vkEnumerateDeviceExtensionProperties(pd, NULL, &next_, NULL);
   ext = calloc(next_, sizeof(*ext)); vkEnumerateDeviceExtensionProperties(pd, NULL, &next_, ext);

   f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
   f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
   f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
   f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
   ftf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;
   frb2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
   f2.pNext = &f11; f11.pNext = &f12; f12.pNext = &f13; f13.pNext = &frb2;
   if (has("VK_EXT_transform_feedback")) frb2.pNext = &ftf;
   vkGetPhysicalDeviceFeatures2(pd, &f2);

   struct { const char *name; const struct rule *d11, *d9; const char *req_ext[4]; } prof[] = {
      {"dxvk-1.10.3",       d3d11_1103,  d3d9_1103,  {"VK_KHR_swapchain","VK_KHR_image_format_list",0}},
      {"dxvk-1.11.1-sarek", d3d11_s1110, d3d9_s1110,  {"VK_KHR_swapchain",0}},
      {"dxvk-1.12.1-sarek", d3d11_s1120, d3d9_s1120, {"VK_KHR_swapchain",0}},
      {"dxvk-v2.3.1",       d3d11_231,   NULL,       {"VK_KHR_swapchain","VK_EXT_robustness2",0}},
   };
   int usable = 0;
   for (unsigned i = 0; i < 4; i++) {
      char miss[512], miss9[512] = "";
      unsigned fl = max_fl(prof[i].d11, miss, sizeof(miss));
      unsigned fl9 = prof[i].d9 ? max_fl(prof[i].d9, miss9, sizeof(miss9)) : 0;
      int extok = 1; char mext[256] = "";
      for (int k = 0; prof[i].req_ext[k]; k++) if (!has(prof[i].req_ext[k])) { extok = 0; strcat(mext, prof[i].req_ext[k]); strcat(mext, " "); }
      if (!strcmp(prof[i].name, "dxvk-v2.3.1") && !frb2.nullDescriptor) { extok = 0; strcat(mext, "nullDescriptor "); }
      int d11ok = fl && extok;
      printf("PROFILE %-18s D3D11 %s", prof[i].name, d11ok ? "OK" : "FAIL");
      if (d11ok) printf(" maxFL=%x_%x", fl >> 12, (fl >> 8) & 0xf);
      if (!d11ok) printf(" missing: %s%s", mext, miss);
      if (prof[i].d9) printf(" | D3D9 %s%s%s", (fl9 && extok) ? "OK" : "FAIL", (fl9 && extok) ? "" : " missing: ", (fl9 && extok) ? "" : miss9);
      printf("\n");
      usable += d11ok;
   }
   /* Degraded features for sarek 1.12 (copied from supported = lost if 0). */
   const char *deg[] = {"geometryShader","tessellationShader","transformFeedback","shaderClipDistance","shaderCullDistance",
                        "textureCompressionBC","multiViewport","depthBounds",0};
   printf("SAREK112_DEGRADED");
   for (int k = 0; deg[k]; k++) {
      int v = !strcmp(deg[k], "depthBounds") ? f2.features.depthBounds : feat_val(deg[k]);
      if (!v) printf(" %s", deg[k]);
   }
   printf("\n");

   /* Device creation like DxvkAdapter::createDevice for sarek 1.12: every
    * optional extension present, features = supported subset. */
   const char *opt[] = {"VK_EXT_4444_formats","VK_EXT_custom_border_color","VK_EXT_depth_clip_enable",
      "VK_EXT_extended_dynamic_state","VK_EXT_host_query_reset","VK_EXT_memory_budget","VK_EXT_non_seamless_cube_map",
      "VK_EXT_robustness2","VK_EXT_shader_demote_to_helper_invocation","VK_EXT_shader_stencil_export",
      "VK_EXT_vertex_attribute_divisor","VK_KHR_create_renderpass2","VK_KHR_depth_stencil_resolve",
      "VK_KHR_draw_indirect_count","VK_KHR_driver_properties","VK_KHR_image_format_list",
      "VK_KHR_sampler_mirror_clamp_to_edge","VK_KHR_shader_float_controls","VK_KHR_swapchain",
      "VK_KHR_timeline_semaphore","VK_EXT_transform_feedback","VK_EXT_shader_viewport_index_layer",
      "VK_EXT_memory_priority","VK_EXT_conservative_rasterization",0};
   const char *en[32]; uint32_t nen = 0;
   printf("OPT_EXT");
   for (int k = 0; opt[k]; k++) { int h = has(opt[k]); printf(" %s%s", h ? "+" : "-", opt[k] + 3); if (h) en[nen++] = opt[k]; }
   printf("\n");
   VkPhysicalDeviceFeatures2 want = f2; /* supported subset == everything supported */
   want.pNext = &f11; f11.pNext = &f12; f12.pNext = &f13; f13.pNext = has("VK_EXT_robustness2") ? (void *)&frb2 : NULL;
   frb2.pNext = NULL;
   float prio = 1.0f;
   VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &want, 0, 1, &q, 0, NULL, nen, en, NULL };
   VkDevice dev;
   VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
   printf("CREATE_DEVICE sarek-like r=%d exts=%u\n", r, nen);
   if (r == VK_SUCCESS) {
      PFN_vkGetDeviceQueue gdq = (PFN_vkGetDeviceQueue)vkGetDeviceProcAddr(dev, "vkGetDeviceQueue");
      PFN_vkQueueSubmit qs = (PFN_vkQueueSubmit)vkGetDeviceProcAddr(dev, "vkQueueSubmit");
      PFN_vkQueueWaitIdle qw = (PFN_vkQueueWaitIdle)vkGetDeviceProcAddr(dev, "vkQueueWaitIdle");
      PFN_vkDestroyDevice dd = (PFN_vkDestroyDevice)vkGetDeviceProcAddr(dev, "vkDestroyDevice");
      VkQueue qq; gdq(dev, 0, 0, &qq);
      VkResult s1 = qs(qq, 0, NULL, VK_NULL_HANDLE), s2 = qw(qq);
      printf("SUBMIT r=%d WAITIDLE r=%d\n", s1, s2);
      dd(dev, NULL);
      if (s1 || s2) r = VK_ERROR_DEVICE_LOST;
   }
   vkDestroyInstance(inst, NULL);
   printf("RESULT %s usable_profiles=%d\n", (r == VK_SUCCESS && usable) ? "ok" : "fail", usable);
   return (r == VK_SUCCESS && usable) ? 0 : 1;
}
