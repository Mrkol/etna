#include "etna/ShaderProgram.hpp"
#include "StateTracking.hpp"
#include <etna/GlobalContext.hpp>
#include <etna/Etna.hpp>
#include <etna/Assert.hpp>
#include <etna/EtnaConfig.hpp>

#include <spdlog/fmt/ranges.h>

#include <unordered_set>
#include <vulkan/vulkan_structs.hpp>


namespace etna
{
  static vk::UniqueInstance createInstance(const InitParams &params)
  {
    vk::ApplicationInfo appInfo
      {
        .pApplicationName = params.applicationName,
        .applicationVersion = params.applicationVersion,
        .pEngineName = ETNA_ENGINE_NAME,
        .engineVersion = ETNA_VERSION,
        .apiVersion = VULKAN_API_VERSION
      };

    std::vector<const char*> extensions(
      params.instanceExtensions.begin(), params.instanceExtensions.end());
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    std::vector<const char*> layers(VALIDATION_LAYERS.begin(), VALIDATION_LAYERS.end());
    // Compatibility layer for devices that do not implement this extension natively.
    // Sync2 provides potential for driver optimization and a saner programmer API,
    // but is able to be translated into old synchronization calls if needed.
    layers.push_back("VK_LAYER_KHRONOS_synchronization2");

    vk::InstanceCreateInfo createInfo
      {
        .pApplicationInfo = &appInfo,
      };

    createInfo.setPEnabledLayerNames(layers);
    createInfo.setPEnabledExtensionNames(extensions);

    return vk::createInstanceUnique(createInfo).value;
  }

  static bool checkPhysicalDeviceSupportsExtensions(vk::PhysicalDevice pdevice, std::span<char const * const> extensions)
  {
    std::vector availableExtensions = pdevice.enumerateDeviceExtensionProperties().value;

    std::unordered_set requestedExtensions(extensions.begin(), extensions.end());
    for (const auto &ext : availableExtensions)
      requestedExtensions.erase(ext.extensionName);

    return requestedExtensions.empty();
  }

  static bool deviceTypeIsBetter(vk::PhysicalDeviceType first, vk::PhysicalDeviceType second)
  {
    auto score = [](vk::PhysicalDeviceType type)
      {
        switch (type)
        {
          case vk::PhysicalDeviceType::eVirtualGpu: return 1;
          case vk::PhysicalDeviceType::eIntegratedGpu: return 2;
          case vk::PhysicalDeviceType::eDiscreteGpu: return 3;
          default: return 0;
        }
      };
    return score(first) > score(second);
  }

  vk::PhysicalDevice pickPhysicalDevice(vk::Instance instance, const InitParams &params)
  {
    std::vector pdevices = instance.enumeratePhysicalDevices().value;

    ETNA_ASSERTF(!pdevices.empty(), "This PC has no GPUs that support Vulkan!");

    {
      std::vector<const char *> pdeviceNames;
      pdeviceNames.reserve(pdevices.size());
      for (auto pdevice : pdevices)
        pdeviceNames.emplace_back(pdevice.getProperties().deviceName);
      spdlog::info("List of physical devices: {}", pdeviceNames);
    }

    if (params.physicalDeviceIndexOverride)
    {
      ETNA_ASSERTF(*params.physicalDeviceIndexOverride < pdevices.size(),
        "There's no device with index {}!", *params.physicalDeviceIndexOverride);

      auto pdevice = pdevices[*params.physicalDeviceIndexOverride];

      if (checkPhysicalDeviceSupportsExtensions(pdevice, params.deviceExtensions))
      {
        return pdevice;
      }

      spdlog::error("Chosen physical device override '{}' does"
        " not support requested extensions! Falling back to automatic"
        " device selection.",
        pdevice.getProperties().deviceName);
    }

    auto bestDevice = pdevices.front();
    auto bestDeviceProps = pdevices.front().getProperties();
    for (auto pdevice : pdevices)
    {
      auto props = pdevice.getProperties();

      if (!checkPhysicalDeviceSupportsExtensions(pdevice, params.deviceExtensions))
        continue;

      if (deviceTypeIsBetter(props.deviceType, bestDeviceProps.deviceType))
      {
        bestDevice = pdevice;
        bestDeviceProps = props;
      }
    }

    return bestDevice;
  }

  uint32_t getQueueFamilyIndex(vk::PhysicalDevice pdevice, vk::QueueFlags flags)
  {
    std::vector queueFamilies = pdevice.getQueueFamilyProperties();

    for (uint32_t i = 0; i < queueFamilies.size(); ++i)
    {
      const auto &props = queueFamilies[i];

      if (props.queueCount > 0 && (props.queueFlags & flags))
        return i;
    }

    ETNA_PANIC("Could not find a queue family that supports all requested flags!");
  }

