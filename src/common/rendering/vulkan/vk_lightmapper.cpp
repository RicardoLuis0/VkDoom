 
#include "vk_lightmapper.h"
#include "vulkan/vk_renderdevice.h"
#include "vulkan/textures/vk_texture.h"
#include "vulkan/commands/vk_commandbuffer.h"
#include "vulkan/descriptorsets/vk_descriptorset.h"
#include "vulkan/shaders/vk_shadercache.h"
#include "vk_levelmesh.h"
#include "zvulkan/vulkanbuilders.h"
#include "filesystem.h"
#include "cmdlib.h"

static int lastSurfaceCount;
static glcycle_t lightmapRaytraceLast;

static uint32_t lastPixelCount;

ADD_STAT(lightmapper)
{
	FString out;
	out.Format("CPU time: %.3fms\nSurface count: %d\nPixel count: %u K", lightmapRaytraceLast.TimeMS(), lastSurfaceCount, lastPixelCount / 1024);
	return out;
}

CVAR(Int, lm_background_updates, 8, CVAR_NOSAVE);
CVAR(Int, lm_max_updates, 128, CVAR_NOSAVE);
CVAR(Float, lm_scale, 1.0, CVAR_NOSAVE);
CVAR(Bool, lm_sunlight, true, CVAR_ARCHIVE);
CVAR(Bool, lm_blur, true, CVAR_ARCHIVE);
CVAR(Bool, lm_ao, true, CVAR_ARCHIVE);
CVAR(Bool, lm_softshadows, true, CVAR_ARCHIVE);
CVAR(Bool, lm_bounce, true, CVAR_ARCHIVE);
CVAR(Bool, lm_dynamic, true, CVAR_ARCHIVE);
EXTERN_CVAR(Bool, lm_dynlights);

VkLightmapper::VkLightmapper(VulkanRenderDevice* fb) : fb(fb)
{
	useRayQuery = fb->IsRayQueryEnabled();

	try
	{
		CreateUniformBuffer();
		CreateTileBuffer();
		CreateDrawIndexedBuffer();

		CreateShaders();
		CreateRaytracePipeline();
		CreateResolvePipeline();
		CreateBlurPipeline();
		CreateCopyPipeline();
		CreateBakeImage();
	}
	catch (...)
	{
		ReleaseResources();
		throw;
	}
}

VkLightmapper::~VkLightmapper()
{
	ReleaseResources();
}

void VkLightmapper::ReleaseResources()
{
	if (copytiles.Buffer)
		copytiles.Buffer->Unmap();
	if (drawindexed.CommandsBuffer)
		drawindexed.CommandsBuffer->Unmap();
	if (drawindexed.ConstantsBuffer)
		drawindexed.ConstantsBuffer->Unmap();
}

void VkLightmapper::SetLevelMesh(LevelMesh* level)
{
	mesh = level;

	lightmapRaytraceLast.Reset();
	lastPixelCount = 0;
	lastSurfaceCount = 0;
}

void VkLightmapper::BeginFrame()
{
	drawindexed.Pos = 0;
	copytiles.Pos = 0;

	UpdateAccelStructDescriptors();
}

void VkLightmapper::Raytrace(const TArray<LightmapTile*>& tiles)
{
	if (mesh && tiles.Size() > 0)
	{
		lightmapRaytraceLast.active = true;

		lightmapRaytraceLast.ResetAndClock();
		fb->GetCommands()->PushGroup(fb->GetCommands()->GetTransferCommands(), "lightmap.total");
		UploadUniforms();

		bool firstCall = true;
		while (true)
		{
			SelectTiles(tiles);
			if (selectedTiles.Size() == 0)
				break;

			Render();
			Resolve();
			if (lm_blur)
				Blur();
			CopyResult();

			if (drawindexed.IsFull || copytiles.Pos == drawindexed.BufferSize)
			{
				fb->GetCommands()->PopGroup(fb->GetCommands()->GetTransferCommands());
				fb->WaitForCommands(false);
				fb->GetCommands()->PushGroup(fb->GetCommands()->GetTransferCommands(), "lightmap.total");
				drawindexed.Pos = 0;
				drawindexed.IsFull = false;
				copytiles.Pos = 0;
			}
		}

		fb->GetCommands()->PopGroup(fb->GetCommands()->GetTransferCommands());
		lightmapRaytraceLast.Unclock();
	}
}

void VkLightmapper::SelectTiles(const TArray<LightmapTile*>& tiles)
{
	bakeImage.maxX = 0;
	bakeImage.maxY = 0;
	selectedTiles.Clear();

	// We use a 3 texel spacing between rectangles so that the blur pass will not pick up anything from a neighbour tile.
	if (!packer)
		packer = std::make_unique<RectPacker>(bakeImageSize, bakeImageSize, 3);
	else
		packer->Clear();

	for (int i = 0, count = tiles.Size(); i < count; i++)
	{
		LightmapTile* tile = tiles[i];

		if (!tile->ReceivedNewLight)
			continue;

		// Only grab surfaces until our bake texture is full
		auto result = packer->Alloc(tile->AtlasLocation.Width, tile->AtlasLocation.Height);
		if (result->PageIndex == 0)
		{
			SelectedTile selected;
			selected.Tile = tile;
			selected.X = result->X;
			selected.Y = result->Y;
			selectedTiles.Push(selected);

			bakeImage.maxX = std::max<uint16_t>(bakeImage.maxX, uint16_t(result->X + tile->AtlasLocation.Width));
			bakeImage.maxY = std::max<uint16_t>(bakeImage.maxY, uint16_t(result->Y + tile->AtlasLocation.Height));

			tile->ReceivedNewLight = false;
			tile->NeedsInitialBake = false;
			tile->GeometryUpdate = false;
		}
	}

	// Include the padding
	bakeImage.maxX += 3;
	bakeImage.maxY += 3;
}

