/* featq.c -- dump the Vulkan 1.1/1.2/1.3 and extension features DXVK needs */
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include "vulkan/vulkan_core.h"
typedef PFN_vkVoidFunction (*G)(VkInstance,const char*);
int main(void){
  const char*icd=getenv("PANVK_ICD_SO"); void*l=dlopen(icd,RTLD_NOW); if(!l){puts(dlerror());return 1;}
  G g=(G)dlsym(l,"vk_icdGetInstanceProcAddr");
  VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
  VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};
  VkInstance in; ((PFN_vkCreateInstance)g(NULL,"vkCreateInstance"))(&ii,NULL,&in);
  uint32_t n=1; VkPhysicalDevice pd; ((PFN_vkEnumeratePhysicalDevices)g(in,"vkEnumeratePhysicalDevices"))(in,&n,&pd);
  VkPhysicalDeviceRobustness2FeaturesEXT rb={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
  VkPhysicalDeviceDepthClipEnableFeaturesEXT dc={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT,.pNext=&rb};
  VkPhysicalDeviceMaintenance5FeaturesKHR m5={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,.pNext=&dc};
  VkPhysicalDeviceMaintenance6FeaturesKHR m6={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR,.pNext=&m5};
  VkPhysicalDeviceTransformFeedbackFeaturesEXT tf={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,.pNext=&m6};
  VkPhysicalDeviceVulkan13Features v13={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.pNext=&tf};
  VkPhysicalDeviceVulkan12Features v12={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,.pNext=&v13};
  VkPhysicalDeviceVulkan11Features v11={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,.pNext=&v12};
  VkPhysicalDeviceFeatures2 f={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&v11};
  ((PFN_vkGetPhysicalDeviceFeatures2)g(in,"vkGetPhysicalDeviceFeatures2"))(pd,&f);
#define P(s,x) printf("%s = %d\n",#x,(int)s.x)
  P(v11,shaderDrawParameters);P(v11,storageBuffer16BitAccess);
  P(v12,bufferDeviceAddress);P(v12,descriptorIndexing);P(v12,storageBuffer8BitAccess);P(v12,descriptorBindingSampledImageUpdateAfterBind);
  P(v12,descriptorBindingUpdateUnusedWhilePending);P(v12,descriptorBindingPartiallyBound);P(v12,hostQueryReset);P(v12,runtimeDescriptorArray);
  P(v12,samplerMirrorClampToEdge);P(v12,scalarBlockLayout);P(v12,shaderInt8);P(v12,timelineSemaphore);P(v12,uniformBufferStandardLayout);
  P(v12,vulkanMemoryModel);P(v13,inlineUniformBlock);P(v13,computeFullSubgroups);P(v13,dynamicRendering);P(v13,maintenance4);
  P(v13,shaderDemoteToHelperInvocation);P(v13,shaderZeroInitializeWorkgroupMemory);P(v13,subgroupSizeControl);P(v13,synchronization2);
  P(rb,robustBufferAccess2);P(rb,nullDescriptor);P(dc,depthClipEnable);P(m5,maintenance5);P(m6,maintenance6);P(tf,transformFeedback);
  return 0; }
