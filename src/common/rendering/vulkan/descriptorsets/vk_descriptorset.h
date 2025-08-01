
#pragma once

#include "zvulkan/vulkanobjects.h"
#include "zvulkan/vulkanbuilders.h"
#include <list>
#include "tarray.h"

class VulkanRenderDevice;
class VkMaterial;
class PPTextureInput;
class VkPPRenderPassSetup;
struct FSWColormap;

class VkDescriptorSetManager
{
public:
	VkDescriptorSetManager(VulkanRenderDevice* fb);
	~VkDescriptorSetManager();

	void Init();
	void Deinit();
	void BeginFrame();
	void ResetHWTextureSets();

	VulkanDescriptorSetLayout* GetLevelMeshLayout() { return LevelMesh.Layout.get(); }
	VulkanDescriptorSetLayout* GetRSBufferLayout() { return RSBuffer.Layout.get(); }
	VulkanDescriptorSetLayout* GetFixedLayout() { return Fixed.Layout.get(); }
	VulkanDescriptorSetLayout* GetBindlessLayout() { return Bindless.Layout.get(); }
	VulkanDescriptorSetLayout* GetLightTilesLayout() { return LightTiles.Layout.get(); }
	VulkanDescriptorSetLayout* GetZMinMaxLayout() { return ZMinMax.Layout.get(); }

	VulkanDescriptorSet* GetLevelMeshSet() { return LevelMesh.Set.get(); }
	VulkanDescriptorSet* GetRSBufferSet() { return RSBuffer.Set.get(); }
	VulkanDescriptorSet* GetFixedSet() { return Fixed.Set.get(); }
	VulkanDescriptorSet* GetBindlessSet() { return Bindless.Set.get(); }
	VulkanDescriptorSet* GetLightTilesSet() { return LightTiles.Set.get(); }
	VulkanDescriptorSet* GetZMinMaxSet(int index) { return ZMinMax.Set[index].get(); }

	VulkanDescriptorSet* GetInput(VkPPRenderPassSetup* passSetup, const TArray<PPTextureInput>& textures, bool bindShadowMapBuffers);

	void AddMaterial(VkMaterial* texture);
	void RemoveMaterial(VkMaterial* texture);

	void UpdateBindlessDescriptorSet();
	void SetBindlessTexture(int index, VulkanImageView* imageview, VulkanSampler* sampler);

	int GetSWColormapTextureIndex(FSWColormap* colormap);
	int GetLightProbeTextureIndex(int probeIndex);

	int AllocBindlessSlot(int count);
	void FreeBindlessSlot(int index);

private:
	void CreateLevelMeshLayout();
	void CreateRSBufferLayout();
	void CreateFixedLayout();
	void CreateLightTilesLayout();
	void CreateZMinMaxLayout();
	void CreateLevelMeshPool();
	void CreateRSBufferPool();
	void CreateFixedPool();
	void CreateLightTilesPool();
	void CreateZMinMaxPool();
	void CreateBindlessSet();
	void UpdateFixedSet();
	void UpdateLevelMeshSet();
	void UpdateLightTilesSet();
	void UpdateZMinMaxSet();

	std::unique_ptr<VulkanDescriptorSet> AllocatePPSet(VulkanDescriptorSetLayout* layout);

	VulkanRenderDevice* fb = nullptr;

	static const int MaxFixedSets = 100;
	static const int MaxBindlessTextures = 16536;
	static const int FixedBindlessSlots = 3;
	static const int MaxLightmaps = 128;

	struct
	{
		std::unique_ptr<VulkanDescriptorSetLayout> Layout;
		std::unique_ptr<VulkanDescriptorPool> Pool;
		std::unique_ptr<VulkanDescriptorSet> Set;
	} LevelMesh;

	struct
	{
		std::unique_ptr<VulkanDescriptorSetLayout> Layout;
		std::unique_ptr<VulkanDescriptorPool> Pool;
		std::unique_ptr<VulkanDescriptorSet> Set;
	} RSBuffer;

	struct
	{
		std::unique_ptr<VulkanDescriptorSetLayout> Layout;
		std::unique_ptr<VulkanDescriptorPool> Pool;
		std::unique_ptr<VulkanDescriptorSet> Set;
	} Fixed;

	struct
	{
		std::unique_ptr<VulkanDescriptorPool> Pool;
		std::unique_ptr<VulkanDescriptorSet> Set;
		std::unique_ptr<VulkanDescriptorSetLayout> Layout;
		WriteDescriptors Writer;
		int NextIndex = FixedBindlessSlots + MaxLightmaps;
		std::vector<int> AllocSizes;
		std::vector<std::vector<int>> FreeSlots;
	} Bindless;

	struct
	{
		std::unique_ptr<VulkanDescriptorPool> Pool;
		std::unique_ptr<VulkanDescriptorSet> Set;
		std::unique_ptr<VulkanDescriptorSetLayout> Layout;
	} Lightmap;

	struct
	{
		std::unique_ptr<VulkanDescriptorPool> Pool;
	} Postprocess;

	struct
	{
		std::unique_ptr<VulkanDescriptorPool> Pool;
		std::unique_ptr<VulkanDescriptorSet> Set;
		std::unique_ptr<VulkanDescriptorSetLayout> Layout;
	} LightTiles;

	struct
	{
		std::unique_ptr<VulkanDescriptorPool> Pool;
		std::unique_ptr<VulkanDescriptorSet> Set[6];
		std::unique_ptr<VulkanDescriptorSetLayout> Layout;
	} ZMinMax;

	std::list<VkMaterial*> Materials;
	std::vector<FSWColormap*> Colormaps;
	std::vector<int> LightProbes;
};