void VkLightmapper::Render()
{
	auto cmdbuffer = fb->GetCommands()->GetTransferCommands();

	fb->GetCommands()->PushGroup(cmdbuffer, "lightmap.raytrace");

	PipelineBarrier()
		.AddImage(bakeImage.raytrace.Image.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
		.Execute(cmdbuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

	RenderPassBegin()
		.RenderPass(raytrace.renderPass.get())
		.RenderArea(0, 0, bakeImage.maxX, bakeImage.maxY)
		.Framebuffer(bakeImage.raytrace.Framebuffer.get())
		.AddClearColor(0.0f, 0.0f, 0.0f, -1.0f)
		.Execute(cmdbuffer);

	VkDeviceSize offset = 0;
	cmdbuffer->bindVertexBuffers(0, 1, &fb->GetLevelMesh()->GetVertexBuffer()->buffer, &offset);
	cmdbuffer->bindIndexBuffer(fb->GetLevelMesh()->GetIndexBuffer()->buffer, 0, VK_INDEX_TYPE_UINT32);
	cmdbuffer->bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, raytrace.pipeline[GetRaytracePipelineIndex()].get());
	cmdbuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, raytrace.pipelineLayout.get(), 0, raytrace.descriptorSet0.get());
	cmdbuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, raytrace.pipelineLayout.get(), 1, raytrace.descriptorSet1.get());
	cmdbuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, raytrace.pipelineLayout.get(), 2, fb->GetDescriptorSetManager()->GetBindlessSet());

	VkViewport viewport = {};
	viewport.maxDepth = 1;
	viewport.width = (float)bakeImageSize;
	viewport.height = (float)bakeImageSize;
	cmdbuffer->setViewport(0, 1, &viewport);

	int startPos = drawindexed.Pos;

	for (int i = 0, count = selectedTiles.Size(); i < count; i++)
	{
		auto& selectedTile = selectedTiles[i];
		LightmapTile* targetTile = selectedTile.Tile;

		LightmapRaytracePC pc;
		pc.TileX = (float)selectedTile.X;
		pc.TileY = (float)selectedTile.Y;
		pc.TextureSize = (float)bakeImageSize;
		pc.TileWidth = (float)targetTile->AtlasLocation.Width;
		pc.TileHeight = (float)targetTile->AtlasLocation.Height;
		pc.WorldToLocal = SwapYZ(targetTile->Transform.TranslateWorldToLocal);
		pc.ProjLocalToU = SwapYZ(targetTile->Transform.ProjLocalToU);
		pc.ProjLocalToV = SwapYZ(targetTile->Transform.ProjLocalToV);

		// Paint all surfaces visible in the tile

		visibleSurfaces.Clear();
		mesh->GetVisibleSurfaces(targetTile, visibleSurfaces);

		if (drawindexed.Pos + (int)visibleSurfaces.Size() >= drawindexed.BufferSize)
		{
			// Our indirect draw buffer is full. Mark the remaining tiles as not rendered
			while (i < count)
			{
				selectedTiles[i].Tile->ReceivedNewLight = true;
				i++;
			}
			drawindexed.IsFull = true;
			break;
		}

		for (int surfaceIndex : visibleSurfaces)
		{
			LevelMeshSurface* surface = &mesh->Mesh.Surfaces[surfaceIndex];
			pc.SurfaceIndex = surfaceIndex;

			VkDrawIndexedIndirectCommand cmd;
			cmd.indexCount = surface->MeshLocation.NumElements;
			cmd.instanceCount = 1;
			cmd.firstIndex = surface->MeshLocation.StartElementIndex;
			cmd.vertexOffset = 0;
			cmd.firstInstance = drawindexed.Pos;
			drawindexed.Constants[drawindexed.Pos] = pc;
			drawindexed.Commands[drawindexed.Pos] = cmd;
			drawindexed.Pos++;
		}

		selectedTile.Rendered = true;
	}

	if (drawindexed.Pos > startPos)
		cmdbuffer->drawIndexedIndirect(drawindexed.CommandsBuffer->buffer, startPos * sizeof(VkDrawIndexedIndirectCommand), drawindexed.Pos - startPos, sizeof(VkDrawIndexedIndirectCommand));

	cmdbuffer->endRenderPass();

	fb->GetCommands()->PopGroup(cmdbuffer);
}

