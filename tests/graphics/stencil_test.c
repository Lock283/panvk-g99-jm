/* stencil_test.c -- T5.2, stencil testing on Valhall v9 / JM.
 *
 * Each case clears stencil to a value, then runs one to three rectangle draws
 * (A = [16,40)^2 at z 0.25, B = [24,48)^2 at z 0.75 on a 64x64 target) with a
 * fixed stencil state per draw: compare op, reference, compare/write masks,
 * pass/fail/depth-fail ops.
 *
 * Readout goes through the stencil test itself, not through a copy: after the
 * case, 256 full-screen "probe" draws run with compare EQUAL, reference k,
 * no stencil writes, each writing colour R = k. Every pixel is hit by exactly
 * the one probe whose k equals its stencil value, so the colour image IS the
 * stencil image. A pixel no probe hits keeps the clear colour (alpha 0), and
 * that is reported separately. If EQUAL were ignored, the last probe (255)
 * would win everywhere.
 *
 * A CPU model applies the same ops in the same order (Vulkan spec:
 * (ref & cmask) OP (s & cmask), then s = (s & ~wmask) | (op(s) & wmask)).
 * Every pixel is compared exactly.
 *
 * Negative control: STENCIL_LIE=1 makes the model swap pass and fail ops:
 * MUST fail. STENCIL_CASE selects the case, STENCIL_FMT=d24s8|d32s8|s8.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include "pngdump.h"
#include "vulkan/vulkan_core.h"

#define CHECK(e,m) do { VkResult _r=(e); \
    if(_r!=VK_SUCCESS){printf("FAILED: %s (VkResult=%d)\n",m,_r);return 1;} } while(0)
typedef PFN_vkVoidFunction (*PFN_icdGetInstanceProcAddr)(VkInstance,const char*);
#ifndef PANVK_DEFAULT_ICD_SO
#define PANVK_DEFAULT_ICD_SO \
 "/data/data/com.termux/files/home/panvk-g57/mesa/build/src/panfrost/vulkan/libvulkan_panfrost.so"
#endif
#define D 64

struct sdraw { int rect;            /* 0 = A, 1 = B */
               VkCompareOp cmp; uint8_t ref, cmask, wmask;
               VkStencilOp pass, fail, dfail; };
struct scase { const char*name; uint8_t clear; int depth; int n; struct sdraw d[3]; };

