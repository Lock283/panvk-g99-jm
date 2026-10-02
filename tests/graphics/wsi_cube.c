/* wsi_cube.c -- T4.9.3, spinning cube presented through VK_KHR_swapchain on
 * an X11 window (VK_KHR_xcb_surface), Valhall v9 / JM, kbase.
 *
 * Geometry, matrix and CPU model come from cube_test.c (CUBE_NO_MAIN). Each
 * frame: vkAcquireNextImageKHR -> render into the swapchain image -> 
 * vkQueuePresentKHR. Nothing in this program copies pixels to the window; the
 * driver's WSI does that.
 *
 * Check: every CUBE_CHECK_EVERY-th frame (default 30) the program reads the
 * window contents back from the X server (xcb_get_image) and compares them
 * with the CPU model of that frame. So what is verified is what is on screen.
 * The X readback happens after vkQueuePresentKHR returned and after a
 * vkQueueWaitIdle, and with FIFO the image shown can lag; with
 * CUBE_PRESENT=immediate|mailbox|fifo you pick the mode. The check accepts
 * the frame it was asked for only (no "previous frame" fallback), so a lagging
 * present shows up as a failure, not as a pass.
 *
 * Negative control: CUBE_LIE=1 checks the screen against the model of
 * angle + 3 degrees: MUST fail.
 * CUBE_FRAMES (600, 0 = forever), CUBE_STEP (1 degree), CUBE_DIM (1000).
 */
#define CUBE_NO_MAIN
#include "cube_test.c"
#define VK_USE_PLATFORM_XCB_KHR
#include <xcb/xcb.h>
#include "vulkan/vulkan.h"

