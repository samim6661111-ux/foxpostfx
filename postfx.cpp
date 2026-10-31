// foxpostfx: post-process pass (sharpen / contrast / saturation / vignette)
// hooked in front of vkQueuePresentKHR. Untested on-device: expect to iterate.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
#include "dobby.h"
#include "vs_spv.h"
#include "fs_spv.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_ERROR 6
#define TAG "foxpostfx"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define FN_LIST(X) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkFreeDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateSampler) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) \
    X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyImage) X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) \
    X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) \
    X(vkQueueSubmit) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateSemaphore) X(vkDeviceWaitIdle) \
    X(vkGetSwapchainImagesKHR) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) \
    X(vkQueuePresentKHR) X(vkGetDeviceQueue) X(vkGetDeviceQueue2)

struct Fns {
#define X(n) PFN_##n n = nullptr;
    FN_LIST(X)
#undef X
} F;

static PFN_vkGetInstanceProcAddr real_gipa;
static PFN_vkGetDeviceProcAddr real_gdpa;
static PFN_vkCreateDevice real_cd;
static VkInstance g_inst;
static VkPhysicalDevice g_phys;
static VkDevice g_dev;
static std::recursive_mutex g_mu;
static std::map<VkQueue, uint32_t> g_qfam;

struct Cfg { int enabled = 1; float sharpen = 0.5f, contrast = 1.05f, saturation = 1.10f, vignette = 0.12f; };
static Cfg g_cfg;
static uint64_t g_frame;

static void load_cfg() {
    char pkg[256] = {};
    if (FILE* f = fopen("/proc/self/cmdline", "r")) { fread(pkg, 1, 255, f); fclose(f); }
    char path[512];
    snprintf(path, sizeof path, "/storage/emulated/0/Android/data/%s/files/foxpostfx.cfg", pkg);
    FILE* c = fopen(path, "r");
    if (!c) return;
    Cfg n; char line[128], k[32]; float v;
    while (fgets(line, sizeof line, c)) {
        if (sscanf(line, "%31[^=]=%f", k, &v) != 2) continue;
        if (!strcmp(k, "enabled")) n.enabled = (int)v;
        else if (!strcmp(k, "sharpen")) n.sharpen = v;
        else if (!strcmp(k, "contrast")) n.contrast = v;
        else if (!strcmp(k, "saturation")) n.saturation = v;
        else if (!strcmp(k, "vignette")) n.vignette = v;
    }
    fclose(c);
    g_cfg = n;
}

// ---------- global objects ----------
static bool g_init;
static VkSampler g_sampler;
static VkDescriptorSetLayout g_dsl;
static VkDescriptorPool g_pool;
static VkPipelineLayout g_pl;
static VkShaderModule g_vs, g_fs;

static bool init_global() {
    if (g_init) return true;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (F.vkCreateSampler(g_dev, &si, nullptr, &g_sampler)) return false;

    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1; dl.pBindings = &b;
    if (F.vkCreateDescriptorSetLayout(g_dev, &dl, nullptr, &g_dsl)) return false;

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = 8; dp.poolSizeCount = 1; dp.pPoolSizes = &ps;
    if (F.vkCreateDescriptorPool(g_dev, &dp, nullptr, &g_pool)) return false;

    VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1; pl.pSetLayouts = &g_dsl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
    if (F.vkCreatePipelineLayout(g_dev, &pl, nullptr, &g_pl)) return false;

    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof(vs_spv); sm.pCode = (const uint32_t*)vs_spv;
    if (F.vkCreateShaderModule(g_dev, &sm, nullptr, &g_vs)) return false;
    sm.codeSize = sizeof(fs_spv); sm.pCode = (const uint32_t*)fs_spv;
    if (F.vkCreateShaderModule(g_dev, &sm, nullptr, &g_fs)) return false;
    g_init = true;
    return true;
}

// ---------- per swapchain ----------
struct SC {
    VkFormat fmt; VkExtent2D ext;
    std::vector<VkImage> img; std::vector<VkImageView> view; std::vector<VkFramebuffer> fb;
    VkImage tmp = 0; VkDeviceMemory tmem = 0; VkImageView tview = 0;
    VkRenderPass rp = 0; VkPipeline pipe = 0; VkDescriptorSet ds = 0;
    VkCommandPool pool = 0;
    std::vector<VkCommandBuffer> cmd; std::vector<VkFence> fence; std::vector<VkSemaphore> sem;
    bool ok = false;
};
static std::map<VkSwapchainKHR, SC> g_sc;

