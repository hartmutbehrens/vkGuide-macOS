﻿//> includes
#include <vk_engine.h>
#include <vk_images.h>

#include <SDL.h>
#include <SDL_vulkan.h>

#include <vk_initializers.h>
#include <vk_types.h>
#include <vk_pipelines.h>

//bootstrap library
#include "VkBootstrap.h"

#include <chrono>
#include <thread>

#include <glm/gtx/transform.hpp>

#define VMA_IMPLEMENTATION
//## -> token pasting operator. when used before __VA_ARGS__, it causes the preceding comma to be omitted if __VA_ARGS__ is empty.
//This allows the macro to compile correctly even when no additional arguments are provided.
//#define VMA_DEBUG_LOG(format, ...) do { printf(format, ##__VA_ARGS__); printf("\n"); } while(false)

#include "vk_mem_alloc.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"

VulkanEngine* loadedEngine = nullptr;

VulkanEngine& VulkanEngine::Get() { return *loadedEngine; }
constexpr bool bUseValidationLayers = true;

void VulkanEngine::init()
{
  // only one engine initialization is allowed with the application.
  assert(loadedEngine == nullptr);
  loadedEngine = this;

  VK_CHECK(volkInitialize());

  // We initialize SDL and create a window with it.
  SDL_Init(SDL_INIT_VIDEO);

  SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

  _window = SDL_CreateWindow(
    "Vulkan Engine",
    SDL_WINDOWPOS_UNDEFINED,
    SDL_WINDOWPOS_UNDEFINED,
    _windowExtent.width,
    _windowExtent.height,
    window_flags);

  init_vulkan();
  init_swapchain();
  init_commands();
  init_sync_structures();
  init_descriptors();
  init_pipelines();
  init_imgui();
  init_default_data();

  // everything went fine
  _isInitialized = true;
}

void VulkanEngine::immediate_submit(std::function<void(VkCommandBuffer cmd)>&& function)
{
  //TODO: this can be improved by submitting on a different queue to the graphics queue
  //TODO: That way the execution can be overlapped with the main render loop
  VK_CHECK(vkResetFences(_device, 1, &_immFence));
  VK_CHECK(vkResetCommandBuffer(_immCommandBuffer, 0));

  VkCommandBuffer cmd = _immCommandBuffer;

  VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

  VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));
  function(cmd);
  VK_CHECK(vkEndCommandBuffer(cmd));

  VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd);
  VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo, nullptr, nullptr);

  // submit command buffer to the queue and execute it.
  //  _renderFence will now block until the graphic commands finish execution
  VK_CHECK(vkQueueSubmit2KHR(_graphicsQueue, 1, &submit, _immFence));

  VK_CHECK(vkWaitForFences(_device, 1, &_immFence, true, 9999999999));
}

AllocatedBuffer VulkanEngine::create_buffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage)
{
  // allocate buffer
  VkBufferCreateInfo bufferInfo
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .pNext = nullptr,
    .size = allocSize,
    .usage = usage
  };

  VmaAllocationCreateInfo vmaallocInfo
  {
    .usage = memoryUsage,
    .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
  };

  AllocatedBuffer newBuffer{};
  // allocate the buffer
  VK_CHECK(vmaCreateBuffer(_allocator, &bufferInfo, &vmaallocInfo, &newBuffer.buffer, &newBuffer.allocation, &newBuffer.info));
  return newBuffer;
}

void VulkanEngine::create_swapchain(uint32_t width, uint32_t height)
{
  vkb::SwapchainBuilder swapchainBuilder{_chosenGPU, _device, _surface};

  _swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;

  vkb::Swapchain vkbSwapchain = swapchainBuilder
                                //.use_default_format_selection()
                                .set_desired_format(VkSurfaceFormatKHR{
                                  .format = _swapchainImageFormat, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
                                })
                                //use vsync present mode
                                .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                                .set_desired_extent(width, height)
                                .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                                .build()
                                .value();

  _swapchainExtent = vkbSwapchain.extent;
  //store swapchain and its related images
  _swapchain = vkbSwapchain.swapchain;
  _swapchainImages = vkbSwapchain.get_images().value();
  _swapchainImageViews = vkbSwapchain.get_image_views().value();
}

void VulkanEngine::destroy_buffer(const AllocatedBuffer& buffer)
{
  vmaDestroyBuffer(_allocator, buffer.buffer, buffer.allocation);
}

void VulkanEngine::destroy_swapchain()
{
  vkDestroySwapchainKHR(_device, _swapchain, nullptr);

  // destroy swapchain resources
  for (auto &swapchainImageView : _swapchainImageViews)
  {
    vkDestroyImageView(_device, swapchainImageView, nullptr);
  }
}