#define ALW VK_COMPARE_OP_ALWAYS
#define K  VK_STENCIL_OP_KEEP
static const struct scase cases[] = {
  {"replace",     0,  0,1,{{0,ALW,5,0xff,0xff,VK_STENCIL_OP_REPLACE,K,K}}},
  {"incr_clamp",253,  0,3,{{0,ALW,0,0xff,0xff,VK_STENCIL_OP_INCREMENT_AND_CLAMP,K,K},
                           {0,ALW,0,0xff,0xff,VK_STENCIL_OP_INCREMENT_AND_CLAMP,K,K},
                           {1,ALW,0,0xff,0xff,VK_STENCIL_OP_INCREMENT_AND_CLAMP,K,K}}},
  {"incr_wrap", 254,  0,3,{{0,ALW,0,0xff,0xff,VK_STENCIL_OP_INCREMENT_AND_WRAP,K,K},
                           {0,ALW,0,0xff,0xff,VK_STENCIL_OP_INCREMENT_AND_WRAP,K,K},
                           {1,ALW,0,0xff,0xff,VK_STENCIL_OP_INCREMENT_AND_WRAP,K,K}}},
  {"decr_clamp",  2,  0,3,{{0,ALW,0,0xff,0xff,VK_STENCIL_OP_DECREMENT_AND_CLAMP,K,K},
                           {0,ALW,0,0xff,0xff,VK_STENCIL_OP_DECREMENT_AND_CLAMP,K,K},
                           {1,ALW,0,0xff,0xff,VK_STENCIL_OP_DECREMENT_AND_CLAMP,K,K}}},
  {"decr_wrap",   1,  0,3,{{0,ALW,0,0xff,0xff,VK_STENCIL_OP_DECREMENT_AND_WRAP,K,K},
                           {0,ALW,0,0xff,0xff,VK_STENCIL_OP_DECREMENT_AND_WRAP,K,K},
                           {1,ALW,0,0xff,0xff,VK_STENCIL_OP_DECREMENT_AND_WRAP,K,K}}},
  {"invert",   0x0f,  0,2,{{0,ALW,0,0xff,0xff,VK_STENCIL_OP_INVERT,K,K},
                           {1,ALW,0,0xff,0xff,VK_STENCIL_OP_INVERT,K,K}}},
  {"zero",        7,  0,1,{{0,ALW,0,0xff,0xff,VK_STENCIL_OP_ZERO,K,K}}},
  /* B: EQUAL 3 passes only in the overlap (INCR -> 4), fails elsewhere
   * (REPLACE -> 3): pass and fail ops both visible */
  {"pass_fail",   0,  0,2,{{0,ALW,3,0xff,0xff,VK_STENCIL_OP_REPLACE,K,K},
                           {1,VK_COMPARE_OP_EQUAL,3,0xff,0xff,VK_STENCIL_OP_INCREMENT_AND_CLAMP,VK_STENCIL_OP_REPLACE,K}}},
  /* B: 6 > 5 passes (INCR -> 6), 6 > 8 fails in the overlap (ZERO -> 0) */
  {"greater",     5,  0,2,{{0,ALW,8,0xff,0xff,VK_STENCIL_OP_REPLACE,K,K},
                           {1,VK_COMPARE_OP_GREATER,6,0xff,0xff,VK_STENCIL_OP_INCREMENT_AND_CLAMP,VK_STENCIL_OP_ZERO,K}}},
  {"write_mask",0xff, 0,1,{{0,ALW,0x00,0xff,0x0f,VK_STENCIL_OP_REPLACE,K,K}}},
  /* compare mask 0x0f: 0x15 & 0x0f == 0x05 and 0x35 & 0x0f == 0x05, so all of
   * B passes and INVERTs; without the mask only... nothing would pass */
  {"compare_mask",0x35,0,2,{{0,ALW,0x15,0xff,0xff,VK_STENCIL_OP_REPLACE,K,K},
                           {1,VK_COMPARE_OP_EQUAL,0x05,0x0f,0xff,VK_STENCIL_OP_INVERT,VK_STENCIL_OP_ZERO,K}}},
  /* depth: A writes depth 0.25 (stencil KEEP), B at 0.75 with depth LESS:
   * overlap fails depth (depthFail REPLACE 9), B-only passes (INCR -> 1) */
  {"depth_fail",  0,  1,2,{{0,ALW,0,0xff,0xff,K,K,K},
                           {1,ALW,9,0xff,0xff,VK_STENCIL_OP_INCREMENT_AND_CLAMP,K,VK_STENCIL_OP_REPLACE}}},
};
static const int rx0[2]={16,24}, rx1[2]={40,48};
static const float rz[2]={0.25f,0.75f};

