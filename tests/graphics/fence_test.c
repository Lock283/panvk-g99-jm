/* fence_test.c -- does a fence passed to vkQueueSubmit ever signal?
 * CTS waits with vkWaitForFences; every Phase 4 harness used vkQueueWaitIdle. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <time.h>
#include "vulkan/vulkan_core.h"
typedef PFN_vkVoidFunction (*G)(VkInstance,const char*);
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(void){
  const char*mode=getenv("FENCE_MODE"); if(!mode)mode="empty";
  void*l=dlopen("/data/data/com.termux/files/home/panvk-g57/mesa/build/src/panfrost/vulkan/libvulkan_panfrost.so",RTLD_NOW);
  G gpa=(G)dlsym(l,"vk_icdGetInstanceProcAddr");
  VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
  VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};
  VkInstance in; ((PFN_vkCreateInstance)gpa(NULL,"vkCreateInstance"))(&ii,NULL,&in);
  uint32_t n=1; VkPhysicalDevice pd; ((PFN_vkEnumeratePhysicalDevices)gpa(in,"vkEnumeratePhysicalDevices"))(in,&n,&pd);
  float pr=1; VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&pr};
  VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
  VkDevice dev; ((PFN_vkCreateDevice)gpa(in,"vkCreateDevice"))(pd,&di,NULL,&dev);
  PFN_vkGetDeviceProcAddr gdpa=(PFN_vkGetDeviceProcAddr)gpa(in,"vkGetDeviceProcAddr");
  #define D(x) PFN_vk##x x=(PFN_vk##x)gdpa(dev,"vk" #x)
  D(GetDeviceQueue);D(CreateFence);D(QueueSubmit);D(WaitForFences);D(GetFenceStatus);D(CreateCommandPool);
  D(AllocateCommandBuffers);D(BeginCommandBuffer);D(EndCommandBuffer);D(QueueWaitIdle);D(CmdFillBuffer);
  D(CreateBuffer);D(GetBufferMemoryRequirements);D(AllocateMemory);D(BindBufferMemory);
  VkQueue q; GetDeviceQueue(dev,0,0,&q);
  VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence f; CreateFence(dev,&fi,NULL,&f);
  VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool cp; CreateCommandPool(dev,&cpi,NULL,&cp);
  VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.commandBufferCount=1};
  VkCommandBuffer cb; AllocateCommandBuffers(dev,&ca,&cb);
  VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; BeginCommandBuffer(cb,&bi);
  if(!strcmp(mode,"work")){
    VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=4096,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer b; CreateBuffer(dev,&bc,NULL,&b); VkMemoryRequirements mr; GetBufferMemoryRequirements(dev,b,&mr);
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=0};
    VkDeviceMemory m; AllocateMemory(dev,&ma,NULL,&m); BindBufferMemory(dev,b,m,0);
    CmdFillBuffer(cb,b,0,4096,0x1234);
  }
  EndCommandBuffer(cb);
  printf("mode=%s fence before submit: %d\n",mode,GetFenceStatus(dev,f));
  VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=strcmp(mode,"nocb")?1:0,.pCommandBuffers=&cb};
  double t0=now(); VkResult r=QueueSubmit(q,1,&si,f); printf("vkQueueSubmit=%d (%.3fs)\n",r,now()-t0);
  printf("fence right after submit: %d\n",GetFenceStatus(dev,f));
  t0=now(); r=WaitForFences(dev,1,&f,VK_TRUE,3000000000ull);
  printf("vkWaitForFences(3s)=%d after %.3fs  (0=SUCCESS, 2=TIMEOUT)\n",r,now()-t0);
  printf("fence status now: %d\n",GetFenceStatus(dev,f));
  t0=now(); r=QueueWaitIdle(q); printf("vkQueueWaitIdle=%d (%.3fs), fence after idle: %d\n",r,now()-t0,GetFenceStatus(dev,f));
  return 0;}