GPUMeshBuffers VulkanEngine::uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices)
{
  const size_t vertexBufferSize = vertices.size() * sizeof(Vertex);
  const size_t indexBufferSize = indices.size() * sizeof(uint32_t);

  GPUMeshBuffers newSurface{};

  //create vertex buffer
  newSurface.vertexBuffer = create_buffer(
    vertexBufferSize,
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    VMA_MEMORY_USAGE_GPU_ONLY);

  //find the adress of the vertex buffer
  VkBufferDeviceAddressInfo deviceAdressInfo
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
    .buffer = newSurface.vertexBuffer.buffer
  };
  // this exposes GPU virtual addresses ("GPU pointer") directly to the application !!
  // we can then send the pointer to the GPU and access it in the shader.
  // https://docs.vulkan.org/samples/latest/samples/extensions/buffer_device_address/README.html
  newSurface.vertexBufferAddress = vkGetBufferDeviceAddress(_device, &deviceAdressInfo);

  //create index buffer
  newSurface.indexBuffer = create_buffer(
    indexBufferSize,
    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    VMA_MEMORY_USAGE_GPU_ONLY);

  AllocatedBuffer staging = create_buffer(
    vertexBufferSize + indexBufferSize,
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    VMA_MEMORY_USAGE_CPU_ONLY);

  void* data = staging.allocation->GetMappedData();

  // copy vertex buffer
  memcpy(data, vertices.data(), vertexBufferSize);
  // copy index buffer
  memcpy((char*)data + vertexBufferSize, indices.data(), indexBufferSize);
  // this can be made more efficient by doing upload on a background thread
  immediate_submit([&](VkCommandBuffer cmd)
  {
    // now do "memcpy" on the GPU side from the staging buffer to vertex and index buffers
    VkBufferCopy vertexCopy
    {
      .dstOffset = 0,
      .srcOffset = 0,
      .size = vertexBufferSize
    };
    vkCmdCopyBuffer(cmd, staging.buffer, newSurface.vertexBuffer.buffer, 1, &vertexCopy);

    VkBufferCopy indexCopy
    {
      .dstOffset = 0,
      .srcOffset = vertexBufferSize,
      .size = indexBufferSize
    };
    vkCmdCopyBuffer(cmd, staging.buffer, newSurface.indexBuffer.buffer, 1, &indexCopy);
  });

  destroy_buffer(staging);
  _mainDeletionQueue.push_function([=]()
  {
    destroy_buffer(newSurface.indexBuffer);
    destroy_buffer(newSurface.vertexBuffer);
  });

  return newSurface;

}