void VkLightmapper::UploadUniforms()
{
	Uniforms values = {};
	values.SunDir = SwapYZ(mesh->SunDirection);
	values.SunColor = mesh->SunColor;
	values.SunIntensity = mesh->SunIntensity;

	uniforms.Uniforms = (uint8_t*)uniforms.TransferBuffer->Map(0, uniforms.NumStructs * uniforms.StructStride);
	*reinterpret_cast<Uniforms*>(uniforms.Uniforms + uniforms.StructStride * uniforms.Index) = values;
	uniforms.TransferBuffer->Unmap();

	auto cmdbuffer = fb->GetCommands()->GetTransferCommands();
	cmdbuffer->copyBuffer(uniforms.TransferBuffer.get(), uniforms.Buffer.get());
	PipelineBarrier()
		.AddBuffer(uniforms.Buffer.get(), VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
		.Execute(cmdbuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

void VkLightmapper::Resolve()
{
	auto cmdbuffer = fb->GetCommands()->GetTransferCommands();

	fb->GetCommands()->PushGroup(cmdbuffer, "lightmap.resolve");

	PipelineBarrier()
		.AddImage(bakeImage.raytrace.Image.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
		.AddImage(bakeImage.resolve.Image.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
		.Execute(cmdbuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	RenderPassBegin()
		.RenderPass(resolve.renderPass.get())
		.RenderArea(0, 0, bakeImage.maxX, bakeImage.maxY)
		.Framebuffer(bakeImage.resolve.Framebuffer.get())
		.Execute(cmdbuffer);

	cmdbuffer->bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, resolve.pipeline.get());
	cmdbuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, resolve.pipelineLayout.get(), 0, bakeImage.resolve.DescriptorSet.get());

	VkViewport viewport = {};
	viewport.maxDepth = 1;
	viewport.width = (float)bakeImage.maxX;
	viewport.height = (float)bakeImage.maxY;
	cmdbuffer->setViewport(0, 1, &viewport);

	cmdbuffer->draw(3, 1, 0, 0);

	cmdbuffer->endRenderPass();

	fb->GetCommands()->PopGroup(cmdbuffer);
}

void VkLightmapper::Blur()
{
	auto cmdbuffer = fb->GetCommands()->GetTransferCommands();

	fb->GetCommands()->PushGroup(cmdbuffer, "lightmap.blur");

	PipelineBarrier()
		.AddImage(bakeImage.resolve.Image.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
		.Execute(cmdbuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	// Pass 0
	{
		RenderPassBegin()
			.RenderPass(blur.renderPass.get())
			.RenderArea(0, 0, bakeImage.maxX, bakeImage.maxY)
			.Framebuffer(bakeImage.blur.Framebuffer.get())
			.Execute(cmdbuffer);

		cmdbuffer->bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, blur.pipeline[0].get());
		cmdbuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, blur.pipelineLayout.get(), 0, bakeImage.blur.DescriptorSet[0].get());

		VkViewport viewport = {};
		viewport.maxDepth = 1;
		viewport.width = (float)bakeImage.maxX;
		viewport.height = (float)bakeImage.maxY;
		cmdbuffer->setViewport(0, 1, &viewport);

		cmdbuffer->draw(3, 1, 0, 0);

		cmdbuffer->endRenderPass();
	}

	PipelineBarrier()
		.AddImage(bakeImage.blur.Image.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
		.Execute(cmdbuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	// Pass 1 (outputs back into resolve fb)
	{
		RenderPassBegin()
			.RenderPass(blur.renderPass.get())
			.RenderArea(0, 0, bakeImage.maxX, bakeImage.maxY)
			.Framebuffer(bakeImage.resolve.Framebuffer.get())
			.Execute(cmdbuffer);

		cmdbuffer->bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, blur.pipeline[1].get());
		cmdbuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, blur.pipelineLayout.get(), 0, bakeImage.blur.DescriptorSet[1].get());

		VkViewport viewport = {};
		viewport.maxDepth = 1;
		viewport.width = (float)bakeImage.maxX;
		viewport.height = (float)bakeImage.maxY;
		cmdbuffer->setViewport(0, 1, &viewport);

		cmdbuffer->draw(3, 1, 0, 0);

		cmdbuffer->endRenderPass();
	}

	fb->GetCommands()->PopGroup(cmdbuffer);
}

void VkLightmapper::CopyResult()
{
	// Sort by destination
	uint32_t pixels = 0;
	lastSurfaceCount = 0;
	for (auto& list : copylists) list.Clear();
	for (int i = 0, count = selectedTiles.Size(); i < count; i++)
	{
		auto& selected = selectedTiles[i];
		if (selected.Rendered)
		{
			unsigned int pageIndex = (unsigned int)selected.Tile->AtlasLocation.ArrayIndex;
			if (pageIndex >= copylists.Size())
			{
				copylists.Resize(pageIndex + 1);
			}
			copylists[pageIndex].Push(&selected);

			pixels += selected.Tile->AtlasLocation.Area();
			lastSurfaceCount++;
		}
	}
	lastPixelCount = pixels;

	if (pixels == 0)
		return;

	std::vector<VkTextureManager::Lightmap>& destTexture = fb->GetTextureManager()->Lightmaps;

	auto cmdbuffer = fb->GetCommands()->GetTransferCommands();

	fb->GetCommands()->PushGroup(cmdbuffer, "lightmap.copy");

	// Transition lightmap destination images to be used as framebuffers and the resolve image as sampling source
	PipelineBarrier barrier0;
	barrier0.AddImage(bakeImage.resolve.Image.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
	for (unsigned int i = 0, count = copylists.Size(); i < count; i++)
	{
		if (copylists[i].Size() > 0)
		{
			barrier0.AddImage(destTexture[i].Light.Image.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
			barrier0.AddImage(destTexture[i].Probe.Image.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
		}
	}
	barrier0.Execute(cmdbuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	// Copy into the lightmap images
	int start = copytiles.Pos;
	int pos = copytiles.Pos;
	for (unsigned int i = 0, count = copylists.Size(); i < count; i++)
	{
		auto& list = copylists[i];
		if (list.Size() == 0)
			continue;

		int destSize = destTexture[i].Light.Image->width;

		// Create framebuffer object if it doesn't exist
		auto& framebuffer = destTexture[i].Light.LMFramebuffer;
		if (!framebuffer)
		{
			auto& lightView = destTexture[i].Light.LMView;
			if (!lightView)
			{
				lightView = ImageViewBuilder()
					.Type(VK_IMAGE_VIEW_TYPE_2D)
					.Image(destTexture[i].Light.Image.get(), VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 1)
					.DebugName("LMLightView")
					.Create(fb->GetDevice());
			}

			auto& probeView = destTexture[i].Probe.LMView;
			if (!probeView)
			{
				probeView = ImageViewBuilder()
					.Type(VK_IMAGE_VIEW_TYPE_2D)
					.Image(destTexture[i].Probe.Image.get(), VK_FORMAT_R16_UINT, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 1)
					.DebugName("LMProbeView")
					.Create(fb->GetDevice());
			}

			framebuffer = FramebufferBuilder()
				.RenderPass(copy.renderPass.get())
				.Size(destSize, destSize)
				.AddAttachment(lightView.get())
				.AddAttachment(probeView.get())
				.DebugName("LMFramebuffer")
				.Create(fb->GetDevice());
		}

		// Copy the tile positions into a storage buffer for the vertex shader to read
		start = pos;
		for (SelectedTile* selected : list)
		{
			LightmapTile* tile = selected->Tile;

			CopyTileInfo* copyinfo = &copytiles.Tiles[pos++];
			copyinfo->SrcPosX = selected->X;
			copyinfo->SrcPosY = selected->Y;
			copyinfo->DestPosX = tile->AtlasLocation.X;
			copyinfo->DestPosY = tile->AtlasLocation.Y;
			copyinfo->TileWidth = tile->AtlasLocation.Width;
			copyinfo->TileHeight = tile->AtlasLocation.Height;
			copyinfo->WorldOrigin = tile->InverseTransform.WorldOrigin;
			copyinfo->WorldU = tile->InverseTransform.WorldU;
			copyinfo->WorldV = tile->InverseTransform.WorldV;
		}

		// Draw the tiles. One instance per tile.
		RenderPassBegin()
			.RenderPass(copy.renderPass.get())
			.RenderArea(0, 0, destSize, destSize)
			.Framebuffer(framebuffer.get())
			.Execute(cmdbuffer);

		cmdbuffer->bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, copy.pipeline.get());
		cmdbuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, copy.pipelineLayout.get(), 0, bakeImage.copy.DescriptorSet.get());

		VkViewport viewport = {};
		viewport.maxDepth = 1;
		viewport.width = (float)destSize;
		viewport.height = (float)destSize;
		cmdbuffer->setViewport(0, 1, &viewport);

		LightmapCopyPC pc;
		pc.SrcTexSize = bakeImageSize;
		pc.DestTexSize = destSize;
		cmdbuffer->pushConstants(copy.pipelineLayout.get(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(LightmapCopyPC), &pc);

		cmdbuffer->draw(4, pos - start, 0, start);

		cmdbuffer->endRenderPass();
	}

	copytiles.Pos = pos;

	// Transition lightmap destination images back to be used for fragment shader sampling
	PipelineBarrier barrier1;
	for (unsigned int i = 0, count = copylists.Size(); i < count; i++)
	{
		if (copylists[i].Size() > 0)
		{
			barrier1.AddImage(destTexture[i].Light.Image.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
			barrier1.AddImage(destTexture[i].Probe.Image.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
		}
	}
	barrier1.Execute(cmdbuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	fb->GetCommands()->PopGroup(cmdbuffer);
}

void VkLightmapper::CreateShaders()
{
	std::string prefix = "#version 460\r\n";
	std::string traceprefix = "#version 460\r\n";
	prefix += "#extension GL_GOOGLE_include_directive : enable\n";

	traceprefix += "#extension GL_GOOGLE_include_directive : enable\n";
	traceprefix += "#extension GL_EXT_nonuniform_qualifier : enable\r\n";
	if (useRayQuery)
	{
		traceprefix += "#extension GL_EXT_ray_query : require\r\n";
		traceprefix += "#define USE_RAYQUERY\r\n";
	}

	shaders.vertRaytrace = CachedGLSLCompiler()
		.Type(ShaderType::Vertex)
		.AddSource("VersionBlock", prefix)
		.AddSource("vert_raytrace.glsl", LoadPrivateShaderLump("shaders/lightmap/vert_raytrace.glsl").GetChars())
		.Compile(fb);

	shaders.vertScreenquad = CachedGLSLCompiler()
		.Type(ShaderType::Vertex)
		.AddSource("VersionBlock", prefix)
		.AddSource("vert_screenquad.glsl", LoadPrivateShaderLump("shaders/lightmap/vert_screenquad.glsl").GetChars())
		.Compile(fb);

	shaders.vertCopy = CachedGLSLCompiler()
		.Type(ShaderType::Vertex)
		.AddSource("VersionBlock", prefix)
		.AddSource("vert_copy.glsl", LoadPrivateShaderLump("shaders/lightmap/vert_copy.glsl").GetChars())
		.Compile(fb);

	for (int i = 0; i < 16; i++)
	{
		std::string defines = traceprefix;
		if (i & 1)
			defines += "#define USE_SOFTSHADOWS\n";
		if (i & 2)
			defines += "#define USE_AO\n";
		if (i & 4)
			defines += "#define USE_SUNLIGHT\n";
		if (i & 8)
			defines += "#define USE_BOUNCE\n";

		shaders.fragRaytrace[i] = CachedGLSLCompiler()
			.Type(ShaderType::Fragment)
			.AddSource("VersionBlock", defines)
			.AddSource("frag_raytrace.glsl", LoadPrivateShaderLump("shaders/lightmap/frag_raytrace.glsl").GetChars())
			.Compile(fb);
	}

	shaders.fragResolve = CachedGLSLCompiler()
		.Type(ShaderType::Fragment)
		.AddSource("VersionBlock", prefix)
		.AddSource("frag_resolve.glsl", LoadPrivateShaderLump("shaders/lightmap/frag_resolve.glsl").GetChars())
		.Compile(fb);

	shaders.fragBlur[0] = CachedGLSLCompiler()
		.Type(ShaderType::Fragment)
		.AddSource("VersionBlock", prefix + "#define BLUR_HORIZONTAL\r\n")
		.AddSource("frag_blur.glsl", LoadPrivateShaderLump("shaders/lightmap/frag_blur.glsl").GetChars())
		.Compile(fb);

	shaders.fragBlur[1] = CachedGLSLCompiler()
		.Type(ShaderType::Fragment)
		.AddSource("VersionBlock", prefix + "#define BLUR_VERTICAL\r\n")
		.AddSource("frag_blur.glsl", LoadPrivateShaderLump("shaders/lightmap/frag_blur.glsl").GetChars())
		.Compile(fb);

	shaders.fragCopy = CachedGLSLCompiler()
		.Type(ShaderType::Fragment)
		.AddSource("VersionBlock", prefix)
		.AddSource("frag_copy.glsl", LoadPrivateShaderLump("shaders/lightmap/frag_copy.glsl").GetChars())
		.Compile(fb);
}

int VkLightmapper::GetRaytracePipelineIndex()
{
	// When running as the baking tool we don't care about the CVAR or hardware preferences and only want to act on what the map specified.
	bool userSoftshadows = RunningAsTool || (lm_softshadows && useRayQuery);
	bool userAO = RunningAsTool || (lm_ao && useRayQuery);
	bool userSunlight = RunningAsTool || lm_sunlight;
	bool userBounce = RunningAsTool || lm_bounce;

	int index = 0;
	if (userSoftshadows)
		index |= 1;
	if (mesh->AmbientOcclusion && userAO)
		index |= 2;
	if (mesh->SunColor != FVector3(0.0f, 0.0f, 0.0f) && userSunlight)
		index |= 4;
	if (mesh->LightBounce && userBounce)
		index |= 8;
	return index;
}

FString VkLightmapper::LoadPrivateShaderLump(const char* lumpname)
{
	return fb->GetShaderCache()->GetPrivateFileText(lumpname);
}

FString VkLightmapper::LoadPublicShaderLump(const char* lumpname)
{
	return fb->GetShaderCache()->GetPublicFileText(lumpname);
}

void VkLightmapper::CreateRaytracePipeline()
{
	raytrace.descriptorSetLayout0 = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		.DebugName("raytrace.descriptorSetLayout0")
		.Create(fb->GetDevice());

	if (useRayQuery)
	{
		raytrace.descriptorSetLayout1 = DescriptorSetLayoutBuilder()
			.AddBinding(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
			.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
			.AddBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
			.DebugName("raytrace.descriptorSetLayout1")
			.Create(fb->GetDevice());
	}
	else
	{
		raytrace.descriptorSetLayout1 = DescriptorSetLayoutBuilder()
			.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
			.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
			.AddBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
			.DebugName("raytrace.descriptorSetLayout1")
			.Create(fb->GetDevice());
	}

	raytrace.pipelineLayout = PipelineLayoutBuilder()
		.AddSetLayout(raytrace.descriptorSetLayout0.get())
		.AddSetLayout(raytrace.descriptorSetLayout1.get())
		.AddSetLayout(fb->GetDescriptorSetManager()->GetBindlessLayout())
		.DebugName("raytrace.pipelineLayout")
		.Create(fb->GetDevice());

	raytrace.renderPass = RenderPassBuilder()
		.AddAttachment(
			VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_SAMPLE_COUNT_4_BIT,
			VK_ATTACHMENT_LOAD_OP_CLEAR,
			VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		.AddSubpass()
		.AddSubpassColorAttachmentRef(0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		.AddExternalSubpassDependency(
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)
		.DebugName("raytrace.renderPass")
		.Create(fb->GetDevice());

	for (int i = 0; i < 16; i++)
	{
		raytrace.pipeline[i] = GraphicsPipelineBuilder()
			.Layout(raytrace.pipelineLayout.get())
			.RenderPass(raytrace.renderPass.get())
			.AddVertexShader(shaders.vertRaytrace)
			.AddFragmentShader(shaders.fragRaytrace[i])
			.AddVertexBufferBinding(0, sizeof(FFlatVertex))
			.AddVertexAttribute(0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0)
			.Topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
			.AddDynamicState(VK_DYNAMIC_STATE_VIEWPORT)
			.RasterizationSamples(VK_SAMPLE_COUNT_4_BIT)
			.Viewport(0.0f, 0.0f, 0.0f, 0.0f)
			.Scissor(0, 0, 4096, 4096)
			.DebugName("raytrace.pipeline")
			.Create(fb->GetDevice());
	}

	raytrace.descriptorPool0 = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6)
		.MaxSets(1)
		.DebugName("raytrace.descriptorPool0")
		.Create(fb->GetDevice());

	if (useRayQuery)
	{
		raytrace.descriptorPool1 = DescriptorPoolBuilder()
			.AddPoolSize(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1)
			.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2)
			.MaxSets(1)
			.DebugName("raytrace.descriptorPool1")
			.Create(fb->GetDevice());
	}
	else
	{
		raytrace.descriptorPool1 = DescriptorPoolBuilder()
			.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3)
			.MaxSets(1)
			.DebugName("raytrace.descriptorPool1")
			.Create(fb->GetDevice());
	}

	raytrace.descriptorSet0 = raytrace.descriptorPool0->allocate(raytrace.descriptorSetLayout0.get());
	raytrace.descriptorSet0->SetDebugName("raytrace.descriptorSet1");

	raytrace.descriptorSet1 = raytrace.descriptorPool1->allocate(raytrace.descriptorSetLayout1.get());
	raytrace.descriptorSet1->SetDebugName("raytrace.descriptorSet1");
}

void VkLightmapper::UpdateAccelStructDescriptors()
{
	if (useRayQuery)
	{
		WriteDescriptors()
			.AddAccelerationStructure(raytrace.descriptorSet1.get(), 0, fb->GetLevelMesh()->GetAccelStruct())
			.AddBuffer(raytrace.descriptorSet1.get(), 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetLevelMesh()->GetVertexBuffer())
			.AddBuffer(raytrace.descriptorSet1.get(), 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetLevelMesh()->GetIndexBuffer())
			.Execute(fb->GetDevice());
	}
	else
	{
		WriteDescriptors()
			.AddBuffer(raytrace.descriptorSet1.get(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetLevelMesh()->GetNodeBuffer())
			.AddBuffer(raytrace.descriptorSet1.get(), 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetLevelMesh()->GetVertexBuffer())
			.AddBuffer(raytrace.descriptorSet1.get(), 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetLevelMesh()->GetIndexBuffer())
			.Execute(fb->GetDevice());
	}

	WriteDescriptors()
		.AddBuffer(raytrace.descriptorSet0.get(), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uniforms.Buffer.get(), 0, sizeof(Uniforms))
		.AddBuffer(raytrace.descriptorSet0.get(), 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetLevelMesh()->GetSurfaceIndexBuffer())
		.AddBuffer(raytrace.descriptorSet0.get(), 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetLevelMesh()->GetSurfaceBuffer())
		.AddBuffer(raytrace.descriptorSet0.get(), 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetLevelMesh()->GetLightBuffer())
		.AddBuffer(raytrace.descriptorSet0.get(), 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetLevelMesh()->GetLightIndexBuffer())
		.AddBuffer(raytrace.descriptorSet0.get(), 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetLevelMesh()->GetPortalBuffer())
		.AddBuffer(raytrace.descriptorSet0.get(), 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, drawindexed.ConstantsBuffer.get(), 0, drawindexed.BufferSize * sizeof(LightmapRaytracePC))
		.Execute(fb->GetDevice());
}

void VkLightmapper::CreateResolvePipeline()
{
	resolve.descriptorSetLayout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.DebugName("resolve.descriptorSetLayout")
		.Create(fb->GetDevice());

	resolve.pipelineLayout = PipelineLayoutBuilder()
		.AddSetLayout(resolve.descriptorSetLayout.get())
		.DebugName("resolve.pipelineLayout")
		.Create(fb->GetDevice());

	resolve.renderPass = RenderPassBuilder()
		.AddAttachment(
			VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_SAMPLE_COUNT_1_BIT,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		.AddSubpass()
		.AddSubpassColorAttachmentRef(0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		.AddExternalSubpassDependency(
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)
		.DebugName("resolve.renderpass")
		.Create(fb->GetDevice());

	resolve.pipeline = GraphicsPipelineBuilder()
		.Layout(resolve.pipelineLayout.get())
		.RenderPass(resolve.renderPass.get())
		.AddVertexShader(shaders.vertScreenquad)
		.AddFragmentShader(shaders.fragResolve)
		.Topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
		.AddDynamicState(VK_DYNAMIC_STATE_VIEWPORT)
		.Viewport(0.0f, 0.0f, 0.0f, 0.0f)
		.Scissor(0, 0, 4096, 4096)
		.DebugName("resolve.pipeline")
		.Create(fb->GetDevice());

	resolve.descriptorPool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1)
		.MaxSets(1)
		.DebugName("resolve.descriptorPool")
		.Create(fb->GetDevice());

	resolve.sampler = SamplerBuilder()
		.DebugName("resolve.Sampler")
		.Create(fb->GetDevice());
}

void VkLightmapper::CreateBlurPipeline()
{
	blur.descriptorSetLayout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.DebugName("blur.descriptorSetLayout")
		.Create(fb->GetDevice());

	blur.pipelineLayout = PipelineLayoutBuilder()
		.AddSetLayout(blur.descriptorSetLayout.get())
		.DebugName("blur.pipelineLayout")
		.Create(fb->GetDevice());

	blur.renderPass = RenderPassBuilder()
		.AddAttachment(
			VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_SAMPLE_COUNT_1_BIT,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		.AddSubpass()
		.AddSubpassColorAttachmentRef(0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		.AddExternalSubpassDependency(
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)
		.DebugName("blur.renderpass")
		.Create(fb->GetDevice());

	for (int i = 0; i < 2; i++)
	{
		blur.pipeline[i] = GraphicsPipelineBuilder()
			.Layout(blur.pipelineLayout.get())
			.RenderPass(blur.renderPass.get())
			.AddVertexShader(shaders.vertScreenquad)
			.AddFragmentShader(shaders.fragBlur[i])
			.Topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
			.AddDynamicState(VK_DYNAMIC_STATE_VIEWPORT)
			.Viewport(0.0f, 0.0f, 0.0f, 0.0f)
			.Scissor(0, 0, 4096, 4096)
			.DebugName("blur.pipeline")
			.Create(fb->GetDevice());
	}

	blur.descriptorPool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2)
		.MaxSets(2)
		.DebugName("blur.descriptorPool")
		.Create(fb->GetDevice());

	blur.sampler = SamplerBuilder()
		.MinFilter(VK_FILTER_NEAREST)
		.MagFilter(VK_FILTER_NEAREST)
		.MipmapMode(VK_SAMPLER_MIPMAP_MODE_NEAREST)
		.DebugName("blur.Sampler")
		.Create(fb->GetDevice());
}

void VkLightmapper::CreateCopyPipeline()
{
	copy.descriptorSetLayout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT)
		.DebugName("copy.descriptorSetLayout")
		.Create(fb->GetDevice());

	copy.pipelineLayout = PipelineLayoutBuilder()
		.AddSetLayout(copy.descriptorSetLayout.get())
		.AddPushConstantRange(VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(LightmapCopyPC))
		.DebugName("copy.pipelineLayout")
		.Create(fb->GetDevice());

	copy.renderPass = RenderPassBuilder()
		.AddAttachment(
			VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_SAMPLE_COUNT_1_BIT,
			VK_ATTACHMENT_LOAD_OP_LOAD,
			VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		.AddAttachment(
			VK_FORMAT_R16_UINT,
			VK_SAMPLE_COUNT_1_BIT,
			VK_ATTACHMENT_LOAD_OP_LOAD,
			VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		.AddSubpass()
		.AddSubpassColorAttachmentRef(0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		.AddSubpassColorAttachmentRef(1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		.AddExternalSubpassDependency(
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)
		.DebugName("copy.renderpass")
		.Create(fb->GetDevice());

	copy.pipeline = GraphicsPipelineBuilder()
		.Layout(copy.pipelineLayout.get())
		.RenderPass(copy.renderPass.get())
		.AddVertexShader(shaders.vertCopy)
		.AddFragmentShader(shaders.fragCopy)
		.Topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
		.AddDynamicState(VK_DYNAMIC_STATE_VIEWPORT)
		.AddColorBlendAttachment(ColorBlendAttachmentBuilder().Create())
		.AddColorBlendAttachment(ColorBlendAttachmentBuilder().Create())
		.Viewport(0.0f, 0.0f, 0.0f, 0.0f)
		.Scissor(0, 0, 4096, 4096)
		.DebugName("copy.pipeline")
		.Create(fb->GetDevice());

	copy.descriptorPool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1)
		.MaxSets(1)
		.DebugName("copy.descriptorPool")
		.Create(fb->GetDevice());

	copy.sampler = SamplerBuilder()
		.MinFilter(VK_FILTER_NEAREST)
		.MagFilter(VK_FILTER_NEAREST)
		.MipmapMode(VK_SAMPLER_MIPMAP_MODE_NEAREST)
		.DebugName("copy.Sampler")
		.Create(fb->GetDevice());
}

void VkLightmapper::CreateBakeImage()
{
	int width = bakeImageSize;
	int height = bakeImageSize;

	bakeImage.raytrace.Image = ImageBuilder()
		.Usage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
		.Format(VK_FORMAT_R16G16B16A16_SFLOAT)
		.Size(width, height)
		.Samples(VK_SAMPLE_COUNT_4_BIT)
		.DebugName("LightmapImage.raytrace.Image")
		.Create(fb->GetDevice());

	bakeImage.raytrace.View = ImageViewBuilder()
		.Image(bakeImage.raytrace.Image.get(), VK_FORMAT_R16G16B16A16_SFLOAT)
		.DebugName("LightmapImage.raytrace.View")
		.Create(fb->GetDevice());

	bakeImage.raytrace.Framebuffer = FramebufferBuilder()
		.RenderPass(raytrace.renderPass.get())
		.Size(width, height)
		.AddAttachment(bakeImage.raytrace.View.get())
		.DebugName("LightmapImage.raytrace.Framebuffer")
		.Create(fb->GetDevice());

	bakeImage.resolve.Image = ImageBuilder()
		.Usage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
		.Format(VK_FORMAT_R16G16B16A16_SFLOAT)
		.Size(width, height)
		.DebugName("LightmapImage.resolve.Image")
		.Create(fb->GetDevice());

	bakeImage.resolve.View = ImageViewBuilder()
		.Image(bakeImage.resolve.Image.get(), VK_FORMAT_R16G16B16A16_SFLOAT)
		.DebugName("LightmapImage.resolve.View")
		.Create(fb->GetDevice());

	bakeImage.resolve.Framebuffer = FramebufferBuilder()
		.RenderPass(resolve.renderPass.get())
		.Size(width, height)
		.AddAttachment(bakeImage.resolve.View.get())
		.DebugName("LightmapImage.resolve.Framebuffer")
		.Create(fb->GetDevice());

	bakeImage.resolve.DescriptorSet = resolve.descriptorPool->allocate(resolve.descriptorSetLayout.get());
	bakeImage.resolve.DescriptorSet->SetDebugName("resolve.descriptorSet");


	bakeImage.blur.Image = ImageBuilder()
		.Usage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
		.Format(VK_FORMAT_R16G16B16A16_SFLOAT)
		.Size(width, height)
		.DebugName("LightmapImage.blur.Image")
		.Create(fb->GetDevice());

	bakeImage.blur.View = ImageViewBuilder()
		.Image(bakeImage.blur.Image.get(), VK_FORMAT_R16G16B16A16_SFLOAT)
		.DebugName("LightmapImage.blur.View")
		.Create(fb->GetDevice());

	bakeImage.blur.Framebuffer = FramebufferBuilder()
		.RenderPass(blur.renderPass.get())
		.Size(width, height)
		.AddAttachment(bakeImage.blur.View.get())
		.DebugName("LightmapImage.blur.Framebuffer")
		.Create(fb->GetDevice());

	bakeImage.blur.DescriptorSet[0] = blur.descriptorPool->allocate(blur.descriptorSetLayout.get());
	bakeImage.blur.DescriptorSet[0]->SetDebugName("blur.descriptorSet");

	bakeImage.blur.DescriptorSet[1] = blur.descriptorPool->allocate(blur.descriptorSetLayout.get());
	bakeImage.blur.DescriptorSet[1]->SetDebugName("blur.descriptorSet");

	bakeImage.copy.DescriptorSet = copy.descriptorPool->allocate(copy.descriptorSetLayout.get());
	bakeImage.copy.DescriptorSet->SetDebugName("copy.descriptorSet");

	WriteDescriptors()
		.AddCombinedImageSampler(bakeImage.resolve.DescriptorSet.get(), 0, bakeImage.raytrace.View.get(), resolve.sampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		.AddCombinedImageSampler(bakeImage.blur.DescriptorSet[0].get(), 0, bakeImage.resolve.View.get(), blur.sampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		.AddCombinedImageSampler(bakeImage.blur.DescriptorSet[1].get(), 0, bakeImage.blur.View.get(), blur.sampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		.AddCombinedImageSampler(bakeImage.copy.DescriptorSet.get(), 0, bakeImage.resolve.View.get(), copy.sampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		.AddBuffer(bakeImage.copy.DescriptorSet.get(), 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, copytiles.Buffer.get())
		.Execute(fb->GetDevice());
}

void VkLightmapper::CreateUniformBuffer()
{
	VkDeviceSize align = fb->GetDevice()->PhysicalDevice.Properties.Properties.limits.minUniformBufferOffsetAlignment;
	uniforms.StructStride = (sizeof(Uniforms) + align - 1) / align * align;

	uniforms.Buffer = BufferBuilder()
		.Usage(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
		.Size(uniforms.NumStructs * uniforms.StructStride)
		.DebugName("LightmapUniformBuffer")
		.Create(fb->GetDevice());

	uniforms.TransferBuffer = BufferBuilder()
		.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
		.Size(uniforms.NumStructs * uniforms.StructStride)
		.DebugName("LightmapUniformTransferBuffer")
		.Create(fb->GetDevice());
}

void VkLightmapper::CreateTileBuffer()
{
	size_t size = sizeof(CopyTileInfo) * copytiles.BufferSize;

	copytiles.Buffer = BufferBuilder()
		.Usage(
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VMA_MEMORY_USAGE_UNKNOWN, VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT)
		.MemoryType(
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
		.Size(size)
		.DebugName("CopyTileBuffer")
		.Create(fb->GetDevice());

	copytiles.Tiles = (CopyTileInfo*)copytiles.Buffer->Map(0, size);
}

void VkLightmapper::CreateDrawIndexedBuffer()
{
	size_t size1 = sizeof(VkDrawIndexedIndirectCommand) * drawindexed.BufferSize;
	size_t size2 = sizeof(LightmapRaytracePC) * drawindexed.BufferSize;

	drawindexed.CommandsBuffer = BufferBuilder()
		.Usage(
			VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
			VMA_MEMORY_USAGE_UNKNOWN, VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT)
		.MemoryType(
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
		.Size(size1)
		.DebugName("DrawIndexed.CommandsBuffer")
		.Create(fb->GetDevice());

	drawindexed.ConstantsBuffer = BufferBuilder()
		.Usage(
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VMA_MEMORY_USAGE_UNKNOWN, VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT)
		.MemoryType(
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
		.Size(size2)
		.DebugName("DrawIndexed.ConstantsBuffer")
		.Create(fb->GetDevice());

	drawindexed.Commands = (VkDrawIndexedIndirectCommand*)drawindexed.CommandsBuffer->Map(0, size1);
	drawindexed.Constants = (LightmapRaytracePC*)drawindexed.ConstantsBuffer->Map(0, size2);
}
