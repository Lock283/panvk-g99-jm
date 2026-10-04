/* adrenoload.c -- load a custom Vulkan driver the way the Winlator launcher
 * (libwinlator.so GPUInformation.*) does: libadrenotools
 * adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, tmp, hookDir,
 * driverDir, libraryName, NULL, NULL), then create an instance, enumerate
 * physical devices and device extensions, and print the same version string
 * the launcher builds ("%d.%d.%d" or "Unknown").
 *
 * usage: adrenoload <libadrenotools.so> <hookDir/> <driverDir/> <libraryName>
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <vulkan/vulkan.h>

#define ADRENOTOOLS_DRIVER_CUSTOM (1 << 0)

typedef void *(*PFN_open)(int, int, const char *, const char *, const char *,
                          const char *, const char *, void **);

int main(int argc, char **argv)
{
   if (argc < 5) {
      fprintf(stderr, "usage: %s libadrenotools.so hookDir/ driverDir/ libName\n", argv[0]);
      return 2;
   }
   /* ADRENOLOAD_PRELOAD=a.so,b.so: load these first, the way a zygote-forked
    * app process already has libEGL/libvulkan/libvndksupport loaded before
    * the app's own JNI libraries (libwinlator.so) are loaded. */
   const char *pre = getenv("ADRENOLOAD_PRELOAD");
   if (pre) {
      char buf[1024];
      snprintf(buf, sizeof(buf), "%s", pre);
      for (char *t = strtok(buf, ","); t; t = strtok(NULL, ","))
         if (!dlopen(t, RTLD_NOW)) printf("preload %s failed: %s\n", t, dlerror());
   }
   void *at = dlopen(argv[1], RTLD_NOW);
   if (!at) { printf("RESULT dlopen-adrenotools-failed %s\n", dlerror()); return 1; }
   PFN_open open_vk = (PFN_open)dlsym(at, "adrenotools_open_libvulkan");
   if (!open_vk) { printf("RESULT no-symbol\n"); return 1; }

   char tmp[4096];
   snprintf(tmp, sizeof(tmp), "%stemp", argv[3]);
   mkdir(tmp, 0770);

   /* ADRENOLOAD_SYSTEM=1: no custom driver (Winlator "System" driver path). */
   int flags = getenv("ADRENOLOAD_SYSTEM") ? 0 : ADRENOTOOLS_DRIVER_CUSTOM;
   void *vk = open_vk(RTLD_NOW, flags, tmp, argv[2], argv[3],
                      flags ? argv[4] : NULL, NULL, NULL);
   if (!vk) { printf("RESULT open_libvulkan-failed %s\n", dlerror()); return 1; }

   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");
   PFN_vkCreateInstance ci = (PFN_vkCreateInstance)dlsym(vk, "vkCreateInstance");
   if (!gipa || !ci) { printf("RESULT no-vk-entry\n"); return 1; }

   VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "Winlator", 1,
                             NULL, 0, VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app };
   VkInstance inst;
   VkResult r = ci(&ici, NULL, &inst);
   if (r != VK_SUCCESS) { printf("RESULT create-instance-failed %d\nVERSION Unknown\n", r); return 1; }

   PFN_vkEnumeratePhysicalDevices epd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceProperties gpdp = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
   PFN_vkEnumerateDeviceExtensionProperties edep = (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
   PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");

   uint32_t n = 0;
   r = epd(inst, &n, NULL);
   printf("ENUM result=%d devices=%u\n", r, n);
   if (n == 0) {
      printf("VERSION Unknown\nEXTENSIONS 0\nRESULT no-device\n");
      di(inst, NULL);
      return 1;
   }
   VkPhysicalDevice pd[8];
   if (n > 8) n = 8;
   epd(inst, &n, pd);
   VkPhysicalDeviceProperties p;
   gpdp(pd[0], &p);
   printf("RENDERER %s\n", p.deviceName);
   printf("VERSION %d.%d.%d\n", VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
          VK_VERSION_PATCH(p.apiVersion));
   uint32_t ne = 0;
   edep(pd[0], NULL, &ne, NULL);
   printf("EXTENSIONS %u\n", ne);
   if (getenv("ADRENOLOAD_LIST")) {
      VkExtensionProperties *e = calloc(ne, sizeof(*e));
      edep(pd[0], NULL, &ne, e);
      for (uint32_t i = 0; i < ne; i++)
         printf("  %s\n", e[i].extensionName);
      free(e);
   }
   if (getenv("ADRENOLOAD_MAPS")) {
      /* where did the driver's NEEDED libs come from? */
      FILE *m = fopen("/proc/self/maps", "r");
      char line[512], last[512] = "";
      while (m && fgets(line, sizeof(line), m)) {
         char *path = strchr(line, '/');
         if (!path || !strstr(path, ".so")) continue;
         if (!strstr(path, "libz.so") && !strstr(path, "libc++_shared") && !strstr(path, "libdrm") &&
             !strstr(path, "libvulkan_panfrost")) continue;
         if (!strcmp(path, last)) continue;
         snprintf(last, sizeof(last), "%s", path);
         printf("MAP %s", path);
      }
      if (m) fclose(m);
   }
   di(inst, NULL);
   printf("RESULT ok\n");
   return 0;
}