void VulkanEngine::init_imgui()
{
  // 1: create descriptor pool for IMGUI
  //  the size of the pool is very oversize, but it's copied from imgui demo
  //  itself.
  VkDescriptorPoolSize pool_sizes[] = {
    {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
    {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
    {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
    {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000}
  };

  VkDescriptorPoolCreateInfo poolInfo =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    .maxSets = 1000,
    .poolSizeCount = static_cast<uint32_t>(std::size(pool_sizes)),
    .pPoolSizes = pool_sizes
  };

  VkDescriptorPool imguiPool;
  VK_CHECK(vkCreateDescriptorPool(_device, &poolInfo, nullptr, &imguiPool));

  // 2: initialize imgui library
  // this initializes the core structures of imgui
  ImGui::CreateContext();

  // this initializes imgui for SDL
  ImGui_ImplSDL2_InitForVulkan(_window);
  //ImGui_ImplVulkan_LoadFunctions(nullptr, nullptr);

  // this initializes imgui for Vulkan
  ImGui_ImplVulkan_InitInfo initInfo =
  {
    .Instance = _instance,
    .PhysicalDevice = _chosenGPU,
    .Device = _device,
    .Queue = _graphicsQueue,
    .DescriptorPool = imguiPool,
    .MinImageCount = 3,
    .ImageCount = 3,
    .UseDynamicRendering = true,
    .ColorAttachmentFormat = _swapchainImageFormat,
    .MSAASamples = VK_SAMPLE_COUNT_1_BIT
  };
  ImGui_ImplVulkan_Init(&initInfo, VK_NULL_HANDLE);

  // execute a gpu command to upload imgui font textures
  immediate_submit([&](VkCommandBuffer cmd)
  {
    ImGui_ImplVulkan_CreateFontsTexture(cmd);
  });

  // clear font textures from cpu data
  ImGui_ImplVulkan_DestroyFontUploadObjects();

  // add the destroy the imgui created structures
  _mainDeletionQueue.push_function([=]()
  {
    vkDestroyDescriptorPool(_device, imguiPool, nullptr);
    ImGui_ImplVulkan_Shutdown();
  });

}

void VulkanEngine::init_pipelines()
{
  //compute pipeline
  init_background_pipelines();
  //graphics pipeline
  init_mesh_pipeline();
}

void VulkanEngine::init_descriptors()
{
  //create a descriptor pool that will hold 10 sets with 1 image each
  std::vector<DescriptorAllocator::PoolSizeRatio> sizes =
  {
    { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 }
  };

  globalDescriptorAllocator.init_pool(_device, 10, sizes);

  //make the descriptor set layout for our compute draw
  {
    DescriptorLayoutBuilder builder;
    builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    _drawImageDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
  }
  //allocate a descriptor set for our draw image
  _drawImageDescriptors = globalDescriptorAllocator.allocate(_device,_drawImageDescriptorLayout);

  // VkDescriptorImageInfo imgInfo
  // {
  //   .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  //   .imageView = _drawImage.imageView
  // };
  //
  // VkWriteDescriptorSet drawImageWrite
  // {
  //   .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
  //   .pNext = nullptr,
  //
  //   .dstBinding = 0,
  //   .dstSet = _drawImageDescriptors,
  //   .descriptorCount = 1,
  //   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
  //   .pImageInfo = &imgInfo
  // };
  //
  // vkUpdateDescriptorSets(_device, 1, &drawImageWrite, 0, nullptr);
  DescriptorWriter writer;
  writer.write_image(0, _drawImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
  writer.update_set(_device,_drawImageDescriptors);

  {
    DescriptorLayoutBuilder builder;
    builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    _gpuSceneDataDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
  }

  //add to deletion queues
  _mainDeletionQueue.push_function([=]()
  {
    vkDestroyDescriptorSetLayout(_device, _drawImageDescriptorLayout, nullptr);
    vkDestroyDescriptorSetLayout(_device, _gpuSceneDataDescriptorLayout, nullptr);
    //globalDescriptorAllocator.clear_descriptors(_device);
    globalDescriptorAllocator.destroy_pool(_device);
  });

  for (auto & frame : _frames)
  {
    // create a descriptor pool
    std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> frame_sizes =
    {
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
    };

    frame._frameDescriptors = DescriptorAllocatorGrowable{};
    frame._frameDescriptors.init(_device, 1000, frame_sizes);

    _mainDeletionQueue.push_function([&]()
    {
      frame._frameDescriptors.destroy_pools(_device);
    });
  }
}

void VulkanEngine::init_vulkan()
{
  vkb::InstanceBuilder builder;
  // make the vulkan instance, with basic debug features
  auto inst_ret = builder.set_app_name("Example Vulkan Application")
                         .request_validation_layers(bUseValidationLayers)
                         .enable_validation_layers()
                         .use_default_debug_messenger()
                         .require_api_version(1, 2, 0)
                         .build();

  vkb::Instance vkb_inst = inst_ret.value();
  //grab the instance
  _instance = vkb_inst.instance;
  _debugMessenger = vkb_inst.debug_messenger;

  volkLoadInstance(_instance);

  SDL_Vulkan_CreateSurface(_window, _instance, &_surface);

  // vulkkan 1.3 features
  // VkPhysicalDeviceVulkan13Features features{};
  // features.dynamicRendering = true;
  // features.synchronization2 = true;

  VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRenderingFeatures
  {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR,
    .pNext = nullptr,
    .dynamicRendering = VK_TRUE
  };

  VkPhysicalDeviceSynchronization2FeaturesKHR synchronization2Features
  {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR,
    .pNext = &dynamicRenderingFeatures,  // load dynamic rendering extension next
    .synchronization2 = VK_TRUE  // Enable the synchronization2 feature
  };

  //vulkan 1.2 features
  VkPhysicalDeviceVulkan12Features features12
  {
    .bufferDeviceAddress = true,
    .descriptorIndexing = true,
    .pNext = &synchronization2Features  // load sync2 extension next
  };

  //use vkbootstrap to select a gpu.
  //We want a gpu that can write to the SDL surface and supports vulkan 1.3 with the correct features
  vkb::PhysicalDeviceSelector selector{vkb_inst};
  vkb::PhysicalDevice physicalDevice = selector
                                       .set_minimum_version(1, 2)
                                       .add_required_extension("VK_KHR_synchronization2")
                                       .add_required_extension("VK_KHR_dynamic_rendering")
                                       .add_required_extension("VK_KHR_copy_commands2")
                                       //.add_required_extension_features(&synchronization2Features)
                                       .set_required_features_12(features12)
                                       .set_surface(_surface)
                                       .select()
                                       .value();

  //create the final vulkan device
  vkb::DeviceBuilder deviceBuilder{physicalDevice};

  vkb::Device vkbDevice = deviceBuilder.build().value();

  // Get the VkDevice handle used in the rest of a vulkan application
  _device = vkbDevice.device;
  _chosenGPU = physicalDevice.physical_device;

  //volkLoadDevice(_device);

  // use vkbootstrap to get a Graphics queue
  _graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
  _graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

  // initialize the memory allocator
  VmaVulkanFunctions vulkanFunctions
  {
    .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
    .vkGetDeviceProcAddr = vkGetDeviceProcAddr
  };

  VmaAllocatorCreateInfo allocatorInfo
  {
    .physicalDevice = _chosenGPU,
    .device = _device,
    .instance = _instance,
    .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
    .pVulkanFunctions = &vulkanFunctions
  };
  vmaCreateAllocator(&allocatorInfo, &_allocator);

  _mainDeletionQueue.push_function([&]() {
      vmaDestroyAllocator(_allocator);
  });
}

void VulkanEngine::init_swapchain()
{
  create_swapchain(_windowExtent.width, _windowExtent.height);
  //draw image size will match the window
  const VkExtent3D drawImageExtent =
  {
    _windowExtent.width,
    _windowExtent.height,
    1
  };

  //hardcoding the draw format to 32 bit float
  _drawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  _drawImage.imageExtent = drawImageExtent;

  VkImageUsageFlags drawImageUsages{};
  drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;
  drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

  const VkImageCreateInfo rimg_info = vkinit::image_create_info(_drawImage.imageFormat, drawImageUsages, drawImageExtent);

  //for the draw image, we want to allocate it from gpu local memory
  VmaAllocationCreateInfo rimg_allocinfo
  {
    .usage = VMA_MEMORY_USAGE_GPU_ONLY,
    .requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
  };

  //allocate and create the image
  vmaCreateImage(_allocator, &rimg_info, &rimg_allocinfo, &_drawImage.image, &_drawImage.allocation, nullptr);

  //build a image-view for the draw image to use for rendering
  const VkImageViewCreateInfo rview_info = vkinit::imageview_create_info(_drawImage.imageFormat, _drawImage.image, VK_IMAGE_ASPECT_COLOR_BIT);

  VK_CHECK(vkCreateImageView(_device, &rview_info, nullptr, &_drawImage.imageView));

  _depthImage.imageFormat = VK_FORMAT_D32_SFLOAT;
  _depthImage.imageExtent = drawImageExtent;
  VkImageUsageFlags depthImageUsages{};
  depthImageUsages |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

  VkImageCreateInfo dimg_info = vkinit::image_create_info(_depthImage.imageFormat, depthImageUsages, drawImageExtent);

  //allocate and create the image
  vmaCreateImage(_allocator, &dimg_info, &rimg_allocinfo, &_depthImage.image, &_depthImage.allocation, nullptr);

  //build a image-view for the draw image to use for rendering
  VkImageViewCreateInfo dview_info = vkinit::imageview_create_info(_depthImage.imageFormat, _depthImage.image, VK_IMAGE_ASPECT_DEPTH_BIT);

  VK_CHECK(vkCreateImageView(_device, &dview_info, nullptr, &_depthImage.imageView));

  //add to deletion queues
  _mainDeletionQueue.push_function([=]()
  {
    vkDestroyImageView(_device, _drawImage.imageView, nullptr);
    vmaDestroyImage(_allocator, _drawImage.image, _drawImage.allocation);

    vkDestroyImageView(_device, _depthImage.imageView, nullptr);
    vmaDestroyImage(_allocator, _depthImage.image, _depthImage.allocation);
  });
}

void VulkanEngine::init_commands()
{
  //create a command pool for commands submitted to the graphics queue.
  //we also want the pool to allow for resetting of individual command buffers
  VkCommandPoolCreateInfo commandPoolInfo = vkinit::command_pool_create_info(
    _graphicsQueueFamily, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

  for (auto& frame : _frames)
  {
    //per-frame command pool for rendering
    VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &frame._commandPool));
    // allocate the default command buffer that we will use for rendering
    VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(frame._commandPool, 1);
    VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &frame._mainCommandBuffer));
  }

  // immediate submit command pool
  VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_immCommandPool));
  // allocate the command buffer for immediate submits
  VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_immCommandPool, 1);
  VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_immCommandBuffer));
  _mainDeletionQueue.push_function([=]()
  {
    vkDestroyCommandPool(_device, _immCommandPool, nullptr);
  });
}

