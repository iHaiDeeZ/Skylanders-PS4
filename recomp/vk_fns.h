// The Vulkan functions gx_vk.cpp uses, loaded through vkGetInstanceProcAddr (the PS4's RADV is
// linked in and has no loader; the PC host gets the same table from libvulkan).
// VKFN_GLOBAL: before an instance exists; VKFN: through the instance.
#define VK_FNS_GLOBAL(VKFN_GLOBAL) VKFN_GLOBAL(vkCreateInstance)
#define VK_FNS(VKFN)                                                                                         \
	VKFN(vkAllocateCommandBuffers) VKFN(vkAllocateDescriptorSets) VKFN(vkAllocateMemory)                     \
	VKFN(vkBeginCommandBuffer) VKFN(vkBindBufferMemory) VKFN(vkBindImageMemory) VKFN(vkCmdBeginRendering)    \
	VKFN(vkCmdBindDescriptorSets) VKFN(vkCmdBindIndexBuffer) VKFN(vkCmdBindPipeline)                         \
	VKFN(vkCmdClearAttachments) VKFN(vkCmdCopyBufferToImage) VKFN(vkCmdCopyImageToBuffer) VKFN(vkCmdDraw)    \
	VKFN(vkCmdDrawIndexed) VKFN(vkCmdEndRendering) VKFN(vkCmdPipelineBarrier) VKFN(vkCmdPushConstants)       \
	VKFN(vkCmdSetScissor) VKFN(vkCmdSetViewport) VKFN(vkCreateBuffer) VKFN(vkCreateCommandPool)              \
	VKFN(vkCreateDescriptorPool) VKFN(vkCreateDescriptorSetLayout) VKFN(vkCreateDevice) VKFN(vkCreateFence)  \
	VKFN(vkCreateGraphicsPipelines) VKFN(vkCreateImage) VKFN(vkCreateImageView) VKFN(vkCreatePipelineCache)  \
	VKFN(vkCreatePipelineLayout) VKFN(vkCreateSampler) VKFN(vkCreateShaderModule) VKFN(vkDestroyImage)       \
	VKFN(vkDestroyImageView) VKFN(vkEndCommandBuffer) VKFN(vkEnumeratePhysicalDevices) VKFN(vkFreeMemory)    \
	VKFN(vkGetBufferMemoryRequirements) VKFN(vkGetDeviceQueue) VKFN(vkGetImageMemoryRequirements)            \
	VKFN(vkGetPhysicalDeviceFeatures2) VKFN(vkGetPhysicalDeviceMemoryProperties)                             \
	VKFN(vkGetPhysicalDeviceProperties) VKFN(vkGetPhysicalDeviceQueueFamilyProperties) VKFN(vkMapMemory)     \
	VKFN(vkQueueSubmit) VKFN(vkResetCommandPool) VKFN(vkResetFences) VKFN(vkUpdateDescriptorSets)            \
	VKFN(vkWaitForFences) VKFN(vkCmdBlitImage) VKFN(vkCmdClearColorImage) VKFN(vkCreateSemaphore)            \
	VKFN(vkCmdBindVertexBuffers)
// presenting (PS4: the kit's headless surface + swapchain on VideoOut)
#define VK_FNS_PRESENT(VKFN)                                                                                 \
	VKFN(vkCreateHeadlessSurfaceEXT) VKFN(vkCreateSwapchainKHR) VKFN(vkGetSwapchainImagesKHR)                \
	VKFN(vkAcquireNextImageKHR) VKFN(vkQueuePresentKHR)
