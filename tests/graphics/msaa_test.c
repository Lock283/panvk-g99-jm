/* msaa_test.c -- T4.7.4, multisampling and resolve on Valhall v9 / JM.
 *
 * One slanted white triangle (msaa.vert) is drawn into a 4x colour attachment
 * and resolved with VK_RESOLVE_MODE_AVERAGE_BIT into a 1x LINEAR host-visible
 * image, which is read back.
 *
 * The expected value of every pixel is computed, not written down: the CPU
 * counts how many of the four Vulkan standard sample positions
 * (0.375,0.125) (0.875,0.375) (0.125,0.625) (0.625,0.875) lie inside the
 * triangle, k = 0..4, and the resolved channel must be 255*k/4 within 1
 * (rounding of the average is implementation-defined). The device reports
 * standardSampleLocations = 1, which is what makes those positions binding.
 * Sample points closer than 1e-3 px to an edge are reported and the pixel is
 * not scored.
 *
 * Cases (MSAA_CASE):
 *   ms4        4x, resolve
 *   ms4_mask   4x with pSampleMask = 0x1: only sample 0 may be written, so the
 *              resolve can only be 0 or 255/4 per pixel
 *   ms1        1x drawn straight into the linear image. Control: the model then
 *              uses the single pixel-centre sample, and no intermediate value
 *              may appear
 * MSAA_LIE=1 makes the model use the pixel centre for a 4x render, so the
 * comparison MUST fail at the partially covered pixels.
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

static const float TRI[3][2] = {{-0.8f,-0.7f},{0.75f,-0.3f},{-0.2f,0.85f}};
static const float SPOS4[4][2] = {{0.375f,0.125f},{0.875f,0.375f},{0.125f,0.625f},{0.625f,0.875f}};
static const float SPOS1[1][2] = {{0.5f,0.5f}};

int main(void){
    const char*cs=getenv("MSAA_CASE"); if(!cs||!cs[0]) cs="ms4";
    const int ms4 = !strcmp(cs,"ms4"), ms4m = !strcmp(cs,"ms4_mask"), ms1 = !strcmp(cs,"ms1");
    if(!ms4 && !ms4m && !ms1){printf("FAILED: unknown MSAA_CASE=%s\n",cs);return 1;}
    const int lie = getenv("MSAA_LIE") && getenv("MSAA_LIE")[0]=='1';
    const VkSampleCountFlagBits NS = ms1 ? VK_SAMPLE_COUNT_1_BIT : VK_SAMPLE_COUNT_4_BIT;
    printf("=== case: %s (samples=%d%s) ===\n",cs,(int)NS,ms4m?", sampleMask 0x1":"");
    if(lie) printf("NEGATIVE CONTROL: model uses the pixel centre only\n");
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

    /* 4x colour attachment: optimal tiling, device memory (linear images cannot
     * be multisampled). The linear image below is the resolve target. */
    VkImage msimg=VK_NULL_HANDLE; VkImageView msview=VK_NULL_HANDLE;
    if(!ms1){
        VkImageCreateInfo mic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType=VK_IMAGE_TYPE_2D,.format=FMT,.extent={DIMS,DIMS,1},
            .mipLevels=1,.arrayLayers=1,.samples=NS,
            .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
        CHECK(CIM(dev,&mic,NULL,&msimg),"vkCreateImage(msaa)");
        VkMemoryRequirements mmr; GIMR(dev,msimg,&mmr);
        uint32_t mt2=mtype(&mp,mmr.memoryTypeBits,0);
        VkMemoryAllocateInfo mma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize=mmr.size,.memoryTypeIndex=mt2};
        VkDeviceMemory mmem; CHECK(AM(dev,&mma,NULL,&mmem),"vkAllocateMemory(msaa)");
        CHECK(BIM(dev,msimg,mmem,0),"vkBindImageMemory(msaa)");
        VkImageViewCreateInfo mvi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image=msimg,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=FMT,
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        CHECK(CIV(dev,&mvi,NULL,&msview),"vkCreateImageView(msaa)");
        printf("msaa image  : %llu bytes\n",(unsigned long long)mmr.size);
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
    char*vc=rf("msaa.vert.spv",&vz); if(!vc)return 1;
    char*fc=rf("white.frag.spv",&fz); if(!fc)return 1;
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
    const VkSampleMask smask = 0x1;
    VkPipelineMultisampleStateCreateInfo msi={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=NS,.pSampleMask=ms4m?&smask:NULL};
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
    VkImageMemoryBarrier mbr=br; mbr.image=msimg;
    VkImageMemoryBarrier brs[2]={br,mbr};
    CPB(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,NULL,0,NULL,ms1?1:2,brs);
    VkRenderingAttachmentInfo at={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView=view,.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue={.color={.float32={0,0,0,1}}}};
    if(!ms1){
        /* render into the 4x image, resolve into the linear 1x image */
        at.imageView=msview;
        at.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at.resolveMode=VK_RESOLVE_MODE_AVERAGE_BIT;
        at.resolveImageView=view;
        at.resolveImageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingInfo ri={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea={{0,0},{DIMS,DIMS}},.layerCount=1,
        .colorAttachmentCount=1,.pColorAttachments=&at};
    CBR(cmd,&ri);
    VkViewport vp={0,0,(float)DIMS,(float)DIMS,0.0f,1.0f};
    VkRect2D sc={{0,0},{DIMS,DIMS}};
    CSV(cmd,0,1,&vp); CSS(cmd,0,1,&sc);
    CBP(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
    CDraw(cmd,3,1,0,0);
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
    /* ---- CPU model: covered sample count per pixel ---- */
    float px[3][2];
    for(int k=0;k<3;k++){ px[k][0]=(TRI[k][0]+1.0f)*DIMS/2.0f; px[k][1]=(TRI[k][1]+1.0f)*DIMS/2.0f; }
    double area=(px[1][0]-px[0][0])*(px[2][1]-px[0][1])-(px[2][0]-px[0][0])*(px[1][1]-px[0][1]);
    const float (*sp)[2] = (ms1||lie) ? SPOS1 : SPOS4;
    const int nsp = (ms1||lie) ? 1 : 4;
    uint32_t hist_model[5]={0}, hist_got[256]={0}, bad=0, amb=0, partial=0; int fx=-1,fy=-1,fwant=0;
    double minmargin=1e9;
    for(int y=0;y<DIMS;y++){uint8_t*row=base+y*sl.rowPitch;
        for(int x=0;x<DIMS;x++){
            int k=0, tie=0;
            for(int si=0;si<nsp;si++){
                if(ms4m && si>0) break;            /* sample mask 0x1: only sample 0 */
                double cx=x+sp[si][0], cy=y+sp[si][1], e[3]; int in=1;
                for(int j=0;j<3;j++){
                    const float*a=px[(j+1)%3],*b=px[(j+2)%3];
                    e[j]=((b[0]-a[0])*(cy-a[1])-(b[1]-a[1])*(cx-a[0]))/area;
                    double len=sqrt((b[0]-a[0])*(b[0]-a[0])+(b[1]-a[1])*(b[1]-a[1]));
                    double d=fabs(e[j])*fabs(area)/len;
                    if(d<1e-3) tie=1; else if(d<minmargin) minmargin=d;
                    if(e[j]<0) in=0;
                }
                k+=in;
            }
            uint8_t*p=row+x*4; hist_got[p[0]]++;
            if(tie){amb++; continue;}
            /* resolved value: k of nsp samples set to 1.0. For the mask case the
             * other three samples keep the clear colour 0. */
            const int denom = ms1 ? 1 : 4;
            if(!ms1 && !lie) hist_model[k]++;
            const double want = 255.0*k/(lie?1:denom);
            if(k>0 && k<denom && !lie) partial++;
            int okp = 1;
            for(int c=0;c<3;c++) if(fabs((double)p[c]-want)>1.0) okp=0;
            if(!okp){bad++; if(fx<0){fx=x;fy=y;fwant=(int)lrint(want);}}
        }}
    int ok = bad==0;
    if(ms1){ for(int v=1;v<255;v++) if(hist_got[v]) ok=0; }
    printf("\n--- result: %s ---\n",cs);
    printf("model       : samples/pixel used=%d, partially covered pixels=%u, ambiguous=%u, min sample margin %.4f px\n",
           nsp,partial,amb,minmargin);
    if(!ms1 && !lie) printf("model k     : k0=%u k1=%u k2=%u k3=%u k4=%u\n",
           hist_model[0],hist_model[1],hist_model[2],hist_model[3],hist_model[4]);
    printf("got values  :");
    for(int v=0;v<256;v++) if(hist_got[v]) printf(" %d:%u",v,hist_got[v]);
    printf("\n");
    printf("mismatches  : %u (tolerance 1)\n",bad);
    if(fx>=0){uint8_t*p=base+fy*sl.rowPitch+fx*4;
        printf("  first     : (%d,%d) got %u,%u,%u model %d\n",fx,fy,p[0],p[1],p[2],fwant);}
    printf("MSAAFP %s bad=%u partial=%u verdict=%s\n",cs,bad,partial,ok?"PASS":"FAIL");

    const char*pp=getenv("MSAA_PPM");
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