int main(void){
    const int frames = getenv("CUBE_FRAMES") ? atoi(getenv("CUBE_FRAMES")) : 600;
    const double step = getenv("CUBE_STEP") ? atof(getenv("CUBE_STEP")) : 1.0;
    const int D = getenv("CUBE_DIM") ? atoi(getenv("CUBE_DIM")) : 1000;
    const int every = getenv("CUBE_CHECK_EVERY") ? atoi(getenv("CUBE_CHECK_EVERY")) : 30;
    const double band = getenv("CUBE_BAND") ? atof(getenv("CUBE_BAND")) : 0.5;
    const int lie = getenv("CUBE_LIE") && getenv("CUBE_LIE")[0]=='1';
    const char*pm_s = getenv("CUBE_PRESENT"); if(!pm_s) pm_s="fifo";
    VkPresentModeKHR want_pm = !strcmp(pm_s,"immediate")?VK_PRESENT_MODE_IMMEDIATE_KHR:
                               !strcmp(pm_s,"mailbox")?VK_PRESENT_MODE_MAILBOX_KHR:VK_PRESENT_MODE_FIFO_KHR;
    printf("=== wsi cube: frames=%d step=%.1f dim=%d present=%s check every %d%s ===\n",
           frames,step,D,pm_s,every,lie?" NEGATIVE CONTROL: model angle +3":"");
    fflush(stdout);

    /* X window */
    xcb_connection_t*xc=xcb_connect(NULL,NULL);
    if(xcb_connection_has_error(xc)){printf("FAILED: xcb_connect\n");return 1;}
    xcb_screen_t*scr=xcb_setup_roots_iterator(xcb_get_setup(xc)).data;
    xcb_window_t win=xcb_generate_id(xc);
    uint32_t vals[2]={scr->black_pixel,1};
    int wx=scr->width_in_pixels>D?(scr->width_in_pixels-D)/2:0, wy=scr->height_in_pixels>D?(scr->height_in_pixels-D)/2:0;
    xcb_create_window(xc,XCB_COPY_FROM_PARENT,win,scr->root,wx,wy,D,D,0,XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      scr->root_visual,XCB_CW_BACK_PIXEL|XCB_CW_OVERRIDE_REDIRECT,vals);
    xcb_map_window(xc,win); xcb_flush(xc);

    const char*icd=getenv("PANVK_ICD_SO"); if(!icd||!icd[0]) icd=PANVK_DEFAULT_ICD_SO;
    void*lib=dlopen(icd,RTLD_NOW); if(!lib){printf("dlopen: %s\n",dlerror());return 1;}
    PFN_icdGetInstanceProcAddr gpa=(PFN_icdGetInstanceProcAddr)dlsym(lib,"vk_icdGetInstanceProcAddr");
    PFN_vkCreateInstance CI=(PFN_vkCreateInstance)gpa(NULL,"vkCreateInstance");
    const char*iext[2]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_XCB_SURFACE_EXTENSION_NAME};
    VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai,
        .enabledExtensionCount=2,.ppEnabledExtensionNames=iext};
    VkInstance inst; CHECK(CI(&ii,NULL,&inst),"vkCreateInstance (surface extensions)");
    #define IP(n) PFN_vk##n n=(PFN_vk##n)gpa(inst,"vk" #n)
    IP(GetDeviceProcAddr); IP(EnumeratePhysicalDevices); IP(CreateDevice); IP(GetPhysicalDeviceMemoryProperties);
    IP(CreateXcbSurfaceKHR); IP(GetPhysicalDeviceSurfaceSupportKHR); IP(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    IP(GetPhysicalDeviceSurfaceFormatsKHR); IP(GetPhysicalDeviceSurfacePresentModesKHR);
    uint32_t n=1; VkPhysicalDevice pd; CHECK(EnumeratePhysicalDevices(inst,&n,&pd),"enum");
    VkPhysicalDeviceMemoryProperties mp; GetPhysicalDeviceMemoryProperties(pd,&mp);

    VkXcbSurfaceCreateInfoKHR xsi={.sType=VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,.connection=xc,.window=win};
    VkSurfaceKHR surf; CHECK(CreateXcbSurfaceKHR(inst,&xsi,NULL,&surf),"vkCreateXcbSurfaceKHR");
    VkBool32 sup=0; CHECK(GetPhysicalDeviceSurfaceSupportKHR(pd,0,surf,&sup),"surface support");
    VkSurfaceCapabilitiesKHR caps; CHECK(GetPhysicalDeviceSurfaceCapabilitiesKHR(pd,surf,&caps),"caps");
    uint32_t nf=0; GetPhysicalDeviceSurfaceFormatsKHR(pd,surf,&nf,NULL);
    VkSurfaceFormatKHR fmts[32]; if(nf>32)nf=32; GetPhysicalDeviceSurfaceFormatsKHR(pd,surf,&nf,fmts);
    uint32_t npm=0; GetPhysicalDeviceSurfacePresentModesKHR(pd,surf,&npm,NULL);
    VkPresentModeKHR pms[8]; if(npm>8)npm=8; GetPhysicalDeviceSurfacePresentModesKHR(pd,surf,&npm,pms);
    printf("surface     : supported=%u images %u..%u extent %ux%u usage 0x%x\n",sup,caps.minImageCount,
           caps.maxImageCount,caps.currentExtent.width,caps.currentExtent.height,caps.supportedUsageFlags);
    printf("formats     :"); for(uint32_t i=0;i<nf;i++) printf(" %d",fmts[i].format); printf("\n");
    printf("presentmodes:"); int have_pm=0; for(uint32_t i=0;i<npm;i++){printf(" %d",pms[i]); have_pm|=pms[i]==want_pm;} printf("\n");
    if(!sup){printf("FAILED: queue family 0 cannot present to this surface\n");return 1;}
    if(!have_pm){printf("FAILED: present mode %s not offered\n",pm_s);return 1;}
    VkFormat FMT=VK_FORMAT_UNDEFINED;
    for(uint32_t i=0;i<nf;i++) if(fmts[i].format==VK_FORMAT_B8G8R8A8_UNORM) FMT=fmts[i].format;
    for(uint32_t i=0;i<nf&&FMT==VK_FORMAT_UNDEFINED;i++) if(fmts[i].format==VK_FORMAT_R8G8B8A8_UNORM) FMT=fmts[i].format;
    if(FMT==VK_FORMAT_UNDEFINED){printf("FAILED: no UNORM 8-bit surface format\n");return 1;}
    const int bgra = FMT==VK_FORMAT_B8G8R8A8_UNORM;

    const char*dext[1]={VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan13Features f13={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.dynamicRendering=VK_TRUE};
    float pr=1; VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&pr};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&f13,.queueCreateInfoCount=1,
        .pQueueCreateInfos=&qi,.enabledExtensionCount=1,.ppEnabledExtensionNames=dext};
    VkDevice dev; CHECK(CreateDevice(pd,&di,NULL,&dev),"vkCreateDevice (swapchain)");
    #define DP(x) PFN_vk##x x=(PFN_vk##x)GetDeviceProcAddr(dev,"vk" #x)
    DP(GetDeviceQueue); DP(CreateCommandPool); DP(AllocateCommandBuffers); DP(BeginCommandBuffer);
    DP(EndCommandBuffer); DP(ResetCommandBuffer); DP(QueueSubmit); DP(QueueWaitIdle); DP(CreateFence); DP(WaitForFences);
    DP(ResetFences); DP(CreateSemaphore); DP(CreateImage); DP(GetImageMemoryRequirements); DP(AllocateMemory);
    DP(BindImageMemory); DP(CreateImageView); DP(MapMemory); DP(CreateShaderModule); DP(CreatePipelineLayout);
    DP(CreateGraphicsPipelines); DP(CmdBeginRendering); DP(CmdEndRendering); DP(CmdBindPipeline);
    DP(CmdSetViewport); DP(CmdSetScissor); DP(CmdDraw); DP(CmdPipelineBarrier); DP(CreateBuffer);
    DP(GetBufferMemoryRequirements); DP(BindBufferMemory); DP(CmdBindVertexBuffers); DP(CmdPushConstants);
    DP(CreateSwapchainKHR); DP(GetSwapchainImagesKHR); DP(AcquireNextImageKHR); DP(QueuePresentKHR);
    VkQueue q; GetDeviceQueue(dev,0,0,&q);

    uint32_t nimg_want = caps.minImageCount+1; if(caps.maxImageCount && nimg_want>caps.maxImageCount) nimg_want=caps.maxImageCount;
    VkSwapchainCreateInfoKHR sci={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,.surface=surf,
        .minImageCount=nimg_want,.imageFormat=FMT,.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent={D,D},.imageArrayLayers=1,.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,.preTransform=caps.currentTransform,
        .compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,.presentMode=want_pm,.clipped=VK_TRUE};
    VkSwapchainKHR sc; CHECK(CreateSwapchainKHR(dev,&sci,NULL,&sc),"vkCreateSwapchainKHR");
    uint32_t nimg=0; GetSwapchainImagesKHR(dev,sc,&nimg,NULL);
    VkImage simg[8]; if(nimg>8){printf("FAILED: %u images\n",nimg);return 1;} GetSwapchainImagesKHR(dev,sc,&nimg,simg);
    VkImageView sview[8];
    for(uint32_t i=0;i<nimg;i++){
        VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=simg[i],.viewType=VK_IMAGE_VIEW_TYPE_2D,
            .format=FMT,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        CHECK(CreateImageView(dev,&vi,NULL,&sview[i]),"swapchain view"); }
    printf("swapchain   : %u images, format %d\n",nimg,FMT);

    /* depth */
    const VkFormat DFMT=VK_FORMAT_D32_SFLOAT;
    VkImageCreateInfo dic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=DFMT,
        .extent={D,D,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT};
    VkImage dimg; CHECK(CreateImage(dev,&dic,NULL,&dimg),"depth");
    VkMemoryRequirements mr; GetImageMemoryRequirements(dev,dimg,&mr);
    VkMemoryAllocateInfo dma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=mtype(&mp,mr.memoryTypeBits,0)};
    VkDeviceMemory dmem; CHECK(AllocateMemory(dev,&dma,NULL,&dmem),"depth mem"); BindImageMemory(dev,dimg,dmem,0);
    VkImageViewCreateInfo dvi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=dimg,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=DFMT,.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1}};
    VkImageView dview; CHECK(CreateImageView(dev,&dvi,NULL,&dview),"depth view");

    /* vertices */
    struct vtx verts[36]; int nv=build_cube(verts);
    VkBuffer vb; VkDeviceMemory vbm; void*vmap;
    VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=sizeof verts,.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
    CHECK(CreateBuffer(dev,&bc,NULL,&vb),"vb"); VkMemoryRequirements r; GetBufferMemoryRequirements(dev,vb,&r);
    VkMemoryAllocateInfo vma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,
        .memoryTypeIndex=mtype(&mp,r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(AllocateMemory(dev,&vma,NULL,&vbm),"vb mem"); BindBufferMemory(dev,vb,vbm,0);
    MapMemory(dev,vbm,0,VK_WHOLE_SIZE,0,&vmap); memcpy(vmap,verts,sizeof verts);

    /* pipeline (same as cube_test.c) */
    size_t vz=0,fz=0; char*vc=rf("cube.vert.spv",&vz); char*fc=rf("cube.frag.spv",&fz); if(!vc||!fc) return 1;
    VkShaderModuleCreateInfo smi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=vz,.pCode=(uint32_t*)vc};
    VkShaderModule vs; CHECK(CreateShaderModule(dev,&smi,NULL,&vs),"vs");
    smi.codeSize=fz; smi.pCode=(uint32_t*)fc; VkShaderModule fs; CHECK(CreateShaderModule(dev,&smi,NULL,&fs),"fs");
    VkPushConstantRange pcr={VK_SHADER_STAGE_VERTEX_BIT,0,64};
    VkPipelineLayoutCreateInfo pli={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.pushConstantRangeCount=1,.pPushConstantRanges=&pcr};
    VkPipelineLayout pl; CHECK(CreatePipelineLayout(dev,&pli,NULL,&pl),"layout");
    VkPipelineShaderStageCreateInfo st[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vs,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fs,.pName="main"}};
    VkVertexInputBindingDescription vbd={0,sizeof(struct vtx),VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vad[2]={{0,0,VK_FORMAT_R32G32B32_SFLOAT,0},{1,0,VK_FORMAT_R32G32B32_SFLOAT,12}};
    VkPipelineVertexInputStateCreateInfo vin={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount=1,.pVertexBindingDescriptions=&vbd,.vertexAttributeDescriptionCount=2,.pVertexAttributeDescriptions=vad};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vpi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo rsi={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo msi={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo dsi={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable=1,.depthWriteEnable=1,.depthCompareOp=VK_COMPARE_OP_LESS};
    VkPipelineColorBlendAttachmentState cba={.colorWriteMask=0xf};
    VkPipelineColorBlendStateCreateInfo cbs={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&cba};
    VkDynamicState dy[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyi={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=dy};
    VkPipelineRenderingCreateInfo pri={.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount=1,.pColorAttachmentFormats=&FMT,.depthAttachmentFormat=DFMT};
    VkGraphicsPipelineCreateInfo gpi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&pri,.stageCount=2,.pStages=st,
        .pVertexInputState=&vin,.pInputAssemblyState=&ia,.pViewportState=&vpi,.pRasterizationState=&rsi,
        .pMultisampleState=&msi,.pDepthStencilState=&dsi,.pColorBlendState=&cbs,.pDynamicState=&dyi,.layout=pl};
    VkPipeline pipe; CHECK(CreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gpi,NULL,&pipe),"pipeline");

    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool cp; CHECK(CreateCommandPool(dev,&cpi,NULL,&cp),"pool");
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd; CHECK(AllocateCommandBuffers(dev,&cai,&cmd),"cb");
    VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; CHECK(CreateFence(dev,&fci,NULL,&fence),"fence");
    VkSemaphoreCreateInfo sci2={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acq,done; CHECK(CreateSemaphore(dev,&sci2,NULL,&acq),"sem"); CHECK(CreateSemaphore(dev,&sci2,NULL,&done),"sem");

    const uint8_t clear8[3]={32,32,40};
    struct model md={malloc((size_t)D*D*3),malloc((size_t)D*D)};
    uint64_t tot_bad=0; int checked=0,bad_frames=0; double t0=now_s(), tlast=t0; int flast=0;
    for(int fr=0; frames==0||fr<frames; fr++){
        double deg=fr*step; double m[16]; make_mvp(deg,m); float mf[16]; for(int i=0;i<16;i++) mf[i]=(float)m[i];
        uint32_t ix=0; VkResult ar=AcquireNextImageKHR(dev,sc,5000000000ull,acq,VK_NULL_HANDLE,&ix);
        if(ar!=VK_SUCCESS&&ar!=VK_SUBOPTIMAL_KHR){printf("FAILED: acquire frame %d -> %d\n",fr,ar);return 1;}
        CHECK(ResetCommandBuffer(cmd,0),"reset");
        VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        CHECK(BeginCommandBuffer(cmd,&bi),"begin");
        VkImageMemoryBarrier b0[2]={
            {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
             .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=simg[ix],
             .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
            {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
             .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=dimg,
             .subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1},.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT}};
        CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,0,0,NULL,0,NULL,2,b0);
        VkRenderingAttachmentInfo at={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=sview[ix],
            .imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue={.color={.float32={32/255.f,32/255.f,40/255.f,1}}}};
        VkRenderingAttachmentInfo dat={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=dview,
            .imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,.clearValue={.depthStencil={1,0}}};
        VkRenderingInfo ri={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{D,D}},.layerCount=1,
            .colorAttachmentCount=1,.pColorAttachments=&at,.pDepthAttachment=&dat};
        CmdBeginRendering(cmd,&ri);
        VkViewport vp={0,0,(float)D,(float)D,0,1}; VkRect2D sr={{0,0},{D,D}};
        CmdSetViewport(cmd,0,1,&vp); CmdSetScissor(cmd,0,1,&sr);
        CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
        VkDeviceSize off=0; CmdBindVertexBuffers(cmd,0,1,&vb,&off);
        CmdPushConstants(cmd,pl,VK_SHADER_STAGE_VERTEX_BIT,0,64,mf);
        CmdDraw(cmd,nv,1,0,0);
        CmdEndRendering(cmd);
        VkImageMemoryBarrier b1={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=simg[ix],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&b1);
        CHECK(EndCommandBuffer(cmd),"end");
        VkPipelineStageFlags ws=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,.pWaitSemaphores=&acq,.pWaitDstStageMask=&ws,
            .commandBufferCount=1,.pCommandBuffers=&cmd,.signalSemaphoreCount=1,.pSignalSemaphores=&done};
        CHECK(ResetFences(dev,1,&fence),"reset fence");
        CHECK(QueueSubmit(q,1,&si,fence),"submit");
        VkPresentInfoKHR pi={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,.pWaitSemaphores=&done,
            .swapchainCount=1,.pSwapchains=&sc,.pImageIndices=&ix};
        VkResult prr=QueuePresentKHR(q,&pi);
        if(prr!=VK_SUCCESS&&prr!=VK_SUBOPTIMAL_KHR){printf("FAILED: present frame %d -> %d\n",fr,prr);return 1;}
        VkResult wr=WaitForFences(dev,1,&fence,VK_TRUE,5000000000ull);
        if(wr!=VK_SUCCESS){printf("FAILED: fence frame %d -> %d\n",fr,wr);return 1;}

        if(every>0 && fr%every==0){
            CHECK(QueueWaitIdle(q),"wait idle");
            /* The software WSI path presents from its own thread (FIFO queue),
             * so the window can still show the previous image when
             * vkQueuePresentKHR returns. Give it CUBE_SETTLE_MS (default 100)
             * and do an X round trip before reading the window back. */
            { int ms=getenv("CUBE_SETTLE_MS")?atoi(getenv("CUBE_SETTLE_MS")):100;
              struct timespec ts={ms/1000,(ms%1000)*1000000L}; nanosleep(&ts,NULL);
              free(xcb_get_input_focus_reply(xc,xcb_get_input_focus(xc),NULL)); }
            xcb_get_image_reply_t*gr=xcb_get_image_reply(xc,xcb_get_image(xc,XCB_IMAGE_FORMAT_Z_PIXMAP,win,0,0,D,D,~0u),NULL);
            if(!gr){printf("FAILED: xcb_get_image\n");return 1;}
            const uint8_t*sp=xcb_get_image_data(gr); uint32_t len=xcb_get_image_data_length(gr);
            cpu_model(D,lie?deg+3.0:deg,verts,nv,1,band,clear8,&md);
            uint32_t bad=0,edge=0; int fx=-1,fy=-1;
            for(int y=0;y<D&&len>=(uint32_t)D*D*4;y++) for(int x=0;x<D;x++){
                const uint8_t*p=sp+((size_t)y*D+x)*4,*e=md.rgb+3*(y*D+x);
                /* X ZPixmap depth 24 on little endian: B,G,R,X */
                int d=abs(p[2]-e[0])>2||abs(p[1]-e[1])>2||abs(p[0]-e[2])>2;
                if(md.edge[y*D+x]){edge++;continue;}
                if(d){bad++; if(fx<0){fx=x;fy=y;}}
            }
            if(len<(uint32_t)D*D*4){printf("FAILED: short get_image %u\n",len);return 1;}
            int lag=-1; uint32_t lagbad=0;
            if(bad){ for(int k=1;k<=3&&lag<0;k++){
                cpu_model(D,(lie?deg+3.0:deg)-k*step,verts,nv,1,band,clear8,&md); uint32_t b2=0;
                for(int y=0;y<D;y++) for(int x=0;x<D;x++){ if(md.edge[y*D+x]) continue;
                    const uint8_t*p=sp+((size_t)y*D+x)*4,*e=md.rgb+3*(y*D+x);
                    b2+=abs(p[2]-e[0])>2||abs(p[1]-e[1])>2||abs(p[0]-e[2])>2; }
                if(b2==0){lag=k;} else if(k==1) lagbad=b2; } }
            checked++; tot_bad+=bad; bad_frames+=bad!=0;
            double tn=now_s();
            printf("WSIFP frame=%04d deg=%7.1f img=%u screen_bad=%u edge=%u fps=%.1f",fr,deg,ix,bad,edge,(fr-flast)/(tn-tlast));
            if(bad) printf(" shown=%s%d", lag>0?"frame-":"none-of-last-3 bad_vs_prev=", lag>0?lag:(int)lagbad);
            if(fx>=0){cpu_model(D,lie?deg+3.0:deg,verts,nv,1,band,clear8,&md);
                const uint8_t*p=sp+((size_t)fy*D+fx)*4,*e=md.rgb+3*(fy*D+fx);
                printf(" first=(%d,%d) screen=%u,%u,%u model=%u,%u,%u",fx,fy,p[2],p[1],p[0],e[0],e[1],e[2]);}
            printf("\n"); fflush(stdout); tlast=now_s(); flast=fr;
            free(gr);
        }
    }
    double wall=now_s()-t0;
    printf("\n--- result ---\nframes=%d in %.1f s = %.1f fps (incl. checks), checked=%d bad_frames=%d screen_bad=%llu\n",
           frames,wall,frames/wall,checked,bad_frames,(unsigned long long)tot_bad);
    int ok=checked>0&&tot_bad==0;
    printf("WSISUM present=%s format=%s frames=%d checked=%d bad_frames=%d bad=%llu verdict=%s\n",pm_s,bgra?"BGRA8":"RGBA8",
           frames,checked,bad_frames,(unsigned long long)tot_bad,ok?"PASS":"FAIL");
    printf("DONE\n");
    return ok?0:2;
}
