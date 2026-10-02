/* tex_test.c -- T5.1, texture sampling on Valhall v9 / JM.
 *
 * An 8x8 RGBA8 texture with a known pattern is uploaded with
 * vkCmdCopyBufferToImage (OPTIMAL by default, so AFBC/tiled on this driver;
 * TEX_TILING=linear for a LINEAR image), then sampled by a fragment shader at
 * uv = gl_FragCoord.xy * scale + off on a 64x64 LINEAR render target.
 * No varying is involved, so the sampled coordinate is known exactly.
 *
 * The CPU model implements the Vulkan spec filtering equations (nearest:
 * floor(u*W); linear: u*W-0.5, bilinear weights) and the address modes
 * REPEAT, MIRRORED_REPEAT, CLAMP_TO_EDGE, CLAMP_TO_BORDER (opaque black).
 * Sample points sit on multiples of 1/8 texel, so a 4-bit sub-texel
 * precision is enough; tolerance is 2 per channel (TEX_TOL).
 *
 * Negative control: TEX_LIE=1 makes the model use the other filter (and, for
 * nearest cases, the other address mode): MUST fail.
 * Select with TEX_CASE. TEX_PNG=path writes the rendered image.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <math.h>
#include "pngdump.h"
#include "vulkan/vulkan_core.h"

#define CHECK(e,m) do { VkResult _r=(e); \
    if(_r!=VK_SUCCESS){printf("FAILED: %s (VkResult=%d)\n",m,_r);return 1;} } while(0)
typedef PFN_vkVoidFunction (*PFN_icdGetInstanceProcAddr)(VkInstance,const char*);
#ifndef PANVK_DEFAULT_ICD_SO
#define PANVK_DEFAULT_ICD_SO \
 "/data/data/com.termux/files/home/panvk-g57/mesa/build/src/panfrost/vulkan/libvulkan_panfrost.so"
#endif
#define TW 8
#define RT 64

static uint32_t mtype(VkPhysicalDeviceMemoryProperties*mp,uint32_t b,VkMemoryPropertyFlags w){
    for(uint32_t i=0;i<mp->memoryTypeCount;i++)
        if((b&(1u<<i))&&(mp->memoryTypes[i].propertyFlags&w)==w) return i;
    return UINT32_MAX;
}
static char*rf(const char*p,size_t*s){
    FILE*f=fopen(p,"rb"); if(!f){printf("FAILED: open %s\n",p);return NULL;}
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    char*b=malloc(n); if(fread(b,1,n,f)!=(size_t)n){fclose(f);free(b);return NULL;}
    fclose(f);*s=(size_t)n;return b;
}

struct tcase { const char*name; VkFilter filter; VkSamplerAddressMode mode;
               float scale, off; };
static const struct tcase cases[] = {
    /* scale 1/32: 64 px cover uv 0..2, i.e. two copies of the texture */
    { "nearest_repeat",  VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT,          1/32.f, -0.5f },
    { "nearest_mirror",  VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, 1/32.f, -0.5f },
    { "nearest_clamp",   VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   1/32.f, -0.5f },
    { "nearest_border",  VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, 1/32.f, -0.5f },
    { "linear_repeat",   VK_FILTER_LINEAR,  VK_SAMPLER_ADDRESS_MODE_REPEAT,          1/32.f, -0.5f },
    { "linear_clamp",    VK_FILTER_LINEAR,  VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   1/32.f, -0.5f },
    { "linear_mirror",   VK_FILTER_LINEAR,  VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, 1/32.f, -0.5f },
    /* magnified 1:1 region: 64 px cover uv 0..1 exactly, centres on texels */
    { "nearest_exact",   VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT,          1/64.f,  0.0f },
};

static void texel(int i,int j,uint8_t o[4]){
    o[0]=(uint8_t)(i*32+16); o[1]=(uint8_t)(j*32+16);
    o[2]=((i^j)&1)?255:0;    o[3]=(uint8_t)(255-i*8-j);
}
/* returns -1 for border */
static int wrap(int c,VkSamplerAddressMode m){
    switch(m){
    case VK_SAMPLER_ADDRESS_MODE_REPEAT: return ((c%TW)+TW)%TW;
    case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: {
        int t=((c%(2*TW))+2*TW)%(2*TW); return t<TW?t:2*TW-1-t; }
    case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: return c<0?0:c>=TW?TW-1:c;
    default: return (c<0||c>=TW)?-1:c;
    }
}
static void fetch(int i,int j,VkSamplerAddressMode m,double o[4]){
    int a=wrap(i,m), b=wrap(j,m);
    if(a<0||b<0){ o[0]=o[1]=o[2]=0; o[3]=255; return; }   /* OPAQUE_BLACK */
    uint8_t t[4]; texel(a,b,t); for(int k=0;k<4;k++) o[k]=t[k];
}
static void sample(double u,double v,VkFilter f,VkSamplerAddressMode m,double o[4]){
    if(f==VK_FILTER_NEAREST){ fetch((int)floor(u*TW),(int)floor(v*TW),m,o); return; }
    double x=u*TW-0.5, y=v*TW-0.5; int i0=(int)floor(x), j0=(int)floor(y);
    double a=x-i0, b=y-j0, t00[4],t10[4],t01[4],t11[4];
    fetch(i0,j0,m,t00); fetch(i0+1,j0,m,t10); fetch(i0,j0+1,m,t01); fetch(i0+1,j0+1,m,t11);
    for(int k=0;k<4;k++)
        o[k]=(1-a)*(1-b)*t00[k]+a*(1-b)*t10[k]+(1-a)*b*t01[k]+a*b*t11[k];
}

