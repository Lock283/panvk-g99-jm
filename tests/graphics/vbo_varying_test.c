/* vbo_varying_test.c -- T4.6, vertex buffers and varyings on Valhall v9 / JM.
 *
 * Everything in Phase 4 before this used shaders that build positions from
 * gl_VertexIndex and need no vertex buffer and no varying. This test feeds the
 * vertex shader from real vertex buffers (per-vertex position and colour, plus a
 * per-INSTANCE cell origin) and interpolates the colour into the fragment shader.
 *
 * No expected image is hand-written. The harness rasterizes the same triangles
 * on the CPU with the Vulkan coverage rule (pixel centre inside), interpolates
 * the vertex colours barycentrically (w = 1, so linear is exact), and compares
 * every pixel with a tolerance of 2 per channel for fp rounding. Pixel centres
 * within 1e-3 px of an edge are reported and not scored.
 *
 * The reference applies the draw parameters exactly as the spec says, so a path
 * that ignores firstVertex, vertexOffset or firstInstance for attribute fetch
 * fails here, and so does wrong interpolation.
 *
 * Geometry: 8 cells, 4 columns x 2 rows, 16 x 32 px each. Instance i draws in
 * cell i, taking its origin from the instance-rate attribute. Vertices 0..2 are
 * triangle A (red, green, blue corners), 3..5 triangle B (yellow, cyan, magenta).
 *
 * Negative controls via VBO_LIE, which corrupt only the GPU-side data while the
 * CPU reference keeps the true values, so the comparison MUST fail:
 *   VBO_LIE=color   vertex colours rotated by one channel
 *   VBO_LIE=cell    instance origins shifted by one cell
 *
 * Select with VBO_CASE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
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

struct tcase {
    const char *name;
    int indirect, indexed;
    VkDrawIndirectCommand c;         /* non-indexed parameters */
    VkDrawIndexedIndirectCommand x;  /* indexed parameters */
    const char *expect;
};

#define NV 6
#define NI 8
static const float VPOS[NV][2] = {
    {0.05f,0.10f},{0.45f,0.10f},{0.05f,0.90f},     /* A */
    {0.45f,0.15f},{0.45f,0.95f},{0.07f,0.95f} };   /* B */
static const float VCOL[NV][4] = {
    {1,0,0,1},{0,1,0,1},{0,0,1,1},
    {1,1,0,1},{0,1,1,1},{1,0,1,1} };
static const uint16_t IDX[NV] = {0,1,2,3,4,5};
static void cell_origin(int i, float o[2]) {
    o[0] = -1.0f + 0.5f * (float)(i % 4);
    o[1] = -1.0f + 1.0f * (float)(i / 4);
}