void VulkanEngine::init_sync_structures()
{
  //create syncronization structures
  //one fence to control when the gpu has finished rendering the frame,
  //and 2 semaphores to syncronize rendering with swapchain
  //we want the fence to start signalled so we can wait on it on the first frame
  VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
  VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();

  for (auto & frame : _frames)
  {
    //per-frame render fence for cpu-gpu synchronization
    VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &frame._renderFence));
    //wait semaphore
    VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &frame._swapchainSemaphore));
    //signal semaphore
    VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &frame._renderSemaphore));
  }
  //immediate submit fence for cpu-gpu synchronizatin (notify on GPU completing operation)
  VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &_immFence));
  _mainDeletionQueue.push_function([=]() { vkDestroyFence(_device, _immFence, nullptr); });
}

void VulkanEngine::init_background_pipelines()
{
  VkPushConstantRange pushConstant
  {
    .offset = 0,
    .size = sizeof(ComputePushConstants),
    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
  };

  VkPipelineLayoutCreateInfo computeLayout
  {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .pNext = nullptr,
    .pSetLayouts = &_drawImageDescriptorLayout,
    .setLayoutCount = 1,
    .pPushConstantRanges = &pushConstant,
    .pushConstantRangeCount = 1
  };

  VK_CHECK(vkCreatePipelineLayout(_device, &computeLayout, nullptr, &_gradientPipelineLayout));

  VkShaderModule gradientShader;
  if (!vkutil::load_shader_module("../shaders/gradient_color.comp.spv", _device, &gradientShader))
  {
    fmt::print("Error when building the gradient compute shader \n");
  }

  VkShaderModule skyShader;
  if (!vkutil::load_shader_module("../shaders/sky.comp.spv", _device, &skyShader))
  {
    fmt::print("Error when building the sky compute shader \n");
  }

  VkPipelineShaderStageCreateInfo stageinfo
  {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
    .pNext = nullptr,
    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
    .module = gradientShader,
    .pName = "main"
  };

  VkComputePipelineCreateInfo computePipelineCreateInfo
  {
    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
    .pNext = nullptr,
    .layout = _gradientPipelineLayout,
    .stage = stageinfo
  };

  ComputeEffect gradient
  {
    .layout = _gradientPipelineLayout,
    .name = "gradient",
    .data = {}
  };
  //default colors
  gradient.data.data1 = glm::vec4(1, 0, 0, 1);
  gradient.data.data2 = glm::vec4(0, 0, 1, 1);

  VK_CHECK(vkCreateComputePipelines(_device,VK_NULL_HANDLE,1,&computePipelineCreateInfo, nullptr, &gradient.pipeline));

  //change the shader module only to create the sky shader
  computePipelineCreateInfo.stage.module = skyShader;

  ComputeEffect sky
  {
    .layout = _gradientPipelineLayout,
    .name = "sky",
    .data = {}
  };
  //default sky parameters
  sky.data.data1 = glm::vec4(0.1, 0.2, 0.4 ,0.97);

  VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &sky.pipeline));

  //add the 2 background effects into the array
  backgroundEffects.push_back(gradient);
  backgroundEffects.push_back(sky);

  vkDestroyShaderModule(_device, gradientShader, nullptr);
  vkDestroyShaderModule(_device, skyShader, nullptr);

  _mainDeletionQueue.push_function([&]()
  {
    vkDestroyPipelineLayout(_device, _gradientPipelineLayout, nullptr);
    vkDestroyPipeline(_device, backgroundEffects[0].pipeline, nullptr);
    vkDestroyPipeline(_device, backgroundEffects[1].pipeline, nullptr);
  });
}