int main(void){
    const char*cs=getenv("TEX_CASE"); if(!cs||!cs[0]) cs="nearest_repeat";
    const struct tcase*tc=NULL;
    for(unsigned i=0;i<sizeof cases/sizeof cases[0];i++) if(!strcmp(cs,cases[i].name)) tc=&cases[i];
    if(!tc){printf("FAILED: unknown TEX_CASE=%s\n",cs);return 1;}
    const int lie=getenv("TEX_LIE")&&getenv("TEX_LIE")[0]=='1';
    const int linear=getenv("TEX_TILING")&&!strcmp(getenv("TEX_TILING"),"linear");
    const int tol=getenv("TEX_TOL")?atoi(getenv("TEX_TOL")):2;
    printf("=== tex case: %s tiling=%s%s ===\n",tc->name,linear?"linear":"optimal",
           lie?" NEGATIVE CONTROL: model uses the other filter/mode":"");
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
    uint32_t n=1; VkPhysicalDevice pd; CHECK(EnumeratePhysicalDevices(inst,&n,&pd),"enum");
    VkPhysicalDeviceMemoryProperties mp; GetPhysicalDeviceMemoryProperties(pd,&mp);
    VkPhysicalDeviceVulkan13Features f13={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.dynamicRendering=VK_TRUE};
    float pr=1; VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&pr};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&f13,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
    VkDevice dev; CHECK(CreateDevice(pd,&di,NULL,&dev),"vkCreateDevice");
    #define DP(x) PFN_vk##x x=(PFN_vk##x)GetDeviceProcAddr(dev,"vk" #x)
    DP(GetDeviceQueue); DP(CreateCommandPool); DP(AllocateCommandBuffers); DP(BeginCommandBuffer);
    DP(EndCommandBuffer); DP(QueueSubmit); DP(QueueWaitIdle); DP(CreateImage); DP(GetImageMemoryRequirements);
    DP(GetImageSubresourceLayout); DP(AllocateMemory); DP(BindImageMemory); DP(CreateImageView); DP(MapMemory);
    DP(CreateShaderModule); DP(CreatePipelineLayout); DP(CreateGraphicsPipelines); DP(CmdBeginRendering);
    DP(CmdEndRendering); DP(CmdBindPipeline); DP(CmdSetViewport); DP(CmdSetScissor); DP(CmdDraw);
    DP(CmdPipelineBarrier); DP(CreateBuffer); DP(GetBufferMemoryRequirements); DP(BindBufferMemory);
    DP(CmdPushConstants); DP(CmdCopyBufferToImage); DP(CreateSampler); DP(CreateDescriptorSetLayout);
    DP(CreateDescriptorPool); DP(AllocateDescriptorSets); DP(UpdateDescriptorSets); DP(CmdBindDescriptorSets);
    VkQueue q; GetDeviceQueue(dev,0,0,&q);
    const VkFormat FMT=VK_FORMAT_R8G8B8A8_UNORM;
    const VkMemoryPropertyFlags HV=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    /* texture */
    VkImageCreateInfo tic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=FMT,
        .extent={TW,TW,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=linear?VK_IMAGE_TILING_LINEAR:VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkImage timg; CHECK(CreateImage(dev,&tic,NULL,&timg),"texture image");
    VkMemoryRequirements mr; GetImageMemoryRequirements(dev,timg,&mr);
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,
        .memoryTypeIndex=mtype(&mp,mr.memoryTypeBits,linear?HV:0)};
    VkDeviceMemory tmem; CHECK(AllocateMemory(dev,&ma,NULL,&tmem),"texture mem"); BindImageMemory(dev,timg,tmem,0);
    VkImageViewCreateInfo tvi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=timg,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=FMT,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkImageView tview; CHECK(CreateImageView(dev,&tvi,NULL,&tview),"texture view");
    /* staging buffer */
    VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=TW*TW*4,.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VkBuffer sb; CHECK(CreateBuffer(dev,&bc,NULL,&sb),"staging"); VkMemoryRequirements br; GetBufferMemoryRequirements(dev,sb,&br);
    VkMemoryAllocateInfo bma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=br.size,.memoryTypeIndex=mtype(&mp,br.memoryTypeBits,HV)};
    VkDeviceMemory sbm; CHECK(AllocateMemory(dev,&bma,NULL,&sbm),"staging mem"); BindBufferMemory(dev,sb,sbm,0);
    uint8_t*sp; MapMemory(dev,sbm,0,VK_WHOLE_SIZE,0,(void**)&sp);
    for(int j=0;j<TW;j++) for(int i=0;i<TW;i++) texel(i,j,sp+4*(j*TW+i));

    /* sampler + descriptor */
    VkSamplerCreateInfo smp={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=tc->filter,.minFilter=tc->filter,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=tc->mode,.addressModeV=tc->mode,.addressModeW=tc->mode,
        .maxLod=0,.borderColor=VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK};
    VkSampler sampler; CHECK(CreateSampler(dev,&smp,NULL,&sampler),"sampler");
    VkDescriptorSetLayoutBinding lb={0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL};
    VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&lb};
    VkDescriptorSetLayout dsl; CHECK(CreateDescriptorSetLayout(dev,&dl,NULL,&dsl),"set layout");
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};
    VkDescriptorPoolCreateInfo dpc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
    VkDescriptorPool dpool; CHECK(CreateDescriptorPool(dev,&dpc,NULL,&dpool),"pool");
    VkDescriptorSetAllocateInfo dsa={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=dpool,
        .descriptorSetCount=1,.pSetLayouts=&dsl};
    VkDescriptorSet dset; CHECK(AllocateDescriptorSets(dev,&dsa,&dset),"alloc set");
    VkDescriptorImageInfo dii={sampler,tview,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wd={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=dset,.descriptorCount=1,
        .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&dii};
    UpdateDescriptorSets(dev,1,&wd,0,NULL);

    /* render target, LINEAR host visible */
    VkImageCreateInfo ric=tic; ric.extent=(VkExtent3D){RT,RT,1}; ric.tiling=VK_IMAGE_TILING_LINEAR;
    ric.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImage rimg; CHECK(CreateImage(dev,&ric,NULL,&rimg),"rt"); GetImageMemoryRequirements(dev,rimg,&mr);
    VkMemoryAllocateInfo rma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=mtype(&mp,mr.memoryTypeBits,HV)};
    VkDeviceMemory rmem; CHECK(AllocateMemory(dev,&rma,NULL,&rmem),"rt mem"); BindImageMemory(dev,rimg,rmem,0);
    VkImageViewCreateInfo rvi=tvi; rvi.image=rimg; VkImageView rview; CHECK(CreateImageView(dev,&rvi,NULL,&rview),"rt view");

    /* pipeline */
    size_t vz=0,fz=0; char*vc=rf("fstri.vert.spv",&vz); char*fc=rf("tex.frag.spv",&fz); if(!vc||!fc) return 1;
    VkShaderModuleCreateInfo smi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=vz,.pCode=(uint32_t*)vc};
    VkShaderModule vs; CHECK(CreateShaderModule(dev,&smi,NULL,&vs),"vs");
    smi.codeSize=fz; smi.pCode=(uint32_t*)fc; VkShaderModule fs; CHECK(CreateShaderModule(dev,&smi,NULL,&fs),"fs");
    VkPushConstantRange pcr={VK_SHADER_STAGE_FRAGMENT_BIT,0,20};
    VkPipelineLayoutCreateInfo pli={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&dsl,
        .pushConstantRangeCount=1,.pPushConstantRanges=&pcr};
    VkPipelineLayout pl; CHECK(CreatePipelineLayout(dev,&pli,NULL,&pl),"layout");
    VkPipelineShaderStageCreateInfo st[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vs,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fs,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vin={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vpi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo rsi={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo msi={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba={.colorWriteMask=0xf};
    VkPipelineColorBlendStateCreateInfo cbs={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&cba};
    VkDynamicState dy[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyi={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=dy};
    VkPipelineRenderingCreateInfo pri={.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,.colorAttachmentCount=1,.pColorAttachmentFormats=&FMT};
    VkGraphicsPipelineCreateInfo gpi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&pri,.stageCount=2,.pStages=st,
        .pVertexInputState=&vin,.pInputAssemblyState=&ia,.pViewportState=&vpi,.pRasterizationState=&rsi,
        .pMultisampleState=&msi,.pColorBlendState=&cbs,.pDynamicState=&dyi,.layout=pl};
    VkPipeline pipe; CHECK(CreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gpi,NULL,&pipe),"pipeline");

    /* record: upload, transition, draw */
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool cp; CHECK(CreateCommandPool(dev,&cpi,NULL,&cp),"cmd pool");
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.commandBufferCount=1};
    VkCommandBuffer cmd; CHECK(AllocateCommandBuffers(dev,&cai,&cmd),"cb");
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(BeginCommandBuffer(cmd,&bi),"begin");
    VkImageMemoryBarrier b0={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=timg,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT};
    CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b0);
    VkBufferImageCopy rg={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={TW,TW,1}};
    CmdCopyBufferToImage(cmd,sb,timg,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&rg);
    VkImageMemoryBarrier b1=b0; b1.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b1.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b1.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b1.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    VkImageMemoryBarrier b2=b0; b2.image=rimg; b2.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b2.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkImageMemoryBarrier b12[2]={b1,b2};
    CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,NULL,0,NULL,2,b12);
    VkRenderingAttachmentInfo at={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=rview,
        .imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue={.color={.float32={1,0,1,1}}}};
    VkRenderingInfo ri={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{RT,RT}},.layerCount=1,
        .colorAttachmentCount=1,.pColorAttachments=&at};
    CmdBeginRendering(cmd,&ri);
    VkViewport vp={0,0,RT,RT,0,1}; VkRect2D sc={{0,0},{RT,RT}};
    CmdSetViewport(cmd,0,1,&vp); CmdSetScissor(cmd,0,1,&sc);
    CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
    CmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pl,0,1,&dset,0,NULL);
    float pc[5]={tc->scale,tc->scale,tc->off,tc->off,0};
    CmdPushConstants(cmd,pl,VK_SHADER_STAGE_FRAGMENT_BIT,0,20,pc);
    CmdDraw(cmd,3,1,0,0);
    CmdEndRendering(cmd);
    CHECK(EndCommandBuffer(cmd),"end");
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    CHECK(QueueSubmit(q,1,&si,VK_NULL_HANDLE),"submit"); CHECK(QueueWaitIdle(q),"wait");

    /* compare */
    VkImageSubresource sr={VK_IMAGE_ASPECT_COLOR_BIT,0,0}; VkSubresourceLayout sl; GetImageSubresourceLayout(dev,rimg,&sr,&sl);
    uint8_t*m; MapMemory(dev,rmem,0,VK_WHOLE_SIZE,0,(void**)&m); uint8_t*base=m+sl.offset;
    VkFilter mf=tc->filter; VkSamplerAddressMode mm=tc->mode;
    if(lie){ mf = mf==VK_FILTER_NEAREST?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
             if(tc->filter==VK_FILTER_NEAREST) mm = mm==VK_SAMPLER_ADDRESS_MODE_REPEAT?VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:VK_SAMPLER_ADDRESS_MODE_REPEAT; }
    uint32_t bad=0,magenta=0; int maxe=0,fx=-1,fy=-1; double fe[4]={0};
    for(int y=0;y<RT;y++) for(int x=0;x<RT;x++){
        const uint8_t*p=base+y*sl.rowPitch+x*4;
        if(p[0]==255&&p[1]==0&&p[2]==255&&p[3]==255) magenta++;
        double u=(x+0.5)*tc->scale+tc->off, v=(y+0.5)*tc->scale+tc->off, e[4];
        sample(u,v,mf,mm,e);
        int worst=0; for(int k=0;k<4;k++){int d=abs(p[k]-(int)lrint(e[k])); if(d>worst) worst=d;}
        if(worst>maxe) maxe=worst;
        if(worst>tol){ bad++; if(fx<0){fx=x;fy=y;memcpy(fe,e,sizeof fe);} }
    }
    int ok=bad==0;
    printf("result      : mismatching=%u of %d, max channel error=%d (tol %d), clear-colour pixels=%u\n",bad,RT*RT,maxe,tol,magenta);
    if(fx>=0){const uint8_t*p=base+fy*sl.rowPitch+fx*4;
        printf("  first     : (%d,%d) got %u,%u,%u,%u model %.1f,%.1f,%.1f,%.1f\n",fx,fy,p[0],p[1],p[2],p[3],fe[0],fe[1],fe[2],fe[3]);}
    printf("TEXFP %s tiling=%s bad=%u maxerr=%d verdict=%s\n",tc->name,linear?"linear":"optimal",bad,maxe,ok?"PASS":"FAIL");
    const char*pp=getenv("TEX_PNG");
    if(pp&&pp[0]){ FILE*f=png_open(pp,"wb"); if(f){ fprintf(f,"P6\n%d %d\n255\n",RT,RT);
        for(int y=0;y<RT;y++) for(int x=0;x<RT;x++) fwrite(base+y*sl.rowPitch+x*4,1,3,f); png_close(f);} }
    printf("DONE\n");
    return ok?0:2;
}