static VkImageView mkview(VkImage im, VkFormat f) {
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.image = im; v.viewType = VK_IMAGE_VIEW_TYPE_2D; v.format = f;
    v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView out = 0;
    F.vkCreateImageView(g_dev, &v, nullptr, &out);
    return out;
}

static bool setup_sc(VkSwapchainKHR sc, const VkSwapchainCreateInfoKHR* ci) {
    if (!init_global()) return false;
    SC s; s.fmt = ci->imageFormat; s.ext = ci->imageExtent;
    uint32_t n = 0;
    F.vkGetSwapchainImagesKHR(g_dev, sc, &n, nullptr);
    s.img.resize(n);
    F.vkGetSwapchainImagesKHR(g_dev, sc, &n, s.img.data());

    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D; ic.format = s.fmt;
    ic.extent = {s.ext.width, s.ext.height, 1}; ic.mipLevels = 1; ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (F.vkCreateImage(g_dev, &ic, nullptr, &s.tmp)) return false;

    VkMemoryRequirements mr;
    F.vkGetImageMemoryRequirements(g_dev, s.tmp, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    ((PFN_vkGetPhysicalDeviceMemoryProperties)real_gipa(g_inst, "vkGetPhysicalDeviceMemoryProperties"))(g_phys, &mp);
    uint32_t mt = ~0u;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt = i; break; }
    if (mt == ~0u) return false;
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size; ma.memoryTypeIndex = mt;
    if (F.vkAllocateMemory(g_dev, &ma, nullptr, &s.tmem)) return false;
    if (F.vkBindImageMemory(g_dev, s.tmp, s.tmem, 0)) return false;
    s.tview = mkview(s.tmp, s.fmt);

    VkAttachmentDescription ad{};
    ad.format = s.fmt; ad.samples = VK_SAMPLE_COUNT_1_BIT;
    ad.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    ad.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ad.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &ar;
    VkRenderPassCreateInfo rc{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rc.attachmentCount = 1; rc.pAttachments = &ad; rc.subpassCount = 1; rc.pSubpasses = &sp;
    if (F.vkCreateRenderPass(g_dev, &rc, nullptr, &s.rp)) return false;

    for (VkImage im : s.img) {
        VkImageView v = mkview(im, s.fmt);
        s.view.push_back(v);
        VkFramebufferCreateInfo fc{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fc.renderPass = s.rp; fc.attachmentCount = 1; fc.pAttachments = &v;
        fc.width = s.ext.width; fc.height = s.ext.height; fc.layers = 1;
        VkFramebuffer fb = 0;
        if (F.vkCreateFramebuffer(g_dev, &fc, nullptr, &fb)) return false;
        s.fb.push_back(fb);
    }

    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = g_vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = g_fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb; gp.pDynamicState = &dy; gp.layout = g_pl; gp.renderPass = s.rp;
    if (F.vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &gp, nullptr, &s.pipe)) return false;

    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = g_pool; da.descriptorSetCount = 1; da.pSetLayouts = &g_dsl;
    if (F.vkAllocateDescriptorSets(g_dev, &da, &s.ds)) return false;
    VkDescriptorImageInfo di{g_sampler, s.tview, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = s.ds; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &di;
    F.vkUpdateDescriptorSets(g_dev, 1, &w, 0, nullptr);

    s.ok = true;
    g_sc[sc] = std::move(s);
    LOGI("swapchain ready %ux%u fmt=%d images=%u", ci->imageExtent.width, ci->imageExtent.height, (int)ci->imageFormat, n);
    return true;
}

static void destroy_sc(SC& s) {
    F.vkDeviceWaitIdle(g_dev);
    for (auto f : s.fb) F.vkDestroyFramebuffer(g_dev, f, nullptr);
    for (auto v : s.view) F.vkDestroyImageView(g_dev, v, nullptr);
    if (s.pipe) F.vkDestroyPipeline(g_dev, s.pipe, nullptr);
    if (s.rp) F.vkDestroyRenderPass(g_dev, s.rp, nullptr);
    if (s.ds) F.vkFreeDescriptorSets(g_dev, g_pool, 1, &s.ds);
    if (s.tview) F.vkDestroyImageView(g_dev, s.tview, nullptr);
    if (s.tmp) F.vkDestroyImage(g_dev, s.tmp, nullptr);
    if (s.tmem) F.vkFreeMemory(g_dev, s.tmem, nullptr);
    // command pool, fences and semaphores are leaked on purpose (no destroy fns loaded)
}

static bool ensure_cmds(SC& s, VkQueue q) {
    if (s.pool) return true;
    auto it = g_qfam.find(q);
    if (it == g_qfam.end()) return false;
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pc.queueFamilyIndex = it->second;
    if (F.vkCreateCommandPool(g_dev, &pc, nullptr, &s.pool)) return false;
    size_t n = s.img.size();
    s.cmd.resize(n); s.fence.resize(n); s.sem.resize(n);
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = s.pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = (uint32_t)n;
    if (F.vkAllocateCommandBuffers(g_dev, &ca, s.cmd.data())) return false;
    for (size_t i = 0; i < n; i++) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo se{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (F.vkCreateFence(g_dev, &fi, nullptr, &s.fence[i])) return false;
        if (F.vkCreateSemaphore(g_dev, &se, nullptr, &s.sem[i])) return false;
    }
    return true;
}

static void barrier(VkCommandBuffer cb, VkImage im, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = sa; b.dstAccessMask = da; b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    F.vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// ---------- hooked functions ----------
static VkResult VKAPI_CALL my_present(VkQueue q, const VkPresentInfoKHR* pi) {
    VkSemaphore outSem = VK_NULL_HANDLE;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if ((g_frame++ % 240) == 0) load_cfg();
        auto it = (pi->swapchainCount == 1) ? g_sc.find(pi->pSwapchains[0]) : g_sc.end();
        if (g_cfg.enabled && it != g_sc.end() && it->second.ok && ensure_cmds(it->second, q)) {
            SC& s = it->second;
            uint32_t idx = pi->pImageIndices[0];
            VkCommandBuffer cb = s.cmd[idx];
            F.vkWaitForFences(g_dev, 1, &s.fence[idx], VK_TRUE, UINT64_MAX);
            F.vkResetCommandBuffer(cb, 0);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            F.vkBeginCommandBuffer(cb, &bi);

            barrier(cb, s.img[idx], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            barrier(cb, s.tmp, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageCopy cp{};
            cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.extent = {s.ext.width, s.ext.height, 1};
            F.vkCmdCopyImage(cb, s.img[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             s.tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
            barrier(cb, s.tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            barrier(cb, s.img[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

            VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            rb.renderPass = s.rp; rb.framebuffer = s.fb[idx]; rb.renderArea = {{0, 0}, s.ext};
            F.vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
            F.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe);
            VkViewport vpt{0, 0, (float)s.ext.width, (float)s.ext.height, 0, 1};
            VkRect2D sc{{0, 0}, s.ext};
            F.vkCmdSetViewport(cb, 0, 1, &vpt);
            F.vkCmdSetScissor(cb, 0, 1, &sc);
            F.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl, 0, 1, &s.ds, 0, nullptr);
            float pc[4] = {g_cfg.sharpen, g_cfg.contrast, g_cfg.saturation, g_cfg.vignette};
            F.vkCmdPushConstants(cb, g_pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
            F.vkCmdDraw(cb, 3, 1, 0, 0);
            F.vkCmdEndRenderPass(cb);
            F.vkEndCommandBuffer(cb);

            std::vector<VkPipelineStageFlags> ws(pi->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.waitSemaphoreCount = pi->waitSemaphoreCount; si.pWaitSemaphores = pi->pWaitSemaphores;
            si.pWaitDstStageMask = ws.data();
            si.commandBufferCount = 1; si.pCommandBuffers = &cb;
            si.signalSemaphoreCount = 1; si.pSignalSemaphores = &s.sem[idx];
            F.vkResetFences(g_dev, 1, &s.fence[idx]);
            if (F.vkQueueSubmit(q, 1, &si, s.fence[idx]) == VK_SUCCESS) {
                outSem = s.sem[idx];
            } else {
                LOGE("submit failed, disabling postfx for this swapchain");
                s.ok = false;
                F.vkQueueSubmit(q, 0, nullptr, s.fence[idx]);  // re-signal the fence
            }
        }
    }
    if (outSem) {
        VkPresentInfoKHR p = *pi;
        p.waitSemaphoreCount = 1; p.pWaitSemaphores = &outSem;
        return F.vkQueuePresentKHR(q, &p);
    }
    return F.vkQueuePresentKHR(q, pi);
}

static VkResult VKAPI_CALL my_create_sc(VkDevice d, const VkSwapchainCreateInfoKHR* ci,
                                        const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
    VkSwapchainCreateInfoKHR m = *ci;
    m.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult r = F.vkCreateSwapchainKHR(d, &m, a, out);
    bool modified = true;
    if (r != VK_SUCCESS) { r = F.vkCreateSwapchainKHR(d, ci, a, out); modified = false; }
    if (r == VK_SUCCESS && modified) {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (!setup_sc(*out, ci)) LOGE("setup_sc failed, postfx off for this swapchain");
    }
    return r;
}

static void VKAPI_CALL my_destroy_sc(VkDevice d, VkSwapchainKHR sc, const VkAllocationCallbacks* a) {
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        auto it = g_sc.find(sc);
        if (it != g_sc.end()) { destroy_sc(it->second); g_sc.erase(it); }
    }
    F.vkDestroySwapchainKHR(d, sc, a);
}

static void VKAPI_CALL my_get_queue(VkDevice d, uint32_t fam, uint32_t idx, VkQueue* q) {
    F.vkGetDeviceQueue(d, fam, idx, q);
    if (q && *q) { std::lock_guard<std::recursive_mutex> lk(g_mu); g_qfam[*q] = fam; }
}

static void VKAPI_CALL my_get_queue2(VkDevice d, const VkDeviceQueueInfo2* qi, VkQueue* q) {
    F.vkGetDeviceQueue2(d, qi, q);
    if (q && *q) { std::lock_guard<std::recursive_mutex> lk(g_mu); g_qfam[*q] = qi->queueFamilyIndex; }
}

static void ensure_fns(VkDevice d) {
    if (!d || d == g_dev) return;
    g_dev = d;
#define X(n) F.n = (PFN_##n)real_gdpa(d, #n);
    FN_LIST(X)
#undef X
}

static PFN_vkVoidFunction wrap_dev(const char* n, PFN_vkVoidFunction real) {
    if (!strcmp(n, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)my_present;
    if (!strcmp(n, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)my_create_sc;
    if (!strcmp(n, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)my_destroy_sc;
    if (!strcmp(n, "vkGetDeviceQueue")) return (PFN_vkVoidFunction)my_get_queue;
    if (!strcmp(n, "vkGetDeviceQueue2")) return (PFN_vkVoidFunction)my_get_queue2;
    return real;
}

static VkResult VKAPI_CALL my_cd(VkPhysicalDevice p, const VkDeviceCreateInfo* ci,
                                 const VkAllocationCallbacks* a, VkDevice* out) {
    g_phys = p;
    VkResult r = real_cd(p, ci, a, out);
    if (r == VK_SUCCESS) { std::lock_guard<std::recursive_mutex> lk(g_mu); ensure_fns(*out); }
    return r;
}

static PFN_vkVoidFunction VKAPI_CALL my_gipa(VkInstance i, const char* n) {
    if (i) g_inst = i;
    PFN_vkVoidFunction r = real_gipa(i, n);
    if (!n || !r) return r;
    if (!strcmp(n, "vkCreateDevice")) { real_cd = (PFN_vkCreateDevice)r; return (PFN_vkVoidFunction)my_cd; }
    return wrap_dev(n, r);
}

static PFN_vkVoidFunction VKAPI_CALL my_gdpa(VkDevice d, const char* n) {
    if (d) { std::lock_guard<std::recursive_mutex> lk(g_mu); ensure_fns(d); }
    PFN_vkVoidFunction r = real_gdpa(d, n);
    if (!n || !r) return r;
    return wrap_dev(n, r);
}

extern "C" __attribute__((visibility("default"))) int JNI_OnLoad(void*, void*) {
    load_cfg();
    void* h = dlopen("libvulkan.so", RTLD_NOW);
    if (!h) { LOGE("libvulkan.so not found"); return 0x00010006; }
    void* gipa = dlsym(h, "vkGetInstanceProcAddr");
    void* gdpa = dlsym(h, "vkGetDeviceProcAddr");
    if (!gipa || !gdpa) { LOGE("vk entry points missing"); return 0x00010006; }
    DobbyHook(gipa, (dobby_dummy_func_t)my_gipa, (dobby_dummy_func_t*)&real_gipa);
    DobbyHook(gdpa, (dobby_dummy_func_t)my_gdpa, (dobby_dummy_func_t*)&real_gdpa);
    LOGI("hooks installed");
    return 0x00010006;
}