void VulkanEngine::init_mesh_pipeline()
{
  VkShaderModule triangleFragShader;
  if (!vkutil::load_shader_module("../shaders/colored_triangle.frag.spv", _device, &triangleFragShader)) {
    fmt::print("Error when building the triangle fragment shader module");
  }
  else {
    fmt::print("Triangle fragment shader succesfully loaded");
  }

  VkShaderModule triangleVertexShader;
  if (!vkutil::load_shader_module("../shaders/colored_triangle_mesh.vert.spv", _device, &triangleVertexShader)) {
    fmt::print("Error when building the triangle vertex shader module");
  }
  else {
    fmt::print("Triangle vertex shader succesfully loaded");
  }

  VkPushConstantRange bufferRange
  {
    .offset = 0,
    .size = sizeof(GPUDrawPushConstants),
    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT
  };

  VkPipelineLayoutCreateInfo pipeline_layout_info = vkinit::pipeline_layout_create_info();
  pipeline_layout_info.pPushConstantRanges = &bufferRange;
  pipeline_layout_info.pushConstantRangeCount = 1;

  VK_CHECK(vkCreatePipelineLayout(_device, &pipeline_layout_info, nullptr, &_meshPipelineLayout));

  PipelineBuilder pipelineBuilder;

  //use the triangle layout we created
  pipelineBuilder._pipelineLayout = _meshPipelineLayout;
  //connecting the vertex and pixel shaders to the pipeline
  pipelineBuilder.set_shaders(triangleVertexShader, triangleFragShader);
  //it will draw triangles
  pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
  //filled triangles
  pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
  //no backface culling
  pipelineBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
  //no multisampling
  pipelineBuilder.set_multisampling_none();
  //no blending
  //pipelineBuilder.disable_blending();
  pipelineBuilder.enable_blending_additive();

  //pipelineBuilder.disable_depthtest();
  pipelineBuilder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);

  //connect the image format we will draw into, from draw image
  pipelineBuilder.set_color_attachment_format(_drawImage.imageFormat);
  pipelineBuilder.set_depth_format(_depthImage.imageFormat);

  //finally build the pipeline
  _meshPipeline = pipelineBuilder.build_pipeline(_device);

  //clean structures
  vkDestroyShaderModule(_device, triangleFragShader, nullptr);
  vkDestroyShaderModule(_device, triangleVertexShader, nullptr);

  _mainDeletionQueue.push_function([&]() {
    vkDestroyPipelineLayout(_device, _meshPipelineLayout, nullptr);
    vkDestroyPipeline(_device, _meshPipeline, nullptr);
  });
}

