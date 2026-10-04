/* ahbprobe.c -- allocate an AHardwareBuffer like the Winlator wrapper's
 * swapchain does and print the gralloc native_handle layout: numFds, numInts,
 * and for every fd its lseek(SEEK_END) size and /proc/self/fd target. Mesa
 * drivers import handle->data[0] as the dma-buf; this shows whether that is
 * right for this gralloc.  usage: ahbprobe [width height] */
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/vfs.h>

typedef struct { int version, numFds, numInts; int data[0]; } nh_t;

int main(int argc, char **argv)
{
   void *nw = dlopen("libnativewindow.so", RTLD_NOW);
   if (!nw) { printf("dlopen libnativewindow: %s\n", dlerror()); return 1; }
   int (*alloc)(const AHardwareBuffer_Desc *, AHardwareBuffer **) = dlsym(nw, "AHardwareBuffer_allocate");
   void (*rel)(AHardwareBuffer *) = dlsym(nw, "AHardwareBuffer_release");
   const nh_t *(*gnh)(const AHardwareBuffer *) = dlsym(nw, "AHardwareBuffer_getNativeHandle");
   if (!alloc || !gnh) { printf("missing AHB symbols\n"); return 1; }

   AHardwareBuffer_Desc d = { 0 };
   d.width = argc > 2 ? atoi(argv[1]) : 600;
   d.height = argc > 2 ? atoi(argv[2]) : 600;
   d.layers = 1;
   d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
   d.usage = argc > 3 ? strtoull(argv[3], NULL, 0)
                      : AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
   AHardwareBuffer *b = NULL;
   int r = alloc(&d, &b);
   printf("allocate %ux%u RGBA8 usage=0x%llx r=%d\n", d.width, d.height, (unsigned long long)d.usage, r);
   if (r || !b) return 1;
   const nh_t *h = gnh(b);
   printf("native_handle version=%d numFds=%d numInts=%d\n", h->version, h->numFds, h->numInts);
   for (int i = 0; i < h->numFds; i++) {
      int fd = h->data[i];
      errno = 0;
      off_t sz = lseek(fd, 0, SEEK_END);
      int e = errno;
      lseek(fd, 0, SEEK_SET);
      char p[64], t[256] = "";
      snprintf(p, sizeof(p), "/proc/self/fd/%d", fd);
      ssize_t n = readlink(p, t, sizeof(t) - 1);
      if (n > 0) t[n] = 0;
      struct statfs sf; long ft = fstatfs(fd, &sf) == 0 ? (long)sf.f_type : -1;
      printf("  fd[%d]=%d size=%lld errno=%d (%s) fstype=0x%lx%s -> %s\n", i, fd, (long long)sz, e, e ? strerror(e) : "ok", ft, ft == 0x444d4142 ? " DMA_BUF" : "", t);
   }
   printf("  ints:");
   for (int i = 0; i < h->numInts && i < 24; i++) printf(" %d", h->data[h->numFds + i]);
   printf("\n");
   rel(b);
   return 0;
}