int main(void){
    const char*cs=getenv("VBO_CASE"); if(!cs||!cs[0]) cs="d_a";
    struct tcase cases[] = {
      { "d_a",   0,0, {3,1,0,0}, {0}, "triangle A, cell 0" },
      { "i_a",   1,0, {3,1,0,0}, {0}, "indirect, same as d_a" },
      { "d_ab4", 0,0, {6,4,0,0}, {0}, "A and B in cells 0..3" },
      { "i_ab4", 1,0, {6,4,0,0}, {0}, "indirect" },
      { "d_fv",  0,0, {3,1,3,0}, {0}, "firstVertex 3: triangle B, cell 0" },
      { "i_fv",  1,0, {3,1,3,0}, {0}, "indirect" },
      { "d_fi",  0,0, {6,2,0,5}, {0}, "firstInstance 5: cells 5 and 6" },
      { "i_fi",  1,0, {6,2,0,5}, {0}, "indirect" },
      { "x_vo",  0,1, {0}, {3,1,0,3,0}, "indexed, vertexOffset 3: triangle B" },
      { "xi_vo", 1,1, {0}, {3,1,0,3,0}, "indexed indirect" },
      { "x_fi",  0,1, {0}, {6,3,0,0,2}, "indexed, firstInstance 2: cells 2..4" },
      { "xi_fi", 1,1, {0}, {6,3,0,0,2}, "indexed indirect" },
      /* all eight cells: 48 vertices against a placeholder vertexCount of 1 */
      { "d_all", 0,0, {6,8,0,0}, {0}, "A and B in all 8 cells" },
      { "i_all", 1,0, {6,8,0,0}, {0}, "indirect, 48 vertices vs placeholder 1" },
    };
    struct tcase*tc=NULL;
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++)
        if(!strcmp(cs,cases[i].name)) tc=&cases[i];
    if(!tc){printf("FAILED: unknown VBO_CASE=%s\n",cs);return 1;}
    const char*lie=getenv("VBO_LIE"); if(lie&&!lie[0]) lie=NULL;
    printf("=== case: %s (%s%s) ===\n",tc->name,tc->indirect?"indirect":"direct",
           tc->indexed?", indexed":"");
    if(tc->indexed)
        printf("params      : indexCount=%u instanceCount=%u firstIndex=%u vertexOffset=%d firstInstance=%u\n",
               tc->x.indexCount,tc->x.instanceCount,tc->x.firstIndex,tc->x.vertexOffset,tc->x.firstInstance);
    else
        printf("params      : vertexCount=%u instanceCount=%u firstVertex=%u firstInstance=%u\n",
               tc->c.vertexCount,tc->c.instanceCount,tc->c.firstVertex,tc->c.firstInstance);
    if(lie) printf("NEGATIVE CONTROL: VBO_LIE=%s, GPU data corrupted, reference is not\n",lie);
    printf("expectation : %s\n", tc->expect);
    printf("expectation : %s\n", tc->expect);
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
    PFN_vkCmdBindVertexBuffers CBVB=DP(CmdBindVertexBuffers);
    PFN_vkCmdPipelineBarrier CPB=DP(CmdPipelineBarrier);
    PFN_vkCreateBuffer CB=DP(CreateBuffer);
    PFN_vkGetBufferMemoryRequirements GBMR=DP(GetBufferMemoryRequirements);
    PFN_vkBindBufferMemory BBM=DP(BindBufferMemory);
    if(tc->indirect && !CDrawInd){
        printf("FAILED: vkCmdDrawIndirect is NULL\n"); return 1; }

    VkQueue q; GQ(dev,0,0,&q);
    VkFormat FMT=VK_FORMAT_R8G8B8A8_UNORM;

    VkBuffer ibuf=VK_NULL_HANDLE; VkDeviceMemory imem=VK_NULL_HANDLE;
    const uint32_t STRIDE = tc->indexed ? sizeof(VkDrawIndexedIndirectCommand)
                                        : sizeof(VkDrawIndirectCommand);
    if(tc->indirect){
        VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size=STRIDE,
            .usage=VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
        CHECK(CB(dev,&bi,NULL,&ibuf),"vkCreateBuffer(indirect)");
        VkMemoryRequirements mr; GBMR(dev,ibuf,&mr);
        uint32_t t=mtype(&mp,mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if(t==UINT32_MAX){printf("FAILED: no host-visible mem\n");return 1;}
        VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize=mr.size,.memoryTypeIndex=t};
        CHECK(AM(dev,&ma,NULL,&imem),"vkAllocateMemory(indirect)");
        CHECK(BBM(dev,ibuf,imem,0),"vkBindBufferMemory");
        void*p=NULL; CHECK(MM(dev,imem,0,STRIDE,0,&p),"map indirect");
        if(tc->indexed) memcpy(p,&tc->x,STRIDE); else memcpy(p,&tc->c,STRIDE);
        UM(dev,imem);
        printf("indirect buffer written, stride %u\n",STRIDE);
    }

    /* vertex buffers: binding 0 per-vertex {pos.xy, colour.rgba}, binding 1
     * per-instance {cell.xy}. One host-visible allocation each. */
    VkBuffer vbuf[2]; VkDeviceMemory vmem[2];
    const VkDeviceSize vsz[2] = { NV*6*sizeof(float), NI*2*sizeof(float) };
    for(int b=0;b<2;b++){
        VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size=vsz[b],.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
        CHECK(CB(dev,&bi,NULL,&vbuf[b]),"vkCreateBuffer(vertex)");
        VkMemoryRequirements vr; GBMR(dev,vbuf[b],&vr);
        uint32_t vt=mtype(&mp,vr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo va={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize=vr.size,.memoryTypeIndex=vt};
        CHECK(AM(dev,&va,NULL,&vmem[b]),"vkAllocateMemory(vertex)");
        CHECK(BBM(dev,vbuf[b],vmem[b],0),"vkBindBufferMemory(vertex)");
        float*f=NULL; CHECK(MM(dev,vmem[b],0,vsz[b],0,(void**)&f),"map vertex");
        if(b==0) for(int v=0;v<NV;v++){
            f[v*6+0]=VPOS[v][0]; f[v*6+1]=VPOS[v][1];
            for(int c=0;c<4;c++){
                int src = (lie&&!strcmp(lie,"color")) ? (c+1)%3 : c;
                f[v*6+2+c] = c==3 ? VCOL[v][3] : VCOL[v][src];
            }
        } else for(int i=0;i<NI;i++){
            float o[2];
            cell_origin((lie&&!strcmp(lie,"cell")) ? (i+1)%NI : i, o);
            f[i*2+0]=o[0]; f[i*2+1]=o[1];
        }
        UM(dev,vmem[b]);
    }
    printf("vertex buffers written: %u vertices, %u instance origins\n",NV,NI);

    /* index buffer: 6 indices {0,1,2,3,4,5} */
    VkBuffer xbuf=VK_NULL_HANDLE; VkDeviceMemory xmem=VK_NULL_HANDLE;
    if(tc->indexed){
        size_t xsz=NV*sizeof(uint16_t);
        VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size=xsz,.usage=VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
        CHECK(CB(dev,&bi,NULL,&xbuf),"vkCreateBuffer(index)");
        VkMemoryRequirements xr; GBMR(dev,xbuf,&xr);
        uint32_t xt=mtype(&mp,xr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if(xt==UINT32_MAX){printf("FAILED: no host-visible mem for index\n");return 1;}
        VkMemoryAllocateInfo xa={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize=xr.size,.memoryTypeIndex=xt};
        CHECK(AM(dev,&xa,NULL,&xmem),"vkAllocateMemory(index)");
        CHECK(BBM(dev,xbuf,xmem,0),"vkBindBufferMemory(index)");
        void*xp=NULL; CHECK(MM(dev,xmem,0,xsz,0,&xp),"map index");
        memcpy(xp,IDX,xsz);
        UM(dev,xmem);
        printf("index buffer written: {0,1,2,3,4,5}\n");
    }

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
    /* VBO_MULTI=1 selects the shader pair with four varying slots plus a flat
     * int. Output colour is identical, so the reference does not change. */
    const int multi = getenv("VBO_MULTI") && getenv("VBO_MULTI")[0]=='1';
    const char*vsn=getenv("VBO_VS"), *fsn=getenv("VBO_FS");
    if(!vsn||!vsn[0]) vsn=multi?"vbo_multi.vert.spv":"vbo.vert.spv";
    if(!fsn||!fsn[0]) fsn=multi?"vbo_multi.frag.spv":"vbo.frag.spv";
    char*vc=rf(vsn,&vz); if(!vc)return 1;
    char*fc=rf(fsn,&fz); if(!fc)return 1;
    printf("shader files: %s + %s\n",vsn,fsn);
    printf("shaders     : %s\n", multi?"vbo_multi (5 varyings)":"vbo (1 varying)");
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
    VkVertexInputBindingDescription vbd[2]={
        {0, 6*sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, 2*sizeof(float), VK_VERTEX_INPUT_RATE_INSTANCE}};
    VkVertexInputAttributeDescription vad[3]={
        {0,0,VK_FORMAT_R32G32_SFLOAT,0},
        {1,0,VK_FORMAT_R32G32B32A32_SFLOAT,2*sizeof(float)},
        {2,1,VK_FORMAT_R32G32_SFLOAT,0}};
    VkPipelineVertexInputStateCreateInfo vin={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount=2,.pVertexBindingDescriptions=vbd,
        .vertexAttributeDescriptionCount=3,.pVertexAttributeDescriptions=vad};
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
        .colorAttachmentCount=1,.pColorAttachmentFormats=&FMT};
    VkGraphicsPipelineCreateInfo gpi={
        .sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&pri,
        .stageCount=2,.pStages=st,.pVertexInputState=&vin,
        .pInputAssemblyState=&ia,.pViewportState=&vpi,
        .pRasterizationState=&rsi,.pMultisampleState=&msi,
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
    CPB(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,NULL,0,NULL,1,&br);
    VkRenderingAttachmentInfo at={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView=view,.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue={.color={.float32={0,0,0,1}}}};
    VkRenderingInfo ri={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea={{0,0},{DIMS,DIMS}},.layerCount=1,
        .colorAttachmentCount=1,.pColorAttachments=&at};
    CBR(cmd,&ri);
    VkViewport vp={0,0,(float)DIMS,(float)DIMS,0.0f,1.0f};
    VkRect2D sc={{0,0},{DIMS,DIMS}};
    CSV(cmd,0,1,&vp); CSS(cmd,0,1,&sc);
    CBP(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
    VkDeviceSize voffs[2]={0,0};
    CBVB(cmd,0,2,vbuf,voffs);
    if(tc->indexed){
        if(!CBindIB){printf("FAILED: vkCmdBindIndexBuffer NULL\n");return 1;}
        CBindIB(cmd,xbuf,0,VK_INDEX_TYPE_UINT16);
        printf("index buffer bound (UINT16)\n");
    }
    if(tc->indirect && tc->indexed){
        printf("calling vkCmdDrawIndexedIndirect\n");
        CDrawIdxInd(cmd,ibuf,0,1,STRIDE);
    } else if(tc->indirect){
        printf("calling vkCmdDrawIndirect\n");
        CDrawInd(cmd,ibuf,0,1,STRIDE);
    } else if(tc->indexed){
        printf("calling vkCmdDrawIndexed\n");
        CDrawIdx(cmd,tc->x.indexCount,tc->x.instanceCount,tc->x.firstIndex,
                 tc->x.vertexOffset,tc->x.firstInstance);
    } else {
        printf("calling vkCmdDraw\n");
        CDraw(cmd,tc->c.vertexCount,tc->c.instanceCount,tc->c.firstVertex,
              tc->c.firstInstance);
    }
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
    /* ---- CPU reference ---- */
    static float ref[DIMS][DIMS][3]; static int cov[DIMS][DIMS];
    memset(ref,0,sizeof(ref)); memset(cov,0,sizeof(cov));
    uint32_t vcount = tc->indexed ? tc->x.indexCount : tc->c.vertexCount;
    uint32_t icount = tc->indexed ? tc->x.instanceCount : tc->c.instanceCount;
    uint32_t finst  = tc->indexed ? tc->x.firstInstance : tc->c.firstInstance;
    double min_margin=1e9; uint32_t ambiguous=0;
    for(uint32_t in=finst; in<finst+icount; in++){
        float o[2]; cell_origin((int)in,o);
        for(uint32_t t=0;t+2<vcount;t+=3){
            int vi[3]; float px[3][2];
            for(int k=0;k<3;k++){
                vi[k] = tc->indexed ? (int)IDX[tc->x.firstIndex+t+k] + tc->x.vertexOffset
                                    : (int)(tc->c.firstVertex+t+k);
                px[k][0]=(o[0]+VPOS[vi[k]][0]+1.0f)*DIMS/2.0f;
                px[k][1]=(o[1]+VPOS[vi[k]][1]+1.0f)*DIMS/2.0f;
            }
            double area=(px[1][0]-px[0][0])*(px[2][1]-px[0][1])-(px[2][0]-px[0][0])*(px[1][1]-px[0][1]);
            for(int y=0;y<DIMS;y++) for(int x=0;x<DIMS;x++){
                double cx=x+0.5, cy=y+0.5, e[3];
                for(int k=0;k<3;k++){
                    const float*a=px[(k+1)%3],*b=px[(k+2)%3];
                    e[k]=((b[0]-a[0])*(cy-a[1])-(b[1]-a[1])*(cx-a[0]))/area;
                }
                int inside = e[0]>=0&&e[1]>=0&&e[2]>=0;
                /* distance in pixels from the centre to each edge line:
                 * |e_k| * |2*area| / |edge k|. A centre closer than 1e-3 px
                 * to an edge it could belong to is a tie and is not scored. */
                double dmin=1e9;
                for(int k=0;k<3;k++){
                    const float*a=px[(k+1)%3],*b=px[(k+2)%3];
                    double len=sqrt((b[0]-a[0])*(b[0]-a[0])+(b[1]-a[1])*(b[1]-a[1]));
                    double d=fabs(e[k])*fabs(area)/len;
                    if(d<dmin) dmin=d;
                }
                int near_inside = e[0]>=-1e-4&&e[1]>=-1e-4&&e[2]>=-1e-4;
                if(dmin<1e-3 && near_inside){ cov[y][x]=-1; ambiguous++; continue; }
                double m=dmin;
                if(!inside) continue;
                if(m<min_margin) min_margin=m;
                cov[y][x]=1;
                for(int c=0;c<3;c++)
                    ref[y][x][c]=e[0]*VCOL[vi[0]][c]+e[1]*VCOL[vi[1]][c]+e[2]*VCOL[vi[2]][c];
            }
        }
    }
    uint32_t nb=0, rc=0, bad_in=0, bad_out=0, maxd=0; int fx=-1,fy=-1;
    for(int y=0;y<DIMS;y++){uint8_t*row=base+y*sl.rowPitch;
        for(int x=0;x<DIMS;x++){uint8_t*p=row+x*4;
            if(p[0]||p[1]||p[2]) nb++;
            if(cov[y][x]<0) continue;
            if(cov[y][x]==1){
                rc++;
                for(int c=0;c<3;c++){
                    int want=(int)lrint(ref[y][x][c]*255.0); if(want<0)want=0; if(want>255)want=255;
                    int d=abs((int)p[c]-want); if((uint32_t)d>maxd) maxd=d;
                    if(d>2){bad_in++; if(fx<0){fx=x;fy=y;} break;}
                }
            } else if(p[0]||p[1]||p[2]){ bad_out++; if(fx<0){fx=x;fy=y;} }
        }}
    int ok = rc>0 && bad_in==0 && bad_out==0;
    printf("\n--- result: %s ---\n",tc->name);
    printf("drawn pixels        : %u\n",nb);
    printf("reference covered   : %u (+%u ambiguous, not scored, min scored margin %.4f px)\n",rc,ambiguous,min_margin);
    printf("colour mismatches   : %u (tolerance 2, max diff seen %u)\n",bad_in,maxd);
    printf("stray pixels        : %u\n",bad_out);
    if(fx>=0){uint8_t*p=base+fy*sl.rowPitch+fx*4;
        printf("first bad pixel     : (%d,%d) got %u,%u,%u\n",fx,fy,p[0],p[1],p[2]);}
    printf("VBOFP %s px=%u ref=%u bad=%u stray=%u maxd=%u verdict=%s\n",
           tc->name,nb,rc,bad_in,bad_out,maxd,ok?"PASS":"FAIL");

    const char*pp=getenv("VBO_PPM");
    if(pp&&pp[0]){
        FILE*f=fopen(pp,"wb");
        if(f){fprintf(f,"P6\n%d %d\n255\n",DIMS,DIMS);
            for(int y=0;y<DIMS;y++){uint8_t*row=base+y*sl.rowPitch;
                for(int x=0;x<DIMS;x++){uint8_t*p=row+x*4;
                    uint8_t rgb[3]={p[0],p[1],p[2]};fwrite(rgb,1,3,f);}}
            fclose(f);printf("VISUAL_DUMP: %s\n",pp);}
    }
    UM(dev,imgm);
    free(vc); free(fc);
    printf("DONE\n");
    return ok?0:2;
}