void VulkanEngine::init_default_data()
{
  _testMeshes = loadGltfMeshes(this,"../assets/basicmesh.glb").value();
}


void VulkanEngine::cleanup()
{
  if (_isInitialized)
  {
    //make sure the gpu has stopped doing its things
    vkDeviceWaitIdle(_device);
    for (auto& frame : _frames)
    {
      vkDestroyCommandPool(_device, frame._commandPool, nullptr);

      //destroy sync objects
      vkDestroyFence(_device, frame._renderFence, nullptr);
      vkDestroySemaphore(_device, frame._renderSemaphore, nullptr);
      vkDestroySemaphore(_device ,frame._swapchainSemaphore, nullptr);

      frame._deletionQueue.flush();
    }
    _mainDeletionQueue.flush();
    destroy_swapchain();

    vkDestroySurfaceKHR(_instance, _surface, nullptr);
    vkDestroyDevice(_device, nullptr);

    vkb::destroy_debug_utils_messenger(_instance, _debugMessenger);
    vkDestroyInstance(_instance, nullptr);
    SDL_DestroyWindow(_window);
  }

  volkFinalize();

  // clear engine pointer
  loadedEngine = nullptr;
}

void VulkanEngine::draw_background(VkCommandBuffer cmd)
{
  ComputeEffect& effect = backgroundEffects[currentBackgroundEffect];
  // bind the gradient drawing compute pipeline
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, effect.pipeline);

  // bind the descriptor set containing the draw image for the compute pipeline
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _gradientPipelineLayout, 0, 1, &_drawImageDescriptors, 0, nullptr);

  vkCmdPushConstants(cmd, _gradientPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &effect.data);

  // execute the compute pipeline dispatch. We are using 16x16 workgroup size so we need to divide by it
  vkCmdDispatch(cmd, std::ceil(_drawExtent.width / 16.0), std::ceil(_drawExtent.height / 16.0), 1);
}

