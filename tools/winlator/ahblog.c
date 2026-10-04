/* ahblog.c -- LD_PRELOAD shim: log AHardwareBuffer_allocate descriptors
 * and the resulting native handle (numFds, which fd is a dma-buf), so we see
 * exactly what the Winlator wrapper allocates for its swapchain. */
#define _GNU_SOURCE
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

typedef struct { int version, numFds, numInts; int data[0]; } nh_t;

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *d, AHardwareBuffer **out)
{
   static int (*real)(const AHardwareBuffer_Desc *, AHardwareBuffer **);
   static const nh_t *(*gnh)(const AHardwareBuffer *);
   if (!real) {
      void *nw = dlopen("libnativewindow.so", RTLD_NOW);
      real = dlsym(nw, "AHardwareBuffer_allocate");
      gnh = dlsym(nw, "AHardwareBuffer_getNativeHandle");
   }
   int r = real(d, out);
   fprintf(stderr, "AHBLOG allocate %ux%u layers=%u fmt=%u usage=0x%llx -> r=%d", d->width, d->height,
           d->layers, d->format, (unsigned long long)d->usage, r);
   if (!r && *out && gnh) {
      const nh_t *h = gnh(*out);
      fprintf(stderr, " numFds=%d numInts=%d", h->numFds, h->numInts);
      for (int i = 0; i < h->numFds; i++) {
         off_t s = lseek(h->data[i], 0, SEEK_END);
         lseek(h->data[i], 0, SEEK_SET);
         fprintf(stderr, " fd%d:%lld", i, (long long)s);
      }
      if (h->numInts > 14)
         fprintf(stderr, " intfmt=0x%x", (unsigned)h->data[h->numFds + 14]);
   }
   fprintf(stderr, "\n");
   return r;
}