static int cmpf(VkCompareOp op,unsigned a,unsigned b){
    switch(op){ case VK_COMPARE_OP_NEVER: return 0; case VK_COMPARE_OP_LESS: return a<b;
    case VK_COMPARE_OP_EQUAL: return a==b; case VK_COMPARE_OP_LESS_OR_EQUAL: return a<=b;
    case VK_COMPARE_OP_GREATER: return a>b; case VK_COMPARE_OP_NOT_EQUAL: return a!=b;
    case VK_COMPARE_OP_GREATER_OR_EQUAL: return a>=b; default: return 1; }
}
static uint8_t sop(VkStencilOp op,uint8_t s,uint8_t ref){
    switch(op){ case VK_STENCIL_OP_KEEP: return s; case VK_STENCIL_OP_ZERO: return 0;
    case VK_STENCIL_OP_REPLACE: return ref; case VK_STENCIL_OP_INCREMENT_AND_CLAMP: return s==255?255:s+1;
    case VK_STENCIL_OP_DECREMENT_AND_CLAMP: return s==0?0:s-1; case VK_STENCIL_OP_INVERT: return ~s;
    case VK_STENCIL_OP_INCREMENT_AND_WRAP: return s+1; case VK_STENCIL_OP_DECREMENT_AND_WRAP: return s-1;
    default: return s; }
}
static void model(const struct scase*c,int lie,uint8_t st[D][D]){
    static float z[D][D];
    for(int y=0;y<D;y++) for(int x=0;x<D;x++){ st[y][x]=c->clear; z[y][x]=1.0f; }
    for(int i=0;i<c->n;i++){ const struct sdraw*d=&c->d[i]; int r=d->rect;
        for(int y=rx0[r];y<rx1[r];y++) for(int x=rx0[r];x<rx1[r];x++){
            uint8_t s=st[y][x]; VkStencilOp op;
            int sp=cmpf(d->cmp,d->ref&d->cmask,s&d->cmask);
            int dp=!c->depth || rz[r]<z[y][x];
            if(!sp) op = lie?d->pass:d->fail;
            else if(!dp) op=d->dfail;
            else op = lie?d->fail:d->pass;
            uint8_t v=sop(op,s,d->ref);
            st[y][x]=(s&~d->wmask)|(v&d->wmask);
            if(sp&&dp&&c->depth) z[y][x]=rz[r];
        }}
}

static PFN_vkCreateGraphicsPipelines CGP;
static VkDevice dev; static VkPipelineLayout pl; static VkShaderModule vs,fs;
static VkFormat FMT=VK_FORMAT_R8G8B8A8_UNORM, DSF;

