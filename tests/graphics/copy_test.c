/* copy_test.c -- T4.8.1, which transfer copy loses data on v9?
 * CTS api.smoke.compute and .transfer both fail with 0x00808080 at index 0.
 * Both round-trip buffer -> OPTIMAL image -> buffer. Steps tested separately:
 *   COPY_CASE=b2b      vkCmdCopyBuffer
 *   COPY_CASE=upd      vkCmdUpdateBuffer
 *   COPY_CASE=rt_opt   buffer -> OPTIMAL image -> buffer (what CTS does)
 *   COPY_CASE=rt_lin   same with a LINEAR image
 *   COPY_CASE=b2i_lin  buffer -> LINEAR host-visible image, read the image directly
 *   COPY_CASE=i2b_lin  host-written LINEAR image -> buffer
 * Source data is index+1 per uint32, as in CTS; destination is pre-poisoned
 * with 0xDEADBEEF so "not written" and "written wrong" differ. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "vulkan/vulkan_core.h"
#include "pngdump.h"
#define N 4096u            /* 64x64 RGBA8 = 4096 texels = 4096 uint32 */
#define D 64u
typedef PFN_vkVoidFunction (*G)(VkInstance,const char*);
static PFN_vkGetDeviceProcAddr gdpa; static VkDevice dev; static VkPhysicalDeviceMemoryProperties mp;
#define P(x) PFN_vk##x x=(PFN_vk##x)gdpa(dev,"vk" #x)
static uint32_t mt(uint32_t bits,VkMemoryPropertyFlags w){for(uint32_t i=0;i<mp.memoryTypeCount;i++)if((bits>>i&1)&&(mp.memoryTypes[i].propertyFlags&w)==w)return i;return 0;}
int main(void){
  const char*cs=getenv("COPY_CASE"); if(!cs)cs="rt_opt";
  void*l=dlopen("/data/data/com.termux/files/home/panvk-g57/mesa/build/src/panfrost/vulkan/libvulkan_panfrost.so",RTLD_NOW);
  G gpa=(G)dlsym(l,"vk_icdGetInstanceProcAddr");
  VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
  VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};
  VkInstance in; ((PFN_vkCreateInstance)gpa(NULL,"vkCreateInstance"))(&ii,NULL,&in);
  uint32_t n=1; VkPhysicalDevice pd; ((PFN_vkEnumeratePhysicalDevices)gpa(in,"vkEnumeratePhysicalDevices"))(in,&n,&pd);
  ((PFN_vkGetPhysicalDeviceMemoryProperties)gpa(in,"vkGetPhysicalDeviceMemoryProperties"))(pd,&mp);
  float pr=1; VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&pr};
  VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
  ((PFN_vkCreateDevice)gpa(in,"vkCreateDevice"))(pd,&di,NULL,&dev);
  gdpa=(PFN_vkGetDeviceProcAddr)gpa(in,"vkGetDeviceProcAddr");
  P(GetDeviceQueue);P(CreateBuffer);P(GetBufferMemoryRequirements);P(AllocateMemory);P(BindBufferMemory);P(MapMemory);
  P(CreateImage);P(GetImageMemoryRequirements);P(BindImageMemory);P(GetImageSubresourceLayout);
  P(CreateCommandPool);P(AllocateCommandBuffers);P(BeginCommandBuffer);P(EndCommandBuffer);P(QueueSubmit);P(QueueWaitIdle);
  P(CmdCopyBuffer);P(CmdUpdateBuffer);P(CmdCopyBufferToImage);P(CmdCopyImageToBuffer);P(CmdPipelineBarrier);
  VkQueue q; GetDeviceQueue(dev,0,0,&q);
  const VkMemoryPropertyFlags HV=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VkBuffer buf[2]; uint32_t*map[2]; VkDeviceMemory bmem[2];
  /* COPY_CACHED=1: put both buffers in the HOST_CACHED, non-coherent type and
   * use explicit flush/invalidate, which is what CTS's "HostVisible"
   * requirement selects first on this device. */
  const int cached = getenv("COPY_CACHED") != NULL;
  for(int i=0;i<2;i++){
    VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=N*4,.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    CreateBuffer(dev,&bc,NULL,&buf[i]); VkMemoryRequirements r; GetBufferMemoryRequirements(dev,buf[i],&r);
    uint32_t t = cached ? mt(r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT) : mt(r.memoryTypeBits,HV);
    if(i==0) printf("buffer memory type %u flags 0x%x\n",t,mp.memoryTypes[t].propertyFlags);
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,.memoryTypeIndex=t};
    VkDeviceMemory m; AllocateMemory(dev,&ma,NULL,&m); BindBufferMemory(dev,buf[i],m,0); MapMemory(dev,m,0,VK_WHOLE_SIZE,0,(void**)&map[i]); bmem[i]=m;
  }
  static uint32_t src[N]; for(uint32_t i=0;i<N;i++) src[i]=i+1;
  memcpy(map[0],src,N*4); for(uint32_t i=0;i<N;i++) map[1][i]=0xDEADBEEF;
  P(FlushMappedMemoryRanges); P(InvalidateMappedMemoryRanges);
  VkMappedMemoryRange mr2[2]={{.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=bmem[0],.size=VK_WHOLE_SIZE},
                              {.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=bmem[1],.size=VK_WHOLE_SIZE}};
  if(cached) FlushMappedMemoryRanges(dev,2,mr2);
  const int lin = strstr(cs,"lin")!=NULL;
  VkImage img=VK_NULL_HANDLE; uint8_t*imap=NULL; VkSubresourceLayout sl={0};
  if(strncmp(cs,"rt",2)==0||strstr(cs,"2i")||strstr(cs,"i2")){
    VkImageCreateInfo ic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
      .extent={D,D,1},.mipLevels=1,.arrayLayers=1,.samples=1,.tiling=lin?VK_IMAGE_TILING_LINEAR:VK_IMAGE_TILING_OPTIMAL,
      .usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,.initialLayout=lin?VK_IMAGE_LAYOUT_PREINITIALIZED:VK_IMAGE_LAYOUT_UNDEFINED};
    CreateImage(dev,&ic,NULL,&img); VkMemoryRequirements r; GetImageMemoryRequirements(dev,img,&r);
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,.memoryTypeIndex=mt(r.memoryTypeBits,lin?HV:0)};
    VkDeviceMemory m; AllocateMemory(dev,&ma,NULL,&m); BindImageMemory(dev,img,m,0);
    if(lin){ MapMemory(dev,m,0,VK_WHOLE_SIZE,0,(void**)&imap);
      VkImageSubresource s={VK_IMAGE_ASPECT_COLOR_BIT,0,0}; GetImageSubresourceLayout(dev,img,&s,&sl);
      for(uint32_t y=0;y<D;y++) for(uint32_t x=0;x<D;x++)
        ((uint32_t*)(imap+sl.offset+y*sl.rowPitch))[x] = strcmp(cs,"i2b_lin")==0 ? src[y*D+x] : 0xDEADBEEF; }
  }
  VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool cp; CreateCommandPool(dev,&cpi,NULL,&cp);
  VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.commandBufferCount=1};
  VkCommandBuffer cb; AllocateCommandBuffers(dev,&ca,&cb);
  VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; BeginCommandBuffer(cb,&bi);
  VkBufferImageCopy rg={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={D,D,1}};
  VkImageMemoryBarrier ib={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
    .image=img,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
  #define BAR(o,nl,sa,da) do{ib.oldLayout=o;ib.newLayout=nl;ib.srcAccessMask=sa;ib.dstAccessMask=da; \
    CmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&ib);}while(0)
  if(!strcmp(cs,"b2b")){ VkBufferCopy c={0,0,N*4}; CmdCopyBuffer(cb,buf[0],buf[1],1,&c); }
  else if(!strcmp(cs,"upd")){ CmdUpdateBuffer(cb,buf[1],0,N*4,src); }
  else if(!strncmp(cs,"rt",2)){
    BAR(lin?VK_IMAGE_LAYOUT_PREINITIALIZED:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    CmdCopyBufferToImage(cb,buf[0],img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&rg);
    BAR(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    CmdCopyImageToBuffer(cb,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buf[1],1,&rg);
  } else if(!strcmp(cs,"b2i_lin")){
    BAR(VK_IMAGE_LAYOUT_PREINITIALIZED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    CmdCopyBufferToImage(cb,buf[0],img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&rg);
  } else if(!strcmp(cs,"i2b_lin")){
    BAR(VK_IMAGE_LAYOUT_PREINITIALIZED,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    CmdCopyImageToBuffer(cb,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buf[1],1,&rg);
  }
  EndCommandBuffer(cb);
  VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};
  QueueSubmit(q,1,&si,VK_NULL_HANDLE); QueueWaitIdle(q);
  if(cached) InvalidateMappedMemoryRanges(dev,1,&mr2[1]);
  const uint32_t*got = map[1];
  static uint32_t tmp[N];
  if(!strcmp(cs,"b2i_lin")){ for(uint32_t y=0;y<D;y++) for(uint32_t x=0;x<D;x++) tmp[y*D+x]=((uint32_t*)(imap+sl.offset+y*sl.rowPitch))[x]; got=tmp; }
  uint32_t bad=0,poison=0,first=UINT32_MAX;
  for(uint32_t i=0;i<N;i++){ if(got[i]==0xDEADBEEF) poison++; if(got[i]!=src[i]){bad++; if(first==UINT32_MAX)first=i;} }
  if(getenv("COPY_DUMP")) for(uint32_t i=0,k=0;i<N&&k<24;i++) if(got[i]!=src[i]){printf("  [%u] got %08x want %08x\n",i,got[i],src[i]);k++;}
  if(getenv("COPY_DUMP")){ uint32_t hist[4][2]={{0}}; for(uint32_t i=0;i<N;i++) for(int c=0;c<4;c++){ uint8_t w=src[i]>>(8*c),g=got[i]>>(8*c); hist[c][w!=g]++; }
    for(int c=0;c<4;c++) printf("  byte%d: ok=%u bad=%u\n",c,hist[c][0],hist[c][1]); }
  /* COPY_PNG: write the result as a 64x64 image, green = correct texel,
   * red = wrong, black = untouched poison. */
  if(getenv("COPY_PNG")){ FILE*f=png_open(getenv("COPY_PNG"),"wb"); fprintf(f,"P6\n%u %u\n255\n",D,D);
    for(uint32_t i=0;i<N;i++){ uint8_t c[3]={0,0,0}; if(got[i]==src[i]) c[1]=200; else if(got[i]!=0xDEADBEEF) c[0]=255; fwrite(c,1,3,f);} png_close(f); }
  printf("COPYFP %s bad=%u poison=%u",cs,bad,poison);
  if(first!=UINT32_MAX) printf(" first=%u got=0x%08x want=0x%08x",first,got[first],src[first]);
  printf(" [0..3]=%08x %08x %08x %08x verdict=%s\n",got[0],got[1],got[2],got[3],bad?"FAIL":"PASS");
  return bad?2:0;}
