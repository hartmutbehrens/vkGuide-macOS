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

VkDescriptorPool DescriptorAllocatorGrowable::create_pool(VkDevice device, uint32_t setCount, std::span<PoolSizeRatio> poolRatios)
{
  std::vector<VkDescriptorPoolSize> poolSizes;
  for (PoolSizeRatio ratio : poolRatios)
  {
    poolSizes.push_back(
      VkDescriptorPoolSize
      {
        .type = ratio.type,
        .descriptorCount = static_cast<uint32_t>(ratio.ratio * setCount)
      });
  }

  VkDescriptorPoolCreateInfo pool_info
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .flags = 0,
    .maxSets = setCount,
    .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
    .pPoolSizes = poolSizes.data()
  };

  VkDescriptorPool newPool;
  vkCreateDescriptorPool(device, &pool_info, nullptr, &newPool);
  return newPool;
}

VkDescriptorPool DescriptorAllocatorGrowable::get_pool(VkDevice device)
{
  VkDescriptorPool newPool;
  if (readyPools.empty())
  {
    newPool = create_pool(device, setsPerPool, ratios);
    setsPerPool = setsPerPool * 2;
    if (setsPerPool > 4092)
    {
      setsPerPool = 4092;
    }
  }
  else
  {
    newPool = readyPools.back();
    readyPools.pop_back();
  }

  return newPool;
}

void DescriptorAllocatorGrowable::init(VkDevice device, uint32_t maxSets, std::span<PoolSizeRatio> poolRatios)
{
  ratios.clear();

  for (auto r : poolRatios)
  {
    ratios.push_back(r);
  }

  setsPerPool = maxSets * 2; //grow it next allocation
  VkDescriptorPool newPool = create_pool(device, maxSets, poolRatios);
  readyPools.push_back(newPool);
}

void DescriptorAllocatorGrowable::clear_pools(VkDevice device)
{
  for (auto p : readyPools)
  {
    vkResetDescriptorPool(device, p, 0);
  }
  for (auto p : fullPools)
  {
    vkResetDescriptorPool(device, p, 0);
    readyPools.push_back(p);
  }
  fullPools.clear();
}

void DescriptorAllocatorGrowable::destroy_pools(VkDevice device)
{
  for (auto p : readyPools)
  {
    vkDestroyDescriptorPool(device, p, nullptr);
  }
  readyPools.clear();
  for (auto p : fullPools)
  {
    vkDestroyDescriptorPool(device, p, nullptr);
  }
  fullPools.clear();
}

VkDescriptorSet DescriptorAllocatorGrowable::allocate(VkDevice device, VkDescriptorSetLayout layout)
{
  //get or create a pool to allocate from
  VkDescriptorPool poolToUse = get_pool(device);

  VkDescriptorSetAllocateInfo allocInfo
  {
    .pNext = nullptr,
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = poolToUse,
    .descriptorSetCount = 1,
    .pSetLayouts = &layout
  };

  VkDescriptorSet ds;
  VkResult result = vkAllocateDescriptorSets(device, &allocInfo, &ds);

  //allocation failed. Try again
  if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL)
  {
    fullPools.push_back(poolToUse);

    poolToUse = get_pool(device);
    allocInfo.descriptorPool = poolToUse;
    VK_CHECK(vkAllocateDescriptorSets(device, &allocInfo, &ds));
  }

  readyPools.push_back(poolToUse);
  return ds;
}