/* one pipeline per stencil state; probe pipelines use a dynamic reference */
static VkPipeline make_pipe(const struct sdraw*d,int depth_test,int depth_write,int dyn_ref){
    VkPipelineShaderStageCreateInfo st[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vs,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fs,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vin={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vpi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo rsi={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo msi={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkStencilOpState so={.failOp=d->fail,.passOp=d->pass,.depthFailOp=d->dfail,.compareOp=d->cmp,
        .compareMask=d->cmask,.writeMask=d->wmask,.reference=d->ref};
    VkPipelineDepthStencilStateCreateInfo dsi={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable=depth_test,.depthWriteEnable=depth_write,.depthCompareOp=VK_COMPARE_OP_LESS,
        .stencilTestEnable=VK_TRUE,.front=so,.back=so};
    VkPipelineColorBlendAttachmentState cba={.colorWriteMask=0xf};
    VkPipelineColorBlendStateCreateInfo cbs={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&cba};
    VkDynamicState dy[3]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dyi={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=dyn_ref?3:2,.pDynamicStates=dy};
    int has_depth = DSF!=VK_FORMAT_S8_UINT;
    VkPipelineRenderingCreateInfo pri={.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,.colorAttachmentCount=1,
        .pColorAttachmentFormats=&FMT,.depthAttachmentFormat=has_depth?DSF:VK_FORMAT_UNDEFINED,.stencilAttachmentFormat=DSF};
    VkGraphicsPipelineCreateInfo gpi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&pri,.stageCount=2,.pStages=st,
        .pVertexInputState=&vin,.pInputAssemblyState=&ia,.pViewportState=&vpi,.pRasterizationState=&rsi,
        .pMultisampleState=&msi,.pDepthStencilState=&dsi,.pColorBlendState=&cbs,.pDynamicState=&dyi,.layout=pl};
    VkPipeline p=VK_NULL_HANDLE;
    VkResult r=CGP(dev,VK_NULL_HANDLE,1,&gpi,NULL,&p);
    if(r!=VK_SUCCESS){printf("FAILED: pipeline (VkResult=%d)\n",r);exit(1);}
    return p;
}
static char*rf(const char*p,size_t*s){
    FILE*f=fopen(p,"rb"); if(!f){printf("FAILED: open %s\n",p);return NULL;}
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    char*b=malloc(n); if(fread(b,1,n,f)!=(size_t)n){fclose(f);free(b);return NULL;}
    fclose(f);*s=(size_t)n;return b;
}
static uint32_t mtype(VkPhysicalDeviceMemoryProperties*mp,uint32_t b,VkMemoryPropertyFlags w){
    for(uint32_t i=0;i<mp->memoryTypeCount;i++)
        if((b&(1u<<i))&&(mp->memoryTypes[i].propertyFlags&w)==w) return i;
    return UINT32_MAX;
}

int main(void){
    const char*cs=getenv("STENCIL_CASE"); if(!cs||!cs[0]) cs="replace";
    const struct scase*tc=NULL;
    for(unsigned i=0;i<sizeof cases/sizeof cases[0];i++) if(!strcmp(cs,cases[i].name)) tc=&cases[i];
    if(!tc){printf("FAILED: unknown STENCIL_CASE=%s\n",cs);return 1;}
    const int lie=getenv("STENCIL_LIE")&&getenv("STENCIL_LIE")[0]=='1';
    const char*fn=getenv("STENCIL_FMT"); if(!fn||!fn[0]) fn="d24s8";
    DSF=!strcmp(fn,"s8")?VK_FORMAT_S8_UINT:!strcmp(fn,"d32s8")?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_D24_UNORM_S8_UINT;
    printf("=== stencil case: %s format=%s%s ===\n",tc->name,fn,lie?" NEGATIVE CONTROL: model swaps pass/fail ops":"");
    fflush(stdout);

    const char*icd=getenv("PANVK_ICD_SO"); if(!icd||!icd[0]) icd=PANVK_DEFAULT_ICD_SO;
    void*lib=dlopen(icd,RTLD_NOW); if(!lib){printf("dlopen: %s\n",dlerror());return 1;}
    PFN_icdGetInstanceProcAddr gpa=(PFN_icdGetInstanceProcAddr)dlsym(lib,"vk_icdGetInstanceProcAddr");
    PFN_vkCreateInstance CI=(PFN_vkCreateInstance)gpa(NULL,"vkCreateInstance");
    VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};
    VkInstance inst; CHECK(CI(&ii,NULL,&inst),"vkCreateInstance");
    #define IP(n) PFN_vk##n n=(PFN_vk##n)gpa(inst,"vk" #n)
    IP(GetDeviceProcAddr); IP(EnumeratePhysicalDevices); IP(CreateDevice); IP(GetPhysicalDeviceMemoryProperties);
    IP(GetPhysicalDeviceFormatProperties);
    uint32_t n=1; VkPhysicalDevice pd; CHECK(EnumeratePhysicalDevices(inst,&n,&pd),"enum");
    VkPhysicalDeviceMemoryProperties mp; GetPhysicalDeviceMemoryProperties(pd,&mp);
    VkFormatProperties fp; GetPhysicalDeviceFormatProperties(pd,DSF,&fp);
    printf("format      : %d optimalTilingFeatures=0x%x\n",DSF,fp.optimalTilingFeatures);
    if(!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)){
        printf("STENCILFP %s fmt=%s verdict=UNSUPPORTED\n",tc->name,fn); return 3; }
    if(tc->depth && DSF==VK_FORMAT_S8_UINT){ printf("STENCILFP %s fmt=%s verdict=SKIP (no depth aspect)\n",tc->name,fn); return 3; }
    VkPhysicalDeviceVulkan13Features f13={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.dynamicRendering=VK_TRUE};
    float pr=1; VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&pr};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&f13,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
    CHECK(CreateDevice(pd,&di,NULL,&dev),"vkCreateDevice");
    #define DP(x) PFN_vk##x x=(PFN_vk##x)GetDeviceProcAddr(dev,"vk" #x)
    DP(GetDeviceQueue); DP(CreateCommandPool); DP(AllocateCommandBuffers); DP(BeginCommandBuffer);
    DP(EndCommandBuffer); DP(QueueSubmit); DP(QueueWaitIdle); DP(CreateImage); DP(GetImageMemoryRequirements);
    DP(GetImageSubresourceLayout); DP(AllocateMemory); DP(BindImageMemory); DP(CreateImageView); DP(MapMemory);
    DP(CreateShaderModule); DP(CreatePipelineLayout); DP(CmdBeginRendering); DP(CmdEndRendering);
    DP(CmdBindPipeline); DP(CmdSetViewport); DP(CmdSetScissor); DP(CmdDraw); DP(CmdPipelineBarrier);
    DP(CmdPushConstants); DP(CmdSetStencilReference);
    CGP=(PFN_vkCreateGraphicsPipelines)GetDeviceProcAddr(dev,"vkCreateGraphicsPipelines");
    VkQueue q; GetDeviceQueue(dev,0,0,&q);

    const VkMemoryPropertyFlags HV=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const int has_depth = DSF!=VK_FORMAT_S8_UINT;
    const VkImageAspectFlags dsa = VK_IMAGE_ASPECT_STENCIL_BIT|(has_depth?VK_IMAGE_ASPECT_DEPTH_BIT:0);
    /* colour: LINEAR host visible */
    VkImageCreateInfo ic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=FMT,.extent={D,D,1},
        .mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_LINEAR,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VkImage img; CHECK(CreateImage(dev,&ic,NULL,&img),"colour"); VkMemoryRequirements mr; GetImageMemoryRequirements(dev,img,&mr);
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=mtype(&mp,mr.memoryTypeBits,HV)};
    VkDeviceMemory imem; CHECK(AllocateMemory(dev,&ma,NULL,&imem),"colour mem"); BindImageMemory(dev,img,imem,0);
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=img,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=FMT,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkImageView view; CHECK(CreateImageView(dev,&vi,NULL,&view),"colour view");
    /* depth/stencil: OPTIMAL */
    VkImageCreateInfo dic=ic; dic.format=DSF; dic.tiling=VK_IMAGE_TILING_OPTIMAL; dic.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImage dimg; CHECK(CreateImage(dev,&dic,NULL,&dimg),"ds image"); GetImageMemoryRequirements(dev,dimg,&mr);
    VkMemoryAllocateInfo dma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=mtype(&mp,mr.memoryTypeBits,0)};
    VkDeviceMemory dmem; CHECK(AllocateMemory(dev,&dma,NULL,&dmem),"ds mem"); BindImageMemory(dev,dimg,dmem,0);
    VkImageViewCreateInfo dvi=vi; dvi.image=dimg; dvi.format=DSF; dvi.subresourceRange.aspectMask=dsa;
    VkImageView dview; CHECK(CreateImageView(dev,&dvi,NULL,&dview),"ds view");

    size_t vz=0,fz=0; char*vc=rf("rect.vert.spv",&vz); char*fc=rf("rect.frag.spv",&fz); if(!vc||!fc) return 1;
    VkShaderModuleCreateInfo smi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=vz,.pCode=(uint32_t*)vc};
    CHECK(CreateShaderModule(dev,&smi,NULL,&vs),"vs"); smi.codeSize=fz; smi.pCode=(uint32_t*)fc; CHECK(CreateShaderModule(dev,&smi,NULL,&fs),"fs");
    VkPushConstantRange pcr={VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,36};
    VkPipelineLayoutCreateInfo pli={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.pushConstantRangeCount=1,.pPushConstantRanges=&pcr};
    CHECK(CreatePipelineLayout(dev,&pli,NULL,&pl),"layout");
    VkPipeline cp_[3]; for(int i=0;i<tc->n;i++) cp_[i]=make_pipe(&tc->d[i],tc->depth,tc->depth,0);
    struct sdraw probe={0,VK_COMPARE_OP_EQUAL,0,0xff,0x00,K,K,K};
    VkPipeline pp=make_pipe(&probe,0,0,1);

    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool cpool; CHECK(CreateCommandPool(dev,&cpi,NULL,&cpool),"pool");
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cpool,.commandBufferCount=1};
    VkCommandBuffer cmd; CHECK(AllocateCommandBuffers(dev,&cai,&cmd),"cb");
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(BeginCommandBuffer(cmd,&bi),"begin");
    VkImageMemoryBarrier b[2]={
        {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=img,
         .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
        {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
         .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=dimg,
         .subresourceRange={dsa,0,1,0,1},.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT}};
    CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,0,0,NULL,0,NULL,2,b);
    VkRenderingAttachmentInfo at={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=view,
        .imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue={.color={.float32={0,1,0,0}}}};
    VkRenderingAttachmentInfo dat={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=dview,
        .imageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,.clearValue={.depthStencil={1.0f,tc->clear}}};
    VkRenderingInfo ri={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{D,D}},.layerCount=1,
        .colorAttachmentCount=1,.pColorAttachments=&at,.pDepthAttachment=has_depth?&dat:NULL,.pStencilAttachment=&dat};
    CmdBeginRendering(cmd,&ri);
    VkViewport vp={0,0,D,D,0,1}; VkRect2D sc={{0,0},{D,D}}; CmdSetViewport(cmd,0,1,&vp); CmdSetScissor(cmd,0,1,&sc);
    for(int i=0;i<tc->n;i++){ int r=tc->d[i].rect;
        float pc[9]={rx0[r],rx0[r],rx1[r],rx1[r], 0,0,1,1, rz[r]};
        CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,cp_[i]);
        CmdPushConstants(cmd,pl,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,36,pc);
        CmdDraw(cmd,6,1,0,0); }
    CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pp);
    for(int k=0;k<256;k++){
        float pc[9]={0,0,D,D, k/255.0f,0,0,1, 0.5f};
        CmdSetStencilReference(cmd,VK_STENCIL_FACE_FRONT_AND_BACK,k);
        CmdPushConstants(cmd,pl,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,36,pc);
        CmdDraw(cmd,6,1,0,0); }
    CmdEndRendering(cmd);
    CHECK(EndCommandBuffer(cmd),"end");
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    CHECK(QueueSubmit(q,1,&si,VK_NULL_HANDLE),"submit"); CHECK(QueueWaitIdle(q),"wait");

    VkImageSubresource sr={VK_IMAGE_ASPECT_COLOR_BIT,0,0}; VkSubresourceLayout sl; GetImageSubresourceLayout(dev,img,&sr,&sl);
    uint8_t*m; MapMemory(dev,imem,0,VK_WHOLE_SIZE,0,(void**)&m); uint8_t*base=m+sl.offset;
    static uint8_t st[D][D]; model(tc,lie,st);
    uint32_t bad=0,unhit=0,hist_hw[256]={0},hist_m[256]={0}; int fx=-1,fy=-1;
    for(int y=0;y<D;y++) for(int x=0;x<D;x++){ const uint8_t*p=base+y*sl.rowPitch+x*4;
        if(p[3]==0){ unhit++; }
        else hist_hw[p[0]]++;
        hist_m[st[y][x]]++;
        if(p[3]!=255||p[0]!=st[y][x]||p[1]!=0||p[2]!=0){ bad++; if(fx<0){fx=x;fy=y;} } }
    printf("model values:"); for(int v=0;v<256;v++) if(hist_m[v]) printf(" %u x%u",v,hist_m[v]); printf("\n");
    printf("gpu values  :"); for(int v=0;v<256;v++) if(hist_hw[v]) printf(" %u x%u",v,hist_hw[v]); printf("  unhit=%u\n",unhit);
    if(fx>=0){const uint8_t*p=base+fy*sl.rowPitch+fx*4;
        printf("  first     : (%d,%d) got %u,%u,%u,%u model stencil %u\n",fx,fy,p[0],p[1],p[2],p[3],st[fy][fx]);}
    int ok=bad==0;
    printf("STENCILFP %s fmt=%s bad=%u unhit=%u verdict=%s\n",tc->name,fn,bad,unhit,ok?"PASS":"FAIL");
    const char*pp_=getenv("STENCIL_PNG");
    if(pp_&&pp_[0]){ FILE*f=png_open(pp_,"wb"); if(f){ fprintf(f,"P6\n%d %d\n255\n",D,D);
        for(int y=0;y<D;y++) for(int x=0;x<D;x++) fwrite(base+y*sl.rowPitch+x*4,1,3,f); png_close(f);} }
    printf("DONE\n");
    return ok?0:2;
}
