﻿// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.

#pragma once

#include <vk_types.h>

#include <ranges>

struct DeletionQueue
{
	std::deque<std::function<void()>> deletors;

	void push_function(std::function<void()>&& function) {
		deletors.push_back(function);
	}

	void flush() {
		// reverse iterate the deletion queue to execute all the functions
		for (auto & deletor : std::ranges::reverse_view(deletors)) {
			deletor(); //call functors
		}

		deletors.clear();
	}
};

struct FrameData {
	VkCommandPool _commandPool;
	VkCommandBuffer _mainCommandBuffer;

	VkSemaphore _swapchainSemaphore;
	VkSemaphore _renderSemaphore;
	VkFence _renderFence;

	DeletionQueue _deletionQueue;
};

constexpr unsigned int FRAME_OVERLAP = 2;

class VulkanEngine {
public:

	bool _isInitialized{ false };
	int _frameNumber {0};
	bool _stopRendering{ false };
	VkExtent2D _windowExtent{ 1700 , 900 };

	struct SDL_Window* _window{ nullptr };

	static VulkanEngine& Get();

	//initializes everything in the engine
	void init();

	//shuts down the engine
	void cleanup();

	void draw_background(VkCommandBuffer cmd);
	//draw loop
	void draw();

	//run main loop
	void run();

	FrameData& get_current_frame() { return _frames[_frameNumber % FRAME_OVERLAP]; };

public:
	VkInstance _instance;  // Vulkan library handle
	VkDebugUtilsMessengerEXT _debugMessenger;  // Vulkan debug output handle
	VkPhysicalDevice _chosenGPU;  // GPU chosen as the default device
	VkDevice _device;  // Vulkan device for commands
	VkSurfaceKHR _surface;  // Vulkan window surface

	VkSwapchainKHR _swapchain;
	VkFormat _swapchainImageFormat;

	std::vector<VkImage> _swapchainImages;
	std::vector<VkImageView> _swapchainImageViews;
	VkExtent2D _swapchainExtent;

	FrameData _frames[FRAME_OVERLAP];

	VkQueue _graphicsQueue;
	uint32_t _graphicsQueueFamily;

	DeletionQueue _mainDeletionQueue;

	VmaAllocator _allocator;

	//draw resources
	AllocatedImage _drawImage;
	VkExtent2D _drawExtent;

private:
	void create_swapchain(uint32_t width, uint32_t height);
	void destroy_swapchain();
	void init_vulkan();
	void init_swapchain();
	void init_commands();
	void init_sync_structures();
};
