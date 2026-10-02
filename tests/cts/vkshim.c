/* Minimal stand-in for libvulkan.so so deqp-vk can drive the PanVK ICD
 * directly, the same way every Phase 4 harness does: no loader, no layers.
 * vkGetInstanceProcAddr forwards to the ICD's vk_icdGetInstanceProcAddr; every
 * other entry point is reached through it. The ICD path comes from
 * PANVK_ICD_SO or falls back to the build tree. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
typedef void (*PFN_void)(void);
typedef PFN_void (*PFN_gipa)(void *, const char *);
static PFN_gipa icd_gipa;
__attribute__((constructor)) static void shim_init(void) {
    const char *p = getenv("PANVK_ICD_SO");
    if (!p || !p[0])
        p = "/data/data/com.termux/files/home/panvk-g57/mesa/build/src/panfrost/vulkan/libvulkan_panfrost.so";
    void *h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "vkshim: dlopen %s: %s\n", p, dlerror()); return; }
    icd_gipa = (PFN_gipa)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!icd_gipa) fprintf(stderr, "vkshim: no vk_icdGetInstanceProcAddr\n");
}
__attribute__((visibility("default"))) PFN_void vkGetInstanceProcAddr(void *instance, const char *name) {
    return icd_gipa ? icd_gipa(instance, name) : NULL;
}
