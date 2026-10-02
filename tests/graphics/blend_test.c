/* blend_test.c -- T5.3, colour blending on Valhall v9 / JM.
 *
 * 64x64 RGBA8 UNORM LINEAR target cleared to CLR. Rect A = [8,40)^2 is drawn
 * with blending off (colour CA), then rect B = [24,56)^2 with the case's blend
 * state (colour CB). That gives four destination situations in one image:
 * clear only, A only, B over clear, B over A. Every pixel is compared with a
 * CPU model of the Vulkan blend equations (factors, ops incl. MIN/MAX, blend
 * constants, separate alpha, colour write mask), tolerance 1 per channel.
 *
 * Negative control: BLEND_LIE=1 makes the model swap the source and
 * destination colour factors; where both factors are the same it rotates the
 * op (ADD -> SUBTRACT -> REVERSE_SUBTRACT -> ADD); MIN and MAX are swapped;
 * for the blend-off write-mask case it ignores the mask. MUST fail.
 * BLEND_CASE selects the case, BLEND_PNG=path writes the image.
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
#define D 64
#define F(x) VK_BLEND_FACTOR_##x
#define O(x) VK_BLEND_OP_##x

struct bcase { const char*name; VkBlendFactor sc,dc; VkBlendOp cop;
               VkBlendFactor sa,da; VkBlendOp aop; uint32_t mask; int enable; };
static const struct bcase cases[] = {
  {"alpha",       F(SRC_ALPHA),F(ONE_MINUS_SRC_ALPHA),O(ADD), F(ONE),F(ONE_MINUS_SRC_ALPHA),O(ADD), 0xf,1},
  {"additive",    F(ONE),F(ONE),O(ADD),                         F(ONE),F(ONE),O(ADD),                 0xf,1},
  {"subtract",    F(ONE),F(ONE),O(SUBTRACT),                    F(ONE),F(ONE),O(SUBTRACT),            0xf,1},
  {"rev_subtract",F(ONE),F(ONE),O(REVERSE_SUBTRACT),            F(ONE),F(ONE),O(REVERSE_SUBTRACT),    0xf,1},
  {"min",         F(ONE),F(ONE),O(MIN),                         F(ONE),F(ONE),O(MIN),                 0xf,1},
  {"max",         F(ONE),F(ONE),O(MAX),                         F(ONE),F(ONE),O(MAX),                 0xf,1},
  {"multiply",    F(DST_COLOR),F(ZERO),O(ADD),                  F(DST_ALPHA),F(ZERO),O(ADD),          0xf,1},
  {"constant",    F(CONSTANT_COLOR),F(ONE_MINUS_CONSTANT_COLOR),O(ADD), F(CONSTANT_ALPHA),F(ONE_MINUS_CONSTANT_ALPHA),O(ADD), 0xf,1},
  {"separate",    F(SRC_ALPHA),F(ONE_MINUS_SRC_ALPHA),O(ADD),   F(ZERO),F(ONE),O(ADD),                0xf,1},
  {"dst_alpha",   F(ONE_MINUS_DST_ALPHA),F(DST_ALPHA),O(ADD),   F(ONE),F(ZERO),O(ADD),                0xf,1},
  {"saturate",    F(SRC_ALPHA_SATURATE),F(ONE),O(ADD),          F(ONE),F(ZERO),O(ADD),                0xf,1},
  {"write_mask",  F(ONE),F(ZERO),O(ADD),                        F(ONE),F(ZERO),O(ADD),                0x5,0},
  {"mask_blend",  F(SRC_ALPHA),F(ONE_MINUS_SRC_ALPHA),O(ADD),   F(ONE),F(ZERO),O(ADD),                0xa,1},
};
static const float CLR[4]={0.2f,0.4f,0.6f,0.8f};
static const float CA[4]={0.9f,0.1f,0.3f,0.5f};
static const float CB[4]={0.25f,0.75f,0.5f,0.6f};
static const float KC[4]={0.3f,0.6f,0.9f,0.4f};

static float q8(float v){ return roundf(fminf(fmaxf(v,0),1)*255.0f)/255.0f; }
static float fac(VkBlendFactor f,int ch,const float*s,const float*d){
    switch(f){
    case F(ZERO): return 0; case F(ONE): return 1;
    case F(SRC_COLOR): return s[ch]; case F(ONE_MINUS_SRC_COLOR): return 1-s[ch];
    case F(DST_COLOR): return d[ch]; case F(ONE_MINUS_DST_COLOR): return 1-d[ch];
    case F(SRC_ALPHA): return s[3]; case F(ONE_MINUS_SRC_ALPHA): return 1-s[3];
    case F(DST_ALPHA): return d[3]; case F(ONE_MINUS_DST_ALPHA): return 1-d[3];
    case F(CONSTANT_COLOR): return KC[ch]; case F(ONE_MINUS_CONSTANT_COLOR): return 1-KC[ch];
    case F(CONSTANT_ALPHA): return KC[3]; case F(ONE_MINUS_CONSTANT_ALPHA): return 1-KC[3];
    case F(SRC_ALPHA_SATURATE): return ch==3?1:fminf(s[3],1-d[3]);
    default: return 0; }
}
static void blend(const struct bcase*c,int lie,const float*s,float*d){
    float o[4];
    for(int ch=0;ch<4;ch++){
        int a=ch==3; VkBlendOp op=a?c->aop:c->cop;
        VkBlendFactor sf=a?c->sa:c->sc, df=a?c->da:c->dc;
        if(lie && !a && op!=O(MIN) && op!=O(MAX)){
            if(sf==df) /* swapping identical factors changes nothing: rotate the op */
                op = op==O(ADD)?O(SUBTRACT):op==O(SUBTRACT)?O(REVERSE_SUBTRACT):O(ADD);
            else { VkBlendFactor t=sf; sf=df; df=t; } }
        if(lie && (op==O(MIN)||op==O(MAX))) op = op==O(MIN)?O(MAX):O(MIN);
        float S=s[ch]*fac(sf,ch,s,d), Dv=d[ch]*fac(df,ch,s,d), r;
        if(!c->enable) r=s[ch];
        else switch(op){
            case O(ADD): r=S+Dv; break; case O(SUBTRACT): r=S-Dv; break;
            case O(REVERSE_SUBTRACT): r=Dv-S; break;
            case O(MIN): r=fminf(s[ch],d[ch]); break; case O(MAX): r=fmaxf(s[ch],d[ch]); break;
            default: r=S+Dv; }
        uint32_t m = lie && !c->enable ? 0xf : c->mask;
        o[ch] = (m>>ch&1) ? q8(r) : d[ch];
    }
    memcpy(d,o,sizeof o);
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
    const char*cs=getenv("BLEND_CASE"); if(!cs||!cs[0]) cs="alpha";
    const struct bcase*tc=NULL;
    for(unsigned i=0;i<sizeof cases/sizeof cases[0];i++) if(!strcmp(cs,cases[i].name)) tc=&cases[i];
    if(!tc){printf("FAILED: unknown BLEND_CASE=%s\n",cs);return 1;}
    const int lie=getenv("BLEND_LIE")&&getenv("BLEND_LIE")[0]=='1';
    printf("=== blend case: %s%s ===\n",tc->name,lie?" NEGATIVE CONTROL: model swaps src/dst factors":"");
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
    DP(CmdPipelineBarrier); DP(CmdPushConstants); DP(CmdSetBlendConstants);
    VkQueue q; GetDeviceQueue(dev,0,0,&q);
    const VkFormat FMT=VK_FORMAT_R8G8B8A8_UNORM;
    const VkMemoryPropertyFlags HV=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkImageCreateInfo ic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=FMT,.extent={D,D,1},
        .mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_LINEAR,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VkImage img; CHECK(CreateImage(dev,&ic,NULL,&img),"image"); VkMemoryRequirements mr; GetImageMemoryRequirements(dev,img,&mr);
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=mtype(&mp,mr.memoryTypeBits,HV)};
    VkDeviceMemory imem; CHECK(AllocateMemory(dev,&ma,NULL,&imem),"mem"); BindImageMemory(dev,img,imem,0);
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=img,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=FMT,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkImageView view; CHECK(CreateImageView(dev,&vi,NULL,&view),"view");
    size_t vz=0,fz=0; char*vc=rf("rect.vert.spv",&vz); char*fc=rf("rect.frag.spv",&fz); if(!vc||!fc) return 1;
    VkShaderModuleCreateInfo smi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=vz,.pCode=(uint32_t*)vc};
    VkShaderModule vs,fs; CHECK(CreateShaderModule(dev,&smi,NULL,&vs),"vs");
    smi.codeSize=fz; smi.pCode=(uint32_t*)fc; CHECK(CreateShaderModule(dev,&smi,NULL,&fs),"fs");
    VkPushConstantRange pcr={VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,36};
    VkPipelineLayoutCreateInfo pli={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.pushConstantRangeCount=1,.pPushConstantRanges=&pcr};
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
    VkDynamicState dy[3]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dyi={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=3,.pDynamicStates=dy};
    VkPipelineRenderingCreateInfo pri={.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,.colorAttachmentCount=1,.pColorAttachmentFormats=&FMT};
    VkPipeline pipe[2];
    for(int k=0;k<2;k++){
        VkPipelineColorBlendAttachmentState cba = k==0 ? (VkPipelineColorBlendAttachmentState){.colorWriteMask=0xf} :
            (VkPipelineColorBlendAttachmentState){.blendEnable=tc->enable,.srcColorBlendFactor=tc->sc,.dstColorBlendFactor=tc->dc,
             .colorBlendOp=tc->cop,.srcAlphaBlendFactor=tc->sa,.dstAlphaBlendFactor=tc->da,.alphaBlendOp=tc->aop,.colorWriteMask=tc->mask};
        VkPipelineColorBlendStateCreateInfo cbs={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&cba};
        VkGraphicsPipelineCreateInfo gpi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&pri,.stageCount=2,.pStages=st,
            .pVertexInputState=&vin,.pInputAssemblyState=&ia,.pViewportState=&vpi,.pRasterizationState=&rsi,
            .pMultisampleState=&msi,.pColorBlendState=&cbs,.pDynamicState=&dyi,.layout=pl};
        CHECK(CreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gpi,NULL,&pipe[k]),"pipeline"); }

    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool cp; CHECK(CreateCommandPool(dev,&cpi,NULL,&cp),"pool");
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.commandBufferCount=1};
    VkCommandBuffer cmd; CHECK(AllocateCommandBuffers(dev,&cai,&cmd),"cb");
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(BeginCommandBuffer(cmd,&bi),"begin");
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=img,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,NULL,0,NULL,1,&b);
    VkRenderingAttachmentInfo at={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=view,
        .imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue={.color={.float32={CLR[0],CLR[1],CLR[2],CLR[3]}}}};
    VkRenderingInfo ri={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{D,D}},.layerCount=1,.colorAttachmentCount=1,.pColorAttachments=&at};
    CmdBeginRendering(cmd,&ri);
    VkViewport vp={0,0,D,D,0,1}; VkRect2D sc={{0,0},{D,D}}; CmdSetViewport(cmd,0,1,&vp); CmdSetScissor(cmd,0,1,&sc);
    CmdSetBlendConstants(cmd,KC);
    float pa[9]={8,8,40,40, CA[0],CA[1],CA[2],CA[3], 0.5f}, pb[9]={24,24,56,56, CB[0],CB[1],CB[2],CB[3], 0.5f};
    CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe[0]);
    CmdPushConstants(cmd,pl,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,36,pa); CmdDraw(cmd,6,1,0,0);
    CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe[1]);
    CmdPushConstants(cmd,pl,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,36,pb); CmdDraw(cmd,6,1,0,0);
    CmdEndRendering(cmd); CHECK(EndCommandBuffer(cmd),"end");
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    CHECK(QueueSubmit(q,1,&si,VK_NULL_HANDLE),"submit"); CHECK(QueueWaitIdle(q),"wait");

    VkImageSubresource sr={VK_IMAGE_ASPECT_COLOR_BIT,0,0}; VkSubresourceLayout sl; GetImageSubresourceLayout(dev,img,&sr,&sl);
    uint8_t*m; MapMemory(dev,imem,0,VK_WHOLE_SIZE,0,(void**)&m); uint8_t*base=m+sl.offset;
    uint32_t bad=0; int maxe=0; const char*region[4]={"clear","A","B/clear","B/A"};
    for(int r=0;r<4;r++){
        int x=r==0?2:r==1?12:r==2?50:30, y=x;
        float d[4],sq[4]; for(int k=0;k<4;k++){ d[k]=q8(r==1||r==3?CA[k]:CLR[k]); sq[k]=CB[k]; }
        if(r>=2) blend(tc,lie,sq,d);
        const uint8_t*p=base+y*sl.rowPitch+x*4;
        printf("  %-8s (%2d,%2d) got %3u,%3u,%3u,%3u model %3.0f,%3.0f,%3.0f,%3.0f\n",region[r],x,y,p[0],p[1],p[2],p[3],
               d[0]*255,d[1]*255,d[2]*255,d[3]*255);
    }
    for(int y=0;y<D;y++) for(int x=0;x<D;x++){
        int inA=x>=8&&x<40&&y>=8&&y<40, inB=x>=24&&x<56&&y>=24&&y<56;
        float d[4],sq[4]; for(int k=0;k<4;k++){ d[k]=q8(inA?CA[k]:CLR[k]); sq[k]=CB[k]; }
        if(inB) blend(tc,lie,sq,d);
        const uint8_t*p=base+y*sl.rowPitch+x*4; int w=0;
        for(int k=0;k<4;k++){ int e=abs(p[k]-(int)lrintf(d[k]*255)); if(e>w) w=e; }
        if(w>maxe) maxe=w; if(w>1) bad++;
    }
    int ok=bad==0;
    printf("BLENDFP %s bad=%u maxerr=%d verdict=%s\n",tc->name,bad,maxe,ok?"PASS":"FAIL");
    const char*pp=getenv("BLEND_PNG");
    if(pp&&pp[0]){ FILE*f=png_open(pp,"wb"); if(f){ fprintf(f,"P6\n%d %d\n255\n",D,D);
        for(int y=0;y<D;y++) for(int x=0;x<D;x++) fwrite(base+y*sl.rowPitch+x*4,1,3,f); png_close(f);} }
    printf("DONE\n");
    return ok?0:2;
}
