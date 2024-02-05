#include <vk_descriptors.h>

void DescriptorLayoutBuilder::add_binding(uint32_t binding, VkDescriptorType type)
{
  const VkDescriptorSetLayoutBinding newbind
  {
    .binding = binding,
    .descriptorCount = 1,
    .descriptorType = type
  };

  bindings.push_back(newbind);
}

void DescriptorLayoutBuilder::clear()
{
  bindings.clear();
}

VkDescriptorSetLayout DescriptorLayoutBuilder::build(VkDevice device, VkShaderStageFlags shaderStages)
{
  for (auto& b : bindings)
  {
    b.stageFlags |= shaderStages;
  }

  const VkDescriptorSetLayoutCreateInfo info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .pNext = nullptr,
    .pBindings = bindings.data(),
    .bindingCount =  static_cast<uint32_t>(bindings.size()),
    .flags = 0
  };

  VkDescriptorSetLayout set;
  VK_CHECK(vkCreateDescriptorSetLayout(device, &info, nullptr, &set));

  return set;
}

void DescriptorAllocator::init_pool(VkDevice device, uint32_t maxSets, std::span<PoolSizeRatio> poolRatios)
{
  std::vector<VkDescriptorPoolSize> poolSizes;
  for (PoolSizeRatio ratio : poolRatios)
  {
    poolSizes.push_back(VkDescriptorPoolSize
      {
        .type = ratio.type,
        .descriptorCount =  static_cast<uint32_t>(ratio.ratio * maxSets)
      });
  }

  VkDescriptorPoolCreateInfo poolInfo =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .flags = 0,
    .maxSets = maxSets,
    .poolSizeCount =  static_cast<uint32_t>(poolSizes.size()),
    .pPoolSizes = poolSizes.data()
  };

  vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool);
}

void DescriptorAllocator::clear_descriptors(VkDevice device)
{
  vkResetDescriptorPool(device, pool, 0);
}

void DescriptorAllocator::destroy_pool(VkDevice device)
{
  vkDestroyDescriptorPool(device, pool, nullptr);
}

VkDescriptorSet DescriptorAllocator::allocate(VkDevice device, VkDescriptorSetLayout layout)
{
  const VkDescriptorSetAllocateInfo allocInfo =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .pNext = nullptr,
    .descriptorPool = pool,
    .descriptorSetCount = 1,
    .pSetLayouts = &layout
  };

  VkDescriptorSet ds;
  VK_CHECK(vkAllocateDescriptorSets(device, &allocInfo, &ds));

  return ds;
}