void VulkanEngine::draw_geometry(VkCommandBuffer cmd)
{

  //allocate a new uniform buffer for the scene data
  AllocatedBuffer gpuSceneDataBuffer = create_buffer(sizeof(GPUSceneData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

  //add it to the deletion queue of this frame so it gets deleted once its been used
  get_current_frame()._deletionQueue.push_function([=]()
  {
    destroy_buffer(gpuSceneDataBuffer);
  });

  //write the buffer
  GPUSceneData* sceneUniformData = (GPUSceneData*)gpuSceneDataBuffer.allocation->GetMappedData();
  *sceneUniformData = sceneData;

  //create a descriptor set that binds that buffer and update it
  VkDescriptorSet globalDescriptor = get_current_frame()._frameDescriptors.allocate(_device, _gpuSceneDataDescriptorLayout);

  DescriptorWriter writer;
  writer.write_buffer(0, gpuSceneDataBuffer.buffer, sizeof(GPUSceneData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
  writer.update_set(_device, globalDescriptor);

  //begin a render pass  connected to our draw image
  VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(_drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_GENERAL);
  VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(_depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

  VkRenderingInfo renderInfo = vkinit::rendering_info(_drawExtent, &colorAttachment, &depthAttachment);
  vkCmdBeginRenderingKHR(cmd, &renderInfo);

  //draw rectangle
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _meshPipeline);

  //set dynamic viewport and scissor
  VkViewport viewport
  {
    .x = 0,
    .y = 0,
    .width = static_cast<float>(_drawExtent.width),
    .height = static_cast<float>(_drawExtent.height),
    .minDepth = 0.f,
    .maxDepth = 1.f
  };

  vkCmdSetViewport(cmd, 0, 1, &viewport);

  VkRect2D scissor
  {
    .offset.x = 0,
    .offset.y = 0,
    .extent.width = _drawExtent.width,
    .extent.height = _drawExtent.height
  };

  vkCmdSetScissor(cmd, 0, 1, &scissor);

  // draw monkeyhead
  // 0 -> cube, 1-> sphere, 2->monkeyhead
  GPUDrawPushConstants push_constants
  {
    .worldMatrix = glm::mat4{ 1.f },
    .vertexBufferAddress = _testMeshes[2]->meshBuffers.vertexBufferAddress
  };

  glm::mat4 view = glm::translate(glm::vec3{ 0,0,-5 });
  // camera projection
  // 10000 to near and 0.1 to far, so depth is reversed
  // i.e. depth == 1 is the near plane, depth == 0 is the far plane
  // this improves the quality of depth testing
  glm::mat4 projection = glm::perspective(glm::radians(70.f), (float)_drawExtent.width / (float)_drawExtent.height, 10000.f, 0.1f);
  // invert the Y direction on projection matrix
  // GLTF is like OpenGL which has positive y-direction up.
  // Vulkan has positive y-direction down
  projection[1][1] *= -1;
  push_constants.worldMatrix = projection * view;

  vkCmdPushConstants(cmd, _meshPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GPUDrawPushConstants), &push_constants);
  vkCmdBindIndexBuffer(cmd, _testMeshes[2]->meshBuffers.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdDrawIndexed(cmd, _testMeshes[2]->surfaces[0].count, 1, _testMeshes[2]->surfaces[0].startIndex, 0, 0);

  vkCmdEndRenderingKHR(cmd);
}

void VulkanEngine::draw_imgui(VkCommandBuffer cmd, VkImageView targetImageView)
{
  VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(targetImageView, nullptr, VK_IMAGE_LAYOUT_GENERAL);
  VkRenderingInfo renderInfo = vkinit::rendering_info(_swapchainExtent, &colorAttachment, nullptr);

  vkCmdBeginRenderingKHR(cmd, &renderInfo);

  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

  vkCmdEndRenderingKHR(cmd);
}

void VulkanEngine::draw()
{
  // wait until the gpu has finished rendering the last frame. Timeout of 1 second
  VK_CHECK(vkWaitForFences(_device, 1, &get_current_frame()._renderFence, VK_TRUE, 1000000000));

  get_current_frame()._deletionQueue.flush();
  get_current_frame()._frameDescriptors.clear_pools(_device);

  //request image from the swapchain
  uint32_t swapchainImageIndex;
  //VK_CHECK(vkAcquireNextImageKHR(_device, _swapchain, 1000000000, get_current_frame()._swapchainSemaphore, nullptr, &swapchainImageIndex));
  VkResult e = vkAcquireNextImageKHR(_device, _swapchain, 1000000000, get_current_frame()._swapchainSemaphore, nullptr, &swapchainImageIndex);
  switch (e)
  {
    case VK_SUBOPTIMAL_KHR: { _resizeRequested = true; break; }
    case VK_ERROR_OUT_OF_DATE_KHR: { _resizeRequested = true; return; }
    default: VK_CHECK(e);
  }

  _drawExtent.height = std::min(_swapchainExtent.height, _drawImage.imageExtent.height) * _renderScale;
  _drawExtent.width= std::min(_swapchainExtent.width, _drawImage.imageExtent.width) * _renderScale;

  VK_CHECK(vkResetFences(_device, 1, &get_current_frame()._renderFence));

  // now that we are sure that the commands finished executing, we can safely
  // reset the command buffer to begin recording again.
  VkCommandBuffer cmd = get_current_frame()._mainCommandBuffer;
  VK_CHECK(vkResetCommandBuffer(cmd, 0));

  //begin the command buffer recording. We will use this command buffer exactly once, so we want to let vulkan know that
  VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
  VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

  // transition our main draw image into general layout so we can write into it
  // we will overwrite it all so we dont care about what was the older layout
  vkutil::transition_image(cmd, _drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

  draw_background(cmd);

  vkutil::transition_image(cmd, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  vkutil::transition_image(cmd, _depthImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

  draw_geometry(cmd);

  //transtion the draw image and the swapchain image into their correct transfer layouts
  vkutil::transition_image(cmd, _drawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  //> imgui_draw
  // execute a copy from the draw image into the swapchain
  vkutil::copy_image_to_image(cmd, _drawImage.image, _swapchainImages[swapchainImageIndex], _drawExtent, _swapchainExtent);

  // set swapchain image layout to Attachment Optimal so we can draw it
  vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  //draw imgui into the swapchain image
  draw_imgui(cmd,  _swapchainImageViews[swapchainImageIndex]);

  // set swapchain image layout to Present so we can draw it
  vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

  //finalize the command buffer (we can no longer add commands, but it can now be executed)
  VK_CHECK(vkEndCommandBuffer(cmd));
  //< imgui_draw


  //prepare the submission to the queue.
  //we want to wait on the _presentSemaphore, as that semaphore is signaled when the swapchain is ready
  //we will signal the _renderSemaphore, to signal that rendering has finished

  VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd);

  VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR,get_current_frame()._swapchainSemaphore);
  VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, get_current_frame()._renderSemaphore);

  VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo,&signalInfo,&waitInfo);

  //submit command buffer to the queue and execute it.
  // _renderFence will now block until the graphic commands finish execution
  VK_CHECK(vkQueueSubmit2KHR(_graphicsQueue, 1, &submit, get_current_frame()._renderFence));

  //prepare present
  // this will put the image we just rendered to into the visible window.
  // we want to wait on the _renderSemaphore for that,
  // as its necessary that drawing commands have finished before the image is displayed to the user
  VkPresentInfoKHR presentInfo
  {
    .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
    .pNext = nullptr,
    .pSwapchains = &_swapchain,
    .swapchainCount = 1,
    .pWaitSemaphores = &get_current_frame()._renderSemaphore,
    .waitSemaphoreCount = 1,
    .pImageIndices = &swapchainImageIndex
  };

  //VK_CHECK(vkQueuePresentKHR(_graphicsQueue, &presentInfo));
  VkResult presentResult = vkQueuePresentKHR(_graphicsQueue, &presentInfo);
  switch (presentResult)
  {
    case VK_SUBOPTIMAL_KHR: { _resizeRequested = true; break; }
    case VK_ERROR_OUT_OF_DATE_KHR: { _resizeRequested = true; return; }
    default: VK_CHECK(presentResult);
  }

  //increase the number of frames drawn
  _frameNumber++;
}

void VulkanEngine::resize_swapchain()
{
  vkDeviceWaitIdle(_device);

  destroy_swapchain();

  int w, h;
  SDL_GetWindowSize(_window, &w, &h);
  _windowExtent.width = w;
  _windowExtent.height = h;

  create_swapchain(_windowExtent.width, _windowExtent.height);

  _resizeRequested = false;
}

void VulkanEngine::run()
{
  SDL_Event e;
  bool bQuit = false;

  // main loop
  while (!bQuit)
  {
    // Handle events on queue
    while (SDL_PollEvent(&e) != 0)
    {
      // close the window when user alt-f4s or clicks the X button
      if (e.type == SDL_QUIT)
      {
        bQuit = true;
      }

      if (e.type == SDL_WINDOWEVENT)
      {
        if (e.window.event == SDL_WINDOWEVENT_MINIMIZED)
        {
          _stopRendering = true;
        }
        if (e.window.event == SDL_WINDOWEVENT_RESTORED)
        {
          _stopRendering = false;
        }
      }
      //send SDL event to imgui for handling
      ImGui_ImplSDL2_ProcessEvent(&e);
    }

    // do not draw if we are minimized
    if (_stopRendering)
    {
      // throttle the speed to avoid the endless spinning
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    if (_resizeRequested) {
      resize_swapchain();
    }

    // imgui new frame
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL2_NewFrame(_window);
    ImGui::NewFrame();
    if (ImGui::Begin("background"))
    {
      ImGui::SliderFloat("Render Scale", &_renderScale, 0.3f, 1.f);
      ComputeEffect& selected = backgroundEffects[currentBackgroundEffect];

      ImGui::Text("Selected effect: ", selected.name);

      ImGui::SliderInt("Effect Index", &currentBackgroundEffect, 0, backgroundEffects.size() - 1);

      ImGui::InputFloat4("data1", (float*)&selected.data.data1);
      ImGui::InputFloat4("data2", (float*)&selected.data.data2);
      ImGui::InputFloat4("data3", (float*)&selected.data.data3);
      ImGui::InputFloat4("data4", (float*)&selected.data.data4);

      ImGui::End();
    }
    //make imgui calculate internal draw structures
    ImGui::Render();

    // our draw function
    draw();
  }
}
