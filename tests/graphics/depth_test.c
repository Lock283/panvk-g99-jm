/* depth_test.c -- T4.7.1, depth testing on Valhall v9 / JM.
 *
 * Two overlapping quads, A (red, z 0.25) and B (green, z 0.75), from
 * depth2.vert. Edges fall on integer pixel coordinates, so coverage is exact.
 *
 * No expected image or depth value is hand-written. The harness runs a CPU
 * model of the Vulkan depth test over the same quads in the same order: for
 * each fragment, if the test is disabled or compare(z, stored) passes, the
 * colour is written, and the depth is written if writes are enabled. Both the
 * colour attachment and the depth attachment are LINEAR and host visible, so
 * both are compared against the model directly, every pixel, colour exactly and
 * depth to 1e-6.
 *
 * The cases are chosen so that each control produces a different image from the
 * case it controls:
 *   off_ab vs less_ab        test disabled vs enabled, same order
 *   less_ab vs less_ba       LESS must make the result order-independent
 *   greater_ba vs off_ba     GREATER keeps the farther quad, off keeps the last
 *   less_clear05             clear depth 0.5: only A can pass
 *   less_clear01             clear depth 0.1: nothing passes
 *   less_nowrite             writes off: B passes in the overlap although A was
 *                            drawn first and is nearer
 *   equal_clear025           EQUAL against clear 0.25: only A passes
 * DEPTH_LIE=1 makes the CPU model use the opposite compare op, so the
 * comparison MUST fail; that checks the harness, not the driver.
 *
 * Select with DEPTH_CASE. DEPTH_FMT=d16|d32 (default d32).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include "pngdump.h"
#include <math.h>
#include "vulkan/vulkan_core.h"

#define CHECK(e,m) do { VkResult _r=(e); \
    if(_r!=VK_SUCCESS){printf("FAILED: %s (VkResult=%d)\n",m,_r);return 1;} } while(0)

typedef PFN_vkVoidFunction (*PFN_icdGetInstanceProcAddr)(VkInstance,const char*);
#define DIMS 64

static uint32_t mtype(VkPhysicalDeviceMemoryProperties*mp,uint32_t b,VkMemoryPropertyFlags w){
    for(uint32_t i=0;i<mp->memoryTypeCount;i++)
        if((b&(1u<<i))&&(mp->memoryTypes[i].propertyFlags&w)==w) return i;
    return UINT32_MAX;
}
static char*rf(const char*p,size_t*s){
    FILE*f=fopen(p,"rb"); if(!f){printf("FAILED: open %s\n",p);return NULL;}
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    char*b=malloc(n);
    if(fread(b,1,n,f)!=(size_t)n){printf("FAILED: read %s\n",p);fclose(f);free(b);return NULL;}
    fclose(f);*s=(size_t)n;return b;
}

#ifndef PANVK_DEFAULT_ICD_SO
#define PANVK_DEFAULT_ICD_SO \
 "/data/data/com.termux/files/home/panvk-g57/mesa/build/src/panfrost/vulkan/libvulkan_panfrost.so"
#endif

struct dcase {
    const char *name;
    int test, write;
    VkCompareOp op;
    float clear;
    int order_ba;       /* 1: draw B then A */
};
static int cmp(VkCompareOp op, float a, float b){
    switch(op){
    case VK_COMPARE_OP_NEVER: return 0;
    case VK_COMPARE_OP_LESS: return a<b;
    case VK_COMPARE_OP_EQUAL: return a==b;
    case VK_COMPARE_OP_LESS_OR_EQUAL: return a<=b;
    case VK_COMPARE_OP_GREATER: return a>b;
    case VK_COMPARE_OP_NOT_EQUAL: return a!=b;
    case VK_COMPARE_OP_GREATER_OR_EQUAL: return a>=b;
    default: return 1; }
}
static VkCompareOp opposite(VkCompareOp op){
    switch(op){
    case VK_COMPARE_OP_LESS: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case VK_COMPARE_OP_GREATER: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case VK_COMPARE_OP_EQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    default: return VK_COMPARE_OP_NEVER; }
}