  static vk::UniqueDevice createDevice(vk::PhysicalDevice pdevice,
    uint32_t universalQueueFamily, const InitParams &params)
  {
    const float defaultQueuePriority {0.0f};

    // For now we use a single universal queue for everything.
    // Also, it's up to the framework to decide what queueus it needs and supports.

    const std::array queueInfos
      {
        vk::DeviceQueueCreateInfo
        {
          .queueFamilyIndex = universalQueueFamily,
          .queueCount = 1,
          .pQueuePriorities = &defaultQueuePriority,
        },
      };

    vk::PhysicalDeviceDynamicRenderingFeatures dynamic_rendering_feature {
      // Evil const cast due to C not having const
      .pNext = const_cast<vk::PhysicalDeviceFeatures2*>(&params.features),
      .dynamicRendering = VK_TRUE
    };

    vk::PhysicalDeviceSynchronization2Features sync2_feature {
      .pNext = &dynamic_rendering_feature,
      .synchronization2 = VK_TRUE
    };

    std::vector<char const *> deviceExtensions(params.deviceExtensions.begin(), params.deviceExtensions.end());
    deviceExtensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    deviceExtensions.push_back(VK_EXT_DEBUG_MARKER_EXTENSION_NAME);


    // NOTE: original design of PhysicalDeviceFeatures did not
    // support extensions, so they had to use a trick to achieve
    // extensions support. Now pNext has to point to a
    // PhysicalDeviceFeatures2 structure while the actual
    // pEnabledFeatures has to be nullptr.
    return pdevice.createDeviceUnique(
      vk::DeviceCreateInfo
      {
        .pNext = &sync2_feature,
        .queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size()),
        .pQueueCreateInfos = queueInfos.data(),
        .enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size()),
        .ppEnabledExtensionNames = deviceExtensions.data(),
      }).value;
  }

#ifndef NDEBUG
  static VkBool32 debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT /*messageType*/,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* /*pUserData*/)
  {
    if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
      spdlog::error(pCallbackData->pMessage);
    }
    else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
      spdlog::warn(pCallbackData->pMessage);
    }
    else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
    {
      spdlog::info(pCallbackData->pMessage);
    }
    else
    {
      spdlog::trace(pCallbackData->pMessage);
    }

    return VK_FALSE;
  }
#endif

  GlobalContext::GlobalContext(const InitParams &params)
  {
    // Proper initialization of vulkan is tricky, as we need to
    // dynamically link vulkan-1.dll and load symbols for various
    // extensions at runtime. Moreover, extensions can be device
    // specific and API version specific, so symbol loading is
    // done in 3 steps:
    // 1) load version-independent symbols
    // 2) load device-independent symbols
    // 3) load device-specific symbols
    vk::DynamicLoader dl;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(
      dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));

    vkInstance = createInstance(params);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkInstance.get());

    // NOTE: Previously we used VK_EXT_debug_report extension,
    // but it is considered to be abandoned in favour of
    // VK_EXT_debug_utils.
    #ifndef NDEBUG
      vkDebugCallback = vkInstance->createDebugUtilsMessengerEXTUnique(
        vk::DebugUtilsMessengerCreateInfoEXT
        {
          .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError
            | vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo
            | vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose
            | vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning,
          .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral
            | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance
            | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation,
          .pfnUserCallback = debugCallback,
          .pUserData = this,
        }).value;
    #endif

    vkPhysDevice = pickPhysicalDevice(vkInstance.get(), params);


    constexpr auto UNIVERSAL_QUEUE_FLAGS =
      vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute | vk::QueueFlagBits::eTransfer;
    universalQueueFamilyIdx = getQueueFamilyIndex(vkPhysDevice, UNIVERSAL_QUEUE_FLAGS);
    vkDevice = createDevice(vkPhysDevice, universalQueueFamilyIdx, params);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkDevice.get());

    universalQueue = vkDevice->getQueue(universalQueueFamilyIdx, 0);

    {
      // VmaVulkanFunctions vulkanFunctions {};
      // vulkanFunctions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
      // vulkanFunctions.vkGetDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

      VmaAllocatorCreateInfo alloc_info
        {
          .flags = {},
          .physicalDevice = vkPhysDevice,
          .device = vkDevice.get(),

          .preferredLargeHeapBlockSize = {},
          .pAllocationCallbacks = {},
          .pDeviceMemoryCallbacks = {},
          .pHeapSizeLimit = {},
          .pVulkanFunctions = {},

          .instance = vkInstance.get(),
          .vulkanApiVersion = VULKAN_API_VERSION,
          .pTypeExternalMemoryHandleTypes = nullptr
        };

      VmaAllocator allocator;
      ::vmaCreateAllocator(&alloc_info, &allocator);

      vmaAllocator = {allocator, &::vmaDestroyAllocator};
    }

    shaderPrograms.emplace(vkDevice.get(), descriptorSetLayouts);
    pipelineManager.emplace(vkDevice.get(), *shaderPrograms);
    descriptorPool.emplace(vkDevice.get(), params.numFramesInFlight);

    resourceTracking = std::make_unique<ResourceStates>();
  }

  Image GlobalContext::createImage(Image::CreateInfo info)
  {
    return Image(vmaAllocator.get(), info);
  }

  Buffer GlobalContext::createBuffer(Buffer::CreateInfo info)
  {
    return Buffer(vmaAllocator.get(), info);
  }

  ResourceStates &GlobalContext::getResourceTracker()
  {
    return *resourceTracking;
  }

  GlobalContext::~GlobalContext() = default;
}
