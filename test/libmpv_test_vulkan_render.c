/*
 * Minimal headless test for the mpv Vulkan render API.
 *
 * Creates a VkInstance + VkDevice, initializes an mpv render context
 * with the Vulkan/gpu-next backend, renders two frames into a VkImage,
 * and verifies no errors occurred.
 *
 * Exit codes: 0 = pass, 1 = fail, 77 = skip (no Vulkan hardware)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_vk.h>

#define CHECK_VK(call, msg) do {            \
    VkResult _r = (call);                   \
    if (_r != VK_SUCCESS) {                 \
        fprintf(stderr, "FAIL: %s (VkResult=%d)\n", (msg), _r); \
        ret = 1;                            \
        goto cleanup;                       \
    }                                       \
} while (0)

#define CHECK_MPV(call, msg) do {           \
    int _r = (call);                        \
    if (_r < 0) {                           \
        fprintf(stderr, "FAIL: %s (%s)\n", (msg), mpv_error_string(_r)); \
        ret = 1;                            \
        goto cleanup;                       \
    }                                       \
} while (0)

static uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t type_bits,
                                 VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    return UINT32_MAX;
}

int main(void)
{
    int ret = 0;

    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    mpv_handle *mpv = NULL;
    mpv_render_context *render_ctx = NULL;

    printf("=== mpv Vulkan render API test ===\n");

    // ---- VkInstance (headless, no extensions) ----
    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "mpv-vulkan-test",
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo instance_ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
    };
    CHECK_VK(vkCreateInstance(&instance_ci, NULL, &instance),
             "vkCreateInstance");
    printf("[OK] VkInstance created\n");

    // ---- Pick first physical device ----
    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(instance, &gpu_count, NULL);
    if (gpu_count == 0) {
        fprintf(stderr, "SKIP: no Vulkan physical devices\n");
        ret = 77;
        goto cleanup;
    }
    VkPhysicalDevice *gpus = calloc(gpu_count, sizeof(*gpus));
    vkEnumeratePhysicalDevices(instance, &gpu_count, gpus);
    VkPhysicalDevice physical_device = gpus[0];
    free(gpus);

    VkPhysicalDeviceProperties dev_props;
    vkGetPhysicalDeviceProperties(physical_device, &dev_props);
    printf("[OK] GPU: %s (Vulkan %u.%u)\n", dev_props.deviceName,
           VK_VERSION_MAJOR(dev_props.apiVersion),
           VK_VERSION_MINOR(dev_props.apiVersion));

    if (dev_props.apiVersion < VK_API_VERSION_1_2) {
        fprintf(stderr, "SKIP: GPU does not support Vulkan 1.2+\n");
        ret = 77;
        goto cleanup;
    }

    // ---- Find graphics queue family ----
    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &qf_count, NULL);
    VkQueueFamilyProperties *qf_props = calloc(qf_count, sizeof(*qf_props));
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &qf_count, qf_props);

    uint32_t graphics_qf = UINT32_MAX;
    for (uint32_t i = 0; i < qf_count; i++) {
        if (qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphics_qf = i;
            break;
        }
    }
    free(qf_props);

    if (graphics_qf == UINT32_MAX) {
        fprintf(stderr, "SKIP: no graphics queue family\n");
        ret = 77;
        goto cleanup;
    }

    // ---- Create VkDevice ----
    float queue_pri = 1.0f;
    VkDeviceQueueCreateInfo queue_ci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = graphics_qf,
        .queueCount = 1,
        .pQueuePriorities = &queue_pri,
    };

    VkPhysicalDeviceVulkan12Features vk12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .timelineSemaphore = VK_TRUE,
        .hostQueryReset = VK_TRUE,
    };
    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &vk12,
    };

    VkDeviceCreateInfo device_ci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features2,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_ci,
    };
    CHECK_VK(vkCreateDevice(physical_device, &device_ci, NULL, &device),
             "vkCreateDevice");

    VkQueue graphics_queue;
    vkGetDeviceQueue(device, graphics_qf, 0, &graphics_queue);
    printf("[OK] VkDevice created (queue family %u)\n", graphics_qf);

    // ---- mpv init ----
    mpv = mpv_create();
    if (!mpv) {
        fprintf(stderr, "FAIL: mpv_create()\n");
        ret = 1;
        goto cleanup;
    }
    CHECK_MPV(mpv_set_option_string(mpv, "vo", "libmpv"),  "set vo=libmpv");
    CHECK_MPV(mpv_set_option_string(mpv, "ao", "null"),     "set ao=null");
    CHECK_MPV(mpv_set_option_string(mpv, "idle", "yes"),    "set idle=yes");
    CHECK_MPV(mpv_set_option_string(mpv, "terminal", "yes"),"set terminal");
    CHECK_MPV(mpv_set_option_string(mpv, "msg-level", "all=v"), "set msg-level");
    CHECK_MPV(mpv_initialize(mpv), "mpv_initialize");
    printf("[OK] mpv initialized\n");

    // ---- Create Vulkan render context ----
    mpv_vulkan_init_params vk_params = {
        .instance = instance,
        .physical_device = physical_device,
        .device = device,
        .graphics_queue = graphics_queue,
        .graphics_queue_family = graphics_qf,
        .get_instance_proc_addr = vkGetInstanceProcAddr,
        .features = &features2,
    };

    char *backend = "gpu-next";
    mpv_render_param create_params[] = {
        {MPV_RENDER_PARAM_API_TYPE,            MPV_RENDER_API_TYPE_VULKAN},
        {MPV_RENDER_PARAM_VULKAN_INIT_PARAMS,  &vk_params},
        {MPV_RENDER_PARAM_BACKEND,             backend},
        {0}
    };
    CHECK_MPV(mpv_render_context_create(&render_ctx, mpv, create_params),
              "mpv_render_context_create");
    printf("[OK] Vulkan render context created (gpu-next)\n");

    // ---- Create target VkImage 320x240 RGBA8 ----
    const uint32_t W = 320, H = 240;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                            | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                            | VK_IMAGE_USAGE_SAMPLED_BIT
                            | VK_IMAGE_USAGE_STORAGE_BIT;

    VkImageCreateInfo image_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { W, H, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    CHECK_VK(vkCreateImage(device, &image_ci, NULL, &image), "vkCreateImage");

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device, image, &mem_reqs);

    uint32_t mem_type = find_memory_type(physical_device, mem_reqs.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX) {
        fprintf(stderr, "FAIL: no device-local memory type for image\n");
        ret = 1;
        goto cleanup;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type,
    };
    CHECK_VK(vkAllocateMemory(device, &alloc_info, NULL, &image_memory),
             "vkAllocateMemory");
    CHECK_VK(vkBindImageMemory(device, image, image_memory, 0),
             "vkBindImageMemory");
    printf("[OK] VkImage %ux%u RGBA8 allocated\n", W, H);

    // ---- Render frame 1 (image starts UNDEFINED) ----
    mpv_vulkan_fbo fbo = {
        .image = image,
        .width = W,
        .height = H,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = usage,
        .current_layout = VK_IMAGE_LAYOUT_UNDEFINED,
        .target_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    int flip_y = 0;
    int block_time = 0;
    mpv_render_param render_params[] = {
        {MPV_RENDER_PARAM_VULKAN_FBO,            &fbo},
        {MPV_RENDER_PARAM_FLIP_Y,                &flip_y},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block_time},
        {0}
    };

    CHECK_MPV(mpv_render_context_render(render_ctx, render_params),
              "render frame 1");
    CHECK_VK(vkDeviceWaitIdle(device), "vkDeviceWaitIdle after frame 1");
    printf("[OK] Frame 1 rendered\n");

    // ---- Render frame 2 (verifies state survived) ----
    fbo.current_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    CHECK_MPV(mpv_render_context_render(render_ctx, render_params),
              "render frame 2");
    CHECK_VK(vkDeviceWaitIdle(device), "vkDeviceWaitIdle after frame 2");
    printf("[OK] Frame 2 rendered\n");

    printf("\n=== ALL TESTS PASSED ===\n");

cleanup:
    if (render_ctx)
        mpv_render_context_free(render_ctx);
    if (mpv)
        mpv_destroy(mpv);
    // Vulkan resources freed after mpv is fully torn down
    if (image != VK_NULL_HANDLE)
        vkDestroyImage(device, image, NULL);
    if (image_memory != VK_NULL_HANDLE)
        vkFreeMemory(device, image_memory, NULL);
    if (device != VK_NULL_HANDLE)
        vkDestroyDevice(device, NULL);
    if (instance != VK_NULL_HANDLE)
        vkDestroyInstance(instance, NULL);

    return ret;
}