int main(void){
    const char*cs=getenv("DEPTH_CASE"); if(!cs||!cs[0]) cs="less_ab";
    struct dcase cases[] = {
      { "off_ab",         0,1, VK_COMPARE_OP_LESS,    1.0f,  0 },
      { "off_ba",         0,1, VK_COMPARE_OP_LESS,    1.0f,  1 },
      { "less_ab",        1,1, VK_COMPARE_OP_LESS,    1.0f,  0 },
      { "less_ba",        1,1, VK_COMPARE_OP_LESS,    1.0f,  1 },
      { "greater_ba",     1,1, VK_COMPARE_OP_GREATER, 0.0f,  1 },
      { "less_clear05",   1,1, VK_COMPARE_OP_LESS,    0.5f,  0 },
      { "less_clear01",   1,1, VK_COMPARE_OP_LESS,    0.1f,  0 },
      { "less_nowrite",   1,0, VK_COMPARE_OP_LESS,    1.0f,  0 },
      { "equal_clear025", 1,1, VK_COMPARE_OP_EQUAL,   0.25f, 0 },
    };
    struct dcase*tc=NULL;
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++)
        if(!strcmp(cs,cases[i].name)) tc=&cases[i];
    if(!tc){printf("FAILED: unknown DEPTH_CASE=%s\n",cs);return 1;}
    const int lie = getenv("DEPTH_LIE") && getenv("DEPTH_LIE")[0]=='1';
    const char*fmtn=getenv("DEPTH_FMT"); if(!fmtn||!fmtn[0]) fmtn="d32";
    const VkFormat DFMT = !strcmp(fmtn,"d16") ? VK_FORMAT_D16_UNORM : VK_FORMAT_D32_SFLOAT;
    printf("=== case: %s ===\n",tc->name);
    printf("depth       : test=%d write=%d op=%d clear=%.3f order=%s format=%s\n",
           tc->test,tc->write,tc->op,tc->clear,tc->order_ba?"B,A":"A,B",fmtn);
    if(lie) printf("NEGATIVE CONTROL: CPU model uses the opposite compare op\n");
    fflush(stdout);

    const char*icd=getenv("PANVK_ICD_SO"); if(!icd||!icd[0]) icd=PANVK_DEFAULT_ICD_SO;
    void*lib=dlopen(icd,RTLD_NOW); if(!lib){printf("dlopen: %s\n",dlerror());return 1;}
    PFN_icdGetInstanceProcAddr gpa=
        (PFN_icdGetInstanceProcAddr)dlsym(lib,"vk_icdGetInstanceProcAddr");
    if(!gpa){printf("dlsym failed\n");return 1;}
    PFN_vkCreateInstance CI=(PFN_vkCreateInstance)gpa(NULL,"vkCreateInstance");
    VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
                          .apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo=&ai};
    VkInstance inst; CHECK(CI(&ii,NULL,&inst),"vkCreateInstance");
    #define IP(n) (PFN_vk##n)gpa(inst,"vk" #n)
    PFN_vkGetDeviceProcAddr GDPA=IP(GetDeviceProcAddr);
    PFN_vkEnumeratePhysicalDevices EPD=IP(EnumeratePhysicalDevices);
    PFN_vkCreateDevice CD=IP(CreateDevice);
    PFN_vkGetPhysicalDeviceMemoryProperties GMP=IP(GetPhysicalDeviceMemoryProperties);
    uint32_t n=0; CHECK(EPD(inst,&n,NULL),"enum");
    if(!n){printf("FAILED: no device\n");return 1;}
    VkPhysicalDevice pd; CHECK(EPD(inst,&n,&pd),"enum2");
    VkPhysicalDeviceMemoryProperties mp; GMP(pd,&mp);
    VkPhysicalDeviceVulkan13Features f13={
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .dynamicRendering=VK_TRUE};
    float pr=1.0f;
    VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=0,.queueCount=1,.pQueuePriorities=&pr};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&f13,
        .queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
    VkDevice dev; CHECK(CD(pd,&di,NULL,&dev),"vkCreateDevice");
    #define DP(x) (PFN_vk##x)GDPA(dev,"vk" #x)
    PFN_vkGetDeviceQueue GQ=DP(GetDeviceQueue);
    PFN_vkCreateCommandPool CCP=DP(CreateCommandPool);
    PFN_vkAllocateCommandBuffers ACB=DP(AllocateCommandBuffers);
    PFN_vkBeginCommandBuffer BCB=DP(BeginCommandBuffer);
    PFN_vkEndCommandBuffer ECB=DP(EndCommandBuffer);
    PFN_vkQueueSubmit QS=DP(QueueSubmit);
    PFN_vkQueueWaitIdle QWI=DP(QueueWaitIdle);
    PFN_vkCreateImage CIM=DP(CreateImage);
    PFN_vkGetImageMemoryRequirements GIMR=DP(GetImageMemoryRequirements);
    PFN_vkGetImageSubresourceLayout GISL=DP(GetImageSubresourceLayout);
    PFN_vkAllocateMemory AM=DP(AllocateMemory);
    PFN_vkBindImageMemory BIM=DP(BindImageMemory);
    PFN_vkCreateImageView CIV=DP(CreateImageView);
    PFN_vkMapMemory MM=DP(MapMemory);
    PFN_vkUnmapMemory UM=DP(UnmapMemory);
    PFN_vkCreateShaderModule CSM=DP(CreateShaderModule);
    PFN_vkCreatePipelineLayout CPL=DP(CreatePipelineLayout);
    PFN_vkCreateGraphicsPipelines CGP=DP(CreateGraphicsPipelines);
    PFN_vkCmdBeginRendering CBR=DP(CmdBeginRendering);
    PFN_vkCmdEndRendering CER=DP(CmdEndRendering);
    PFN_vkCmdBindPipeline CBP=DP(CmdBindPipeline);
    PFN_vkCmdSetViewport CSV=DP(CmdSetViewport);
    PFN_vkCmdSetScissor CSS=DP(CmdSetScissor);
    PFN_vkCmdDraw CDraw=DP(CmdDraw);
    PFN_vkCmdDrawIndirect CDrawInd=DP(CmdDrawIndirect);
    PFN_vkCmdDrawIndexed CDrawIdx=DP(CmdDrawIndexed);
    PFN_vkCmdDrawIndexedIndirect CDrawIdxInd=DP(CmdDrawIndexedIndirect);
    PFN_vkCmdBindIndexBuffer CBindIB=DP(CmdBindIndexBuffer);
    PFN_vkCmdPipelineBarrier CPB=DP(CmdPipelineBarrier);
    PFN_vkCreateBuffer CB=DP(CreateBuffer);
    PFN_vkGetBufferMemoryRequirements GBMR=DP(GetBufferMemoryRequirements);
    PFN_vkBindBufferMemory BBM=DP(BindBufferMemory);

    VkQueue q; GQ(dev,0,0,&q);
    VkFormat FMT=VK_FORMAT_R8G8B8A8_UNORM;

    /* depth attachment, LINEAR + host visible so it can be read back directly */
    VkImageCreateInfo dic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=DFMT,.extent={DIMS,DIMS,1},
        .mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_LINEAR,.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage dimg; CHECK(CIM(dev,&dic,NULL,&dimg),"vkCreateImage(depth)");
    VkMemoryRequirements dmr; GIMR(dev,dimg,&dmr);
    uint32_t dt=mtype(&mp,dmr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(dt==UINT32_MAX){printf("FAILED: no host-visible memory for depth\n");return 1;}
    VkMemoryAllocateInfo dma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=dmr.size,.memoryTypeIndex=dt};
    VkDeviceMemory dmem; CHECK(AM(dev,&dma,NULL,&dmem),"vkAllocateMemory(depth)");
    CHECK(BIM(dev,dimg,dmem,0),"vkBindImageMemory(depth)");
    VkImageViewCreateInfo dvi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=dimg,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=DFMT,
        .subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1}};
    VkImageView dview; CHECK(CIV(dev,&dvi,NULL,&dview),"vkCreateImageView(depth)");

    VkImageCreateInfo ic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=FMT,.extent={DIMS,DIMS,1},
        .mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_LINEAR,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img; CHECK(CIM(dev,&ic,NULL,&img),"vkCreateImage");
    VkMemoryRequirements mr; GIMR(dev,img,&mr);
    uint32_t t=mtype(&mp,mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=mr.size,.memoryTypeIndex=t};
    VkDeviceMemory imgm; CHECK(AM(dev,&ma,NULL,&imgm),"vkAllocateMemory(image)");
    CHECK(BIM(dev,img,imgm,0),"vkBindImageMemory");
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=img,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=FMT,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkImageView view; CHECK(CIV(dev,&vi,NULL,&view),"vkCreateImageView");

    size_t vz=0,fz=0;
    char*vc=rf("depth2.vert.spv",&vz); if(!vc)return 1;
    char*fc=rf("depth2.frag.spv",&fz); if(!fc)return 1;
    VkShaderModuleCreateInfo vm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=vz,.pCode=(uint32_t*)vc};
    VkShaderModule vs; CHECK(CSM(dev,&vm,NULL,&vs),"vert");
    VkShaderModuleCreateInfo fm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=fz,.pCode=(uint32_t*)fc};
    VkShaderModule fs; CHECK(CSM(dev,&fm,NULL,&fs),"frag");
    VkPipelineLayoutCreateInfo pli={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout pl; CHECK(CPL(dev,&pli,NULL,&pl),"layout");
    VkPipelineShaderStageCreateInfo st[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vs,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fs,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vin={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vpi={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo rsi={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,
        .frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1.0f};
    VkPipelineMultisampleStateCreateInfo msi={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo dsi={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable=tc->test,.depthWriteEnable=tc->write,
        .depthCompareOp=tc->op};
    VkPipelineColorBlendAttachmentState cbaa={
        .colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                        VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo cbi={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&cbaa};
    VkDynamicState dy[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyi={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount=2,.pDynamicStates=dy};
    VkPipelineRenderingCreateInfo pri={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount=1,.pColorAttachmentFormats=&FMT,
        .depthAttachmentFormat=DFMT};
    VkGraphicsPipelineCreateInfo gpi={
        .sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&pri,
        .stageCount=2,.pStages=st,.pVertexInputState=&vin,
        .pInputAssemblyState=&ia,.pViewportState=&vpi,
        .pRasterizationState=&rsi,.pMultisampleState=&msi,.pDepthStencilState=&dsi,
        .pColorBlendState=&cbi,.pDynamicState=&dyi,.layout=pl};
    VkPipeline pipe; CHECK(CGP(dev,VK_NULL_HANDLE,1,&gpi,NULL,&pipe),"pipeline");

    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex=0};
    VkCommandPool cp; CHECK(CCP(dev,&cpi,NULL,&cp),"pool");
    VkCommandBufferAllocateInfo cbi2={
        .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd; CHECK(ACB(dev,&cbi2,&cmd),"cb");
    VkCommandBufferBeginInfo cbb={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(BCB(cmd,&cbb),"begin");
    VkImageMemoryBarrier br={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=img,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},
        .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    VkImageMemoryBarrier dbr={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=dimg,
        .subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1},
        .dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    VkImageMemoryBarrier brs[2]={br,dbr};
    CPB(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        0,0,NULL,0,NULL,2,brs);
    VkRenderingAttachmentInfo at={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView=view,.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue={.color={.float32={0,0,0,1}}}};
    VkRenderingAttachmentInfo dat={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView=dview,.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue={.depthStencil={tc->clear,0}}};
    VkRenderingInfo ri={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea={{0,0},{DIMS,DIMS}},.layerCount=1,
        .colorAttachmentCount=1,.pColorAttachments=&at,.pDepthAttachment=&dat};
    CBR(cmd,&ri);
    VkViewport vp={0,0,(float)DIMS,(float)DIMS,0.0f,1.0f};
    VkRect2D sc={{0,0},{DIMS,DIMS}};
    CSV(cmd,0,1,&vp); CSS(cmd,0,1,&sc);
    CBP(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
    /* Two separate draws so the order is an API order, not intra-draw order. */
    if(tc->order_ba){ CDraw(cmd,6,1,6,0); CDraw(cmd,6,1,0,0); }
    else            { CDraw(cmd,6,1,0,0); CDraw(cmd,6,1,6,0); }
    printf("drawn       : quad %s then quad %s\n",tc->order_ba?"B":"A",tc->order_ba?"A":"B");
    fflush(stdout);
    CER(cmd);
    CHECK(ECB(cmd),"end");
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount=1,.pCommandBuffers=&cmd};
    CHECK(QS(q,1,&si,VK_NULL_HANDLE),"vkQueueSubmit");
    CHECK(QWI(q),"vkQueueWaitIdle");
    printf("vkQueueWaitIdle returned (no hang)\n");

    VkImageSubresource sr={VK_IMAGE_ASPECT_COLOR_BIT,0,0};
    VkSubresourceLayout sl; GISL(dev,img,&sr,&sl);
    void*m=NULL; CHECK(MM(dev,imgm,0,VK_WHOLE_SIZE,0,&m),"map image");
    uint8_t*base=(uint8_t*)m+sl.offset;
    /* ---- CPU model of the depth test ---- */
    static uint8_t rc[DIMS][DIMS][3]; static float rd[DIMS][DIMS];
    const VkCompareOp mop = lie ? opposite(tc->op) : tc->op;
    for(int y=0;y<DIMS;y++) for(int x=0;x<DIMS;x++){ rc[y][x][0]=rc[y][x][1]=rc[y][x][2]=0; rd[y][x]=tc->clear; }
    const int lo[2]={16,24}; const float qz[2]={0.25f,0.75f};
    for(int k=0;k<2;k++){
        int q = tc->order_ba ? 1-k : k;
        for(int y=lo[q];y<lo[q]+24;y++) for(int x=lo[q];x<lo[q]+24;x++){
            if(tc->test && !cmp(mop, qz[q], rd[y][x])) continue;
            rc[y][x][0] = q==0 ? 255 : 0; rc[y][x][1] = q==1 ? 255 : 0; rc[y][x][2]=0;
            if(tc->test && tc->write) rd[y][x]=qz[q];
        }
    }
    /* Depth writes only happen when the test is enabled (spec: depth writes
     * are disabled when depthTestEnable is VK_FALSE). */

    /* ---- compare colour ---- */
    uint32_t red=0, green=0, cbad=0; int cfx=-1,cfy=-1;
    for(int y=0;y<DIMS;y++){uint8_t*row=base+y*sl.rowPitch;
        for(int x=0;x<DIMS;x++){uint8_t*p=row+x*4;
            if(p[0]==255&&p[1]==0) red++; if(p[1]==255&&p[0]==0) green++;
            if(p[0]!=rc[y][x][0]||p[1]!=rc[y][x][1]||p[2]!=rc[y][x][2]){cbad++; if(cfx<0){cfx=x;cfy=y;}}
        }}
    /* ---- compare depth ---- */
    VkImageSubresource dsr={VK_IMAGE_ASPECT_DEPTH_BIT,0,0};
    VkSubresourceLayout dsl; GISL(dev,dimg,&dsr,&dsl);
    void*dm=NULL; CHECK(MM(dev,dmem,0,VK_WHOLE_SIZE,0,&dm),"map depth");
    uint8_t*db=(uint8_t*)dm+dsl.offset;
    const int d16 = DFMT==VK_FORMAT_D16_UNORM;
    const double dtol = d16 ? 1.0/65535.0 : 1e-6;
    uint32_t dbad=0; int dfx=-1,dfy=-1; double dmaxe=0, got_at_fx=0;
    for(int y=0;y<DIMS;y++) for(int x=0;x<DIMS;x++){
        double g = d16 ? ((uint16_t*)(db+y*dsl.rowPitch))[x]/65535.0
                       : ((float*)(db+y*dsl.rowPitch))[x];
        double e = fabs(g - rd[y][x]); if(e>dmaxe) dmaxe=e;
        if(e>dtol){dbad++; if(dfx<0){dfx=x;dfy=y;got_at_fx=g;}}
    }
    UM(dev,dmem);
    uint32_t mred=0,mgreen=0;
    for(int y=0;y<DIMS;y++) for(int x=0;x<DIMS;x++){ if(rc[y][x][0]) mred++; if(rc[y][x][1]) mgreen++; }
    int ok = cbad==0 && dbad==0;
    printf("\n--- result: %s ---\n",tc->name);
    printf("colour      : red=%u green=%u   model red=%u green=%u   mismatching pixels=%u\n",
           red,green,mred,mgreen,cbad);
    if(cfx>=0){uint8_t*p=base+cfy*sl.rowPitch+cfx*4;
        printf("  first     : (%d,%d) got %u,%u,%u model %u,%u,%u\n",cfx,cfy,p[0],p[1],p[2],
               rc[cfy][cfx][0],rc[cfy][cfx][1],rc[cfy][cfx][2]);}
    printf("depth       : rowPitch=%llu mismatching=%u max error=%.3g (tol %.3g)\n",
           (unsigned long long)dsl.rowPitch,dbad,dmaxe,dtol);
    if(dfx>=0) printf("  first     : (%d,%d) got %.6f model %.6f\n",dfx,dfy,got_at_fx,rd[dfy][dfx]);
    printf("DEPTHFP %s fmt=%s red=%u green=%u cbad=%u dbad=%u verdict=%s\n",
           tc->name,fmtn,red,green,cbad,dbad,ok?"PASS":"FAIL");

    const char*pp=getenv("DEPTH_PPM");
    if(pp&&pp[0]){
        FILE*f=png_open(pp,"wb");
        if(f){fprintf(f,"P6\n%d %d\n255\n",DIMS,DIMS);
            for(int y=0;y<DIMS;y++){uint8_t*row=base+y*sl.rowPitch;
                for(int x=0;x<DIMS;x++){uint8_t*p=row+x*4;
                    uint8_t rgb[3]={p[0],p[1],p[2]};fwrite(rgb,1,3,f);}}
            png_close(f);printf("VISUAL_DUMP: %s\n",pp);}
    }
    UM(dev,imgm);
    free(vc); free(fc);
    printf("DONE\n");
    return ok?0:2;
}
