// Copyright (C) 2018-2020 - DevSH Graphics Programming Sp. z O.O.
// This file is part of the "Nabla Engine".
// For conditions of distribution and use, see copyright notice in nabla.h

#define _NBL_STATIC_LIB_
#include <nabla.h>

#include "../common/CommonAPI.h"
#include "nbl/ext/ScreenShot/ScreenShot.h"

using namespace nbl;
using namespace nbl::asset;
using namespace nbl::core;
using namespace nbl::video;

#define FATAL_LOG(x, ...) {logger->log(##x, system::ILogger::ELL_ERROR, __VA_ARGS__); exit(-1);}

using ScaledBoxKernel = asset::CScaledImageFilterKernel<CBoxImageFilterKernel>;
using BlitFilter = asset::CBlitImageFilter<asset::VoidSwizzle, asset::IdentityDither, void, false, ScaledBoxKernel, ScaledBoxKernel, ScaledBoxKernel>;

core::smart_refctd_ptr<ICPUImage> createCPUImage(const std::array<uint32_t, 3>& dims, const asset::IImage::E_TYPE imageType, const asset::E_FORMAT format)
{
	IImage::SCreationParams imageParams = {};
	imageParams.flags = static_cast<IImage::E_CREATE_FLAGS>(0u);
	imageParams.type = imageType;
	imageParams.format = format;
	imageParams.extent = { dims[0], dims[1], dims[2] };
	imageParams.mipLevels = 1u;
	imageParams.arrayLayers = 1u;
	imageParams.samples = asset::ICPUImage::ESCF_1_BIT;

	auto imageRegions = core::make_refctd_dynamic_array<core::smart_refctd_dynamic_array<asset::IImage::SBufferCopy>>(1ull);
	auto& region = (*imageRegions)[0];
	region.bufferImageHeight = 0u;
	region.bufferOffset = 0ull;
	region.bufferRowLength = dims[0];
	region.imageExtent = { dims[0], dims[1], dims[2] };
	region.imageOffset = { 0u, 0u, 0u };
	region.imageSubresource.baseArrayLayer = 0u;
	region.imageSubresource.layerCount = 1u;
	region.imageSubresource.mipLevel = 0;

	size_t bufferSize = asset::getTexelOrBlockBytesize(imageParams.format) * static_cast<size_t>(region.imageExtent.width) * region.imageExtent.height * region.imageExtent.depth;
	auto imageBuffer = core::make_smart_refctd_ptr<asset::ICPUBuffer>(bufferSize);
	core::smart_refctd_ptr<ICPUImage> image = ICPUImage::create(std::move(imageParams));
	image->setBufferAndRegions(core::smart_refctd_ptr(imageBuffer), imageRegions);

	return image;
}

static inline asset::IImageView<asset::ICPUImage>::E_TYPE getImageViewTypeFromImageType_CPU(const asset::IImage::E_TYPE type)
{
	switch (type)
	{
	case asset::IImage::ET_1D:
		return asset::ICPUImageView::ET_1D;
	case asset::IImage::ET_2D:
		return asset::ICPUImageView::ET_2D;
	case asset::IImage::ET_3D:
		return asset::ICPUImageView::ET_3D;
	default:
		__debugbreak();
		return static_cast<asset::IImageView<asset::ICPUImage>::E_TYPE>(0u);
	}
}

static inline video::IGPUImageView::E_TYPE getImageViewTypeFromImageType_GPU(const video::IGPUImage::E_TYPE type)
{
	switch (type)
	{
	case video::IGPUImage::ET_1D:
		return video::IGPUImageView::ET_1D;
	case video::IGPUImage::ET_2D:
		return video::IGPUImageView::ET_2D;
	case video::IGPUImage::ET_3D:
		return video::IGPUImageView::ET_3D;
	default:
		__debugbreak();
		return static_cast<video::IGPUImageView::E_TYPE>(0u);
	}
}

float computeAlphaCoverage(const double referenceAlpha, asset::ICPUImage* image)
{
	const uint32_t mipLevel = 0u;

	uint32_t alphaTestPassCount = 0u;
	const auto& extent = image->getCreationParameters().extent;

	for (uint32_t z = 0u; z < extent.depth; ++z)
	{
		for (uint32_t y = 0u; y < extent.height; ++y)
		{
			for (uint32_t x = 0u; x < extent.width; ++x)
			{
				const core::vectorSIMDu32 texCoord(x, y, z);
				core::vectorSIMDu32 dummy;
				const void* encodedPixel = image->getTexelBlockData(mipLevel, texCoord, dummy);

				double decodedPixel[4];
				asset::decodePixelsRuntime(image->getCreationParameters().format, &encodedPixel, decodedPixel, dummy.x, dummy.y);

				if (decodedPixel[3] > referenceAlpha)
					++alphaTestPassCount;
			}
		}
	}

	const float alphaCoverage = float(alphaTestPassCount) / float(extent.width * extent.height * extent.depth);
	return alphaCoverage;
};

class CBlitFilter
{
private:
	struct alignas(16) vec3_aligned
	{
		float x, y, z;
	};

	struct alignas(16) uvec3_aligned
	{
		uint32_t x, y, z;
	};

public:
	static constexpr uint32_t NBL_GLSL_DEFAULT_WORKGROUP_DIM = 16u;
	static constexpr uint32_t NBL_GLSL_DEFAULT_BIN_COUNT = 256u;

	struct alpha_test_push_constants_t
	{
		float referenceAlpha;
	};

	struct blit_push_constants_t
	{
		uvec3_aligned inDim;
		uvec3_aligned outDim;
		vec3_aligned negativeSupport;
		vec3_aligned positiveSupport;
		uvec3_aligned windowDim;
		uvec3_aligned phaseCount;
		uint32_t windowsPerWG;
	};

	struct normalization_push_constants_t
	{
		uvec3_aligned outDim;
		uint32_t inPixelCount;
		float referenceAlpha;
	};

	struct dispatch_info_t
	{
		uint32_t wgCount[3];
	};

	CBlitFilter(video::ILogicalDevice* logicalDevice, const uint32_t smemSize = 16*1024u) : sharedMemorySize(smemSize)
	{
		sampler = nullptr;
		{
			video::IGPUSampler::SParams params = {};
			params.TextureWrapU = asset::ISampler::ETC_CLAMP_TO_EDGE;
			params.TextureWrapV = asset::ISampler::ETC_CLAMP_TO_EDGE;
			params.TextureWrapW = asset::ISampler::ETC_CLAMP_TO_EDGE;
			params.BorderColor = asset::ISampler::ETBC_FLOAT_OPAQUE_BLACK;
			params.MinFilter = asset::ISampler::ETF_LINEAR;
			params.MaxFilter = asset::ISampler::ETF_LINEAR;
			params.MipmapMode = asset::ISampler::ESMM_NEAREST;
			params.AnisotropicFilter = 0u;
			params.CompareEnable = 0u;
			params.CompareFunc = asset::ISampler::ECO_ALWAYS;
			sampler = logicalDevice->createGPUSampler(std::move(params));
		}

		constexpr uint32_t BLIT_DESCRIPTOR_COUNT = 4u;
		asset::E_DESCRIPTOR_TYPE types[BLIT_DESCRIPTOR_COUNT] = { asset::EDT_COMBINED_IMAGE_SAMPLER, asset::EDT_STORAGE_IMAGE, asset::EDT_UNIFORM_BUFFER, asset::EDT_STORAGE_BUFFER }; // input image, output image, cached weights, alpha histogram
		blitDSLayout = getDSLayout(BLIT_DESCRIPTOR_COUNT, types, logicalDevice, sampler);

		asset::SPushConstantRange pcRange = {};
		{
			pcRange.stageFlags = asset::IShader::ESS_COMPUTE;
			pcRange.offset = 0u;
			pcRange.size = sizeof(blit_push_constants_t);
		}

		blitPipelineLayout = logicalDevice->createGPUPipelineLayout(&pcRange, &pcRange + 1ull, core::smart_refctd_ptr(blitDSLayout));
	}

	inline core::smart_refctd_ptr<video::IGPUDescriptorSetLayout> getDefaultBlitDSLayout() const { return blitDSLayout; }
	inline core::smart_refctd_ptr<video::IGPUPipelineLayout> getDefaultBlitPipelineLayout() const { return blitPipelineLayout; }

	inline core::smart_refctd_ptr<video::IGPUDescriptorSetLayout> getDefaultAlphaTestDSLayout(video::ILogicalDevice* logicalDevice)
	{
		if (!alphaTestDSLayout)
		{
			constexpr uint32_t DESCRIPTOR_COUNT = 2u;
			asset::E_DESCRIPTOR_TYPE types[DESCRIPTOR_COUNT] = { asset::EDT_COMBINED_IMAGE_SAMPLER, asset::EDT_STORAGE_BUFFER }; // input image, alpha test atomic counter
			alphaTestDSLayout = getDSLayout(DESCRIPTOR_COUNT, types, logicalDevice, sampler);
		}

		return alphaTestDSLayout;
	}

	inline core::smart_refctd_ptr<video::IGPUPipelineLayout> getDefaultAlphaTestPipelineLayout(video::ILogicalDevice* logicalDevice, core::smart_refctd_ptr<video::IGPUDescriptorSetLayout>&& dsLayout)
	{
		if (!alphaTestPipelineLayout)
		{
			asset::SPushConstantRange pcRange = {};
			{
				pcRange.stageFlags = asset::IShader::ESS_COMPUTE;
				pcRange.offset = 0u;
				pcRange.size = sizeof(alpha_test_push_constants_t);
			}

			alphaTestPipelineLayout = logicalDevice->createGPUPipelineLayout(&pcRange, &pcRange + 1ull, std::move(dsLayout));
		}

		return alphaTestPipelineLayout;
	}

	core::smart_refctd_ptr<video::IGPUSpecializedShader> getDefaultAlphaTestSpecializedShader(video::ILogicalDevice* logicalDevice)
	{
		if (!alphaTestSpecShader)
		{
			auto system = logicalDevice->getPhysicalDevice()->getSystem();
			system::future<core::smart_refctd_ptr<system::IFile>> future;
			const bool status = system->createFile(future, "../alpha_test_2d.comp", static_cast<system::IFile::E_CREATE_FLAGS>(system::IFile::ECF_READ | system::IFile::ECF_MAPPABLE));
			if (!status)
				return nullptr;

			auto glslFile = future.get();
			auto buffer = core::make_smart_refctd_ptr<asset::ICPUBuffer>(glslFile->getSize());
			memcpy(buffer->getPointer(), glslFile->getMappedPointer(), glslFile->getSize());

			auto cpuShader = core::make_smart_refctd_ptr<asset::ICPUShader>(std::move(buffer), asset::IShader::buffer_contains_glsl_t{}, asset::IShader::ESS_COMPUTE, "????");

			// Todo(achal): This needs to be extended to 3D images
			auto cpuShaderOverriden = asset::IGLSLCompiler::createOverridenCopy(cpuShader.get(),
				"#define _NBL_GLSL_WORKGROUP_SIZE_X_ %d\n"
				"#define _NBL_GLSL_WORKGROUP_SIZE_Y_ %d\n"
				"#define _NBL_GLSL_WORKGROUP_SIZE_Z_ %d\n",
				NBL_GLSL_DEFAULT_WORKGROUP_DIM, NBL_GLSL_DEFAULT_WORKGROUP_DIM, NBL_GLSL_DEFAULT_WORKGROUP_DIM);

			cpuShaderOverriden->setFilePathHint("../alpha_test_2d.comp");

			auto gpuUnspecShader = logicalDevice->createGPUShader(std::move(cpuShaderOverriden));

			alphaTestSpecShader = logicalDevice->createGPUSpecializedShader(gpuUnspecShader.get(), { nullptr, nullptr, "main" });
		}

		return alphaTestSpecShader;
	}

	inline core::smart_refctd_ptr<video::IGPUComputePipeline> getDefaultAlphaTestPipeline(video::ILogicalDevice* logicalDevice, core::smart_refctd_ptr<video::IGPUPipelineLayout>&& pipelineLayout, core::smart_refctd_ptr<video::IGPUSpecializedShader> specShader)
	{
		if (!alphaTestPipeline)
			alphaTestPipeline = logicalDevice->createGPUComputePipeline(nullptr, std::move(pipelineLayout), std::move(specShader));

		return alphaTestPipeline;
	}

	static void updateAlphaTestDescriptorSet(video::ILogicalDevice* logicalDevice, video::IGPUDescriptorSet* ds, core::smart_refctd_ptr<video::IGPUImageView> inImageView, core::smart_refctd_ptr<video::IGPUBuffer> alphaTestCounterBuffer)
	{
		constexpr uint32_t MAX_DESCRIPTOR_COUNT = 5u;

		const auto& bindings = ds->getLayout()->getBindings();
		const uint32_t descriptorCount = static_cast<uint32_t>(bindings.size());
		assert(descriptorCount < MAX_DESCRIPTOR_COUNT);

		video::IGPUDescriptorSet::SWriteDescriptorSet writes[MAX_DESCRIPTOR_COUNT] = {};
		video::IGPUDescriptorSet::SDescriptorInfo infos[MAX_DESCRIPTOR_COUNT] = {};

		for (uint32_t i = 0u; i < descriptorCount; ++i)
		{
			writes[i].dstSet = ds;
			writes[i].binding = i;
			writes[i].arrayElement = 0u;
			writes[i].count = 1u;
			writes[i].info = &infos[i];
			writes[i].descriptorType = bindings.begin()[i].type;
		}

		// input image
		infos[0].desc = inImageView;
		infos[0].image.imageLayout = asset::EIL_GENERAL;
		infos[0].image.sampler = nullptr;

		// alpha test counter buffer (this will be assimilated into the scratch buffer soon which will hold the space both for the histogram and this atomic counter)
		infos[1].desc = alphaTestCounterBuffer;
		infos[1].buffer.offset = 0u;
		infos[1].buffer.size = alphaTestCounterBuffer->getCachedCreationParams().declaredSize;

		logicalDevice->updateDescriptorSets(descriptorCount, writes, 0u, nullptr);
	}

	inline void buildAlphaTestParameters(const float referenceAlpha, const core::vectorSIMDu32& inImageExtent, alpha_test_push_constants_t& outPC, dispatch_info_t& outDispatchInfo)
	{
		outPC.referenceAlpha = referenceAlpha;

		const core::vectorSIMDu32 workgroupSize(NBL_GLSL_DEFAULT_WORKGROUP_DIM, NBL_GLSL_DEFAULT_WORKGROUP_DIM, NBL_GLSL_DEFAULT_WORKGROUP_DIM, 1u);
		const core::vectorSIMDu32 workgroupCount = (inImageExtent + workgroupSize - core::vectorSIMDu32(1u, 1u, 1u, 1u)) / workgroupSize;
		outDispatchInfo.wgCount[0] = workgroupCount.x;
		outDispatchInfo.wgCount[1] = workgroupCount.y;
		outDispatchInfo.wgCount[2] = workgroupCount.z;
	}

	inline core::smart_refctd_ptr<video::IGPUDescriptorSetLayout> getDefaultNormalizationDSLayout(video::ILogicalDevice* logicalDevice)
	{
		if (!normalizationDSLayout)
		{
			constexpr uint32_t DESCRIPTOR_COUNT = 3u;
			asset::E_DESCRIPTOR_TYPE types[DESCRIPTOR_COUNT] = { asset::EDT_STORAGE_IMAGE, asset::EDT_STORAGE_BUFFER, asset::EDT_STORAGE_BUFFER }; // image to normalize, alpha histogram, alpha test atomic counter
			normalizationDSLayout = getDSLayout(DESCRIPTOR_COUNT, types, logicalDevice, sampler);
		}

		return normalizationDSLayout;
	}

	inline core::smart_refctd_ptr<video::IGPUPipelineLayout> getDefaultNormalizationPipelineLayout(video::ILogicalDevice* logicalDevice, core::smart_refctd_ptr<video::IGPUDescriptorSetLayout>&& dsLayout)
	{
		if (!normalizationPipelineLayout)
		{
			asset::SPushConstantRange pcRange = {};
			{
				pcRange.stageFlags = asset::IShader::ESS_COMPUTE;
				pcRange.offset = 0u;
				pcRange.size = sizeof(normalization_push_constants_t);
			}

			normalizationPipelineLayout = logicalDevice->createGPUPipelineLayout(&pcRange, &pcRange + 1ull, std::move(dsLayout));
		}

		return normalizationPipelineLayout;
	}

	core::smart_refctd_ptr<video::IGPUSpecializedShader> getDefaultNormalizationSpecializedShader(video::ILogicalDevice* logicalDevice)
	{
		if (!normalizationSpecShader)
		{
			auto system = logicalDevice->getPhysicalDevice()->getSystem();
			system::future<core::smart_refctd_ptr<system::IFile>> future;
			const bool status = system->createFile(future, "../normalization_2d.comp", static_cast<system::IFile::E_CREATE_FLAGS>(system::IFile::ECF_READ | system::IFile::ECF_MAPPABLE));
			if (!status)
				return nullptr;

			auto glslFile = future.get();
			auto buffer = core::make_smart_refctd_ptr<asset::ICPUBuffer>(glslFile->getSize());
			memcpy(buffer->getPointer(), glslFile->getMappedPointer(), glslFile->getSize());

			auto cpuShader = core::make_smart_refctd_ptr<asset::ICPUShader>(std::move(buffer), asset::IShader::buffer_contains_glsl_t{}, asset::IShader::ESS_COMPUTE, "????");

			auto cpuShaderOverriden = asset::IGLSLCompiler::createOverridenCopy(cpuShader.get(),
				"#define _NBL_GLSL_WORKGROUP_SIZE_X_ %d\n"
				"#define _NBL_GLSL_WORKGROUP_SIZE_Y_ %d\n"
				"#define _NBL_GLSL_WORKGROUP_SIZE_Z_ %d\n"
				"#define _NBL_GLSL_BIN_COUNT_ %d\n",
				// Todo(achal): Not make DEFAULT_WG_SIZE_Z = 1
				NBL_GLSL_DEFAULT_WORKGROUP_DIM, NBL_GLSL_DEFAULT_WORKGROUP_DIM, 1u, NBL_GLSL_DEFAULT_BIN_COUNT);

			cpuShaderOverriden->setFilePathHint("../normalization_2d.comp");

			auto gpuUnspecShader = logicalDevice->createGPUShader(std::move(cpuShaderOverriden));

			normalizationSpecShader = logicalDevice->createGPUSpecializedShader(gpuUnspecShader.get(), { nullptr, nullptr, "main" });
		}

		return normalizationSpecShader;
	}

	inline core::smart_refctd_ptr<video::IGPUComputePipeline> getDefaultNormalizationPipeline(video::ILogicalDevice* logicalDevice, core::smart_refctd_ptr<video::IGPUPipelineLayout>&& pipelineLayout, core::smart_refctd_ptr<video::IGPUSpecializedShader> specShader)
	{
		if (!normalizationPipeline)
			normalizationPipeline = logicalDevice->createGPUComputePipeline(nullptr, std::move(pipelineLayout), std::move(specShader));

		return normalizationPipeline;
	}

	static void updateNormalizationDescriptorSet(video::ILogicalDevice* logicalDevice, video::IGPUDescriptorSet* ds, core::smart_refctd_ptr<video::IGPUImageView> outImageView, core::smart_refctd_ptr<video::IGPUBuffer> alphaHistogramBuffer, core::smart_refctd_ptr<video::IGPUBuffer> alphaTestCounterBuffer)
	{
		constexpr uint32_t MAX_DESCRIPTOR_COUNT = 5u;

		const auto& bindings = ds->getLayout()->getBindings();
		const uint32_t descriptorCount = static_cast<uint32_t>(bindings.size());
		assert(descriptorCount < MAX_DESCRIPTOR_COUNT);

		video::IGPUDescriptorSet::SWriteDescriptorSet writes[MAX_DESCRIPTOR_COUNT] = {};
		video::IGPUDescriptorSet::SDescriptorInfo infos[MAX_DESCRIPTOR_COUNT] = {};

		for (uint32_t i = 0u; i < descriptorCount; ++i)
		{
			writes[i].dstSet = ds;
			writes[i].binding = i;
			writes[i].arrayElement = 0u;
			writes[i].count = 1u;
			writes[i].info = &infos[i];
			writes[i].descriptorType = bindings.begin()[i].type;
		}

		// input image
		infos[0].desc = outImageView;
		infos[0].image.imageLayout = asset::EIL_GENERAL;
		infos[0].image.sampler = nullptr;

		// alpha histogram buffer
		infos[1].desc = alphaHistogramBuffer;
		infos[1].buffer.offset = 0u;
		infos[1].buffer.size = alphaHistogramBuffer->getCachedCreationParams().declaredSize;

		// alpha test counter buffer
		infos[2].desc = alphaTestCounterBuffer;
		infos[2].buffer.offset = 0u;
		infos[2].buffer.size = alphaTestCounterBuffer->getCachedCreationParams().declaredSize;

		logicalDevice->updateDescriptorSets(descriptorCount, writes, 0u, nullptr);
	}

	void buildNormalizationParameters(const core::vectorSIMDu32& inImageExtent, const core::vectorSIMDu32& outImageExtent, const float referenceAlpha, normalization_push_constants_t& outPC, dispatch_info_t& outDispatchInfo)
	{
		outPC.outDim = { outImageExtent.x, outImageExtent.y, outImageExtent.z };
		outPC.inPixelCount = inImageExtent.x * inImageExtent.y * inImageExtent.z;
		outPC.referenceAlpha = static_cast<float>(referenceAlpha);

		const core::vectorSIMDu32 workgroupSize(NBL_GLSL_DEFAULT_WORKGROUP_DIM, NBL_GLSL_DEFAULT_WORKGROUP_DIM, NBL_GLSL_DEFAULT_WORKGROUP_DIM, 1u);
		const core::vectorSIMDu32 workgroupCount = (outImageExtent + workgroupSize - core::vectorSIMDu32(1u, 1u, 1u, 1u)) / workgroupSize;

		outDispatchInfo.wgCount[0] = workgroupCount.x;
		outDispatchInfo.wgCount[1] = workgroupCount.y;
		outDispatchInfo.wgCount[2] = workgroupCount.z;
	}

	core::smart_refctd_ptr<video::IGPUSpecializedShader> getDefaultBlitSpecializedShader(video::ILogicalDevice* logicalDevice, const core::vectorSIMDu32& windowDim)
	{
		if (!blitSpecShader)
		{
			auto system = logicalDevice->getPhysicalDevice()->getSystem();
			system::future<core::smart_refctd_ptr<system::IFile>> future;
			const bool status = system->createFile(future, "../blit_2d.comp", static_cast<system::IFile::E_CREATE_FLAGS>(system::IFile::ECF_READ | system::IFile::ECF_MAPPABLE));
			if (!status)
				return nullptr;

			auto glslFile = future.get();
			auto buffer = core::make_smart_refctd_ptr<asset::ICPUBuffer>(glslFile->getSize());
			memcpy(buffer->getPointer(), glslFile->getMappedPointer(), glslFile->getSize());

			auto cpuShader = core::make_smart_refctd_ptr<asset::ICPUShader>(std::move(buffer), asset::IShader::buffer_contains_glsl_t{}, asset::IShader::ESS_COMPUTE, "????");

			auto cpuShaderOverriden = asset::IGLSLCompiler::createOverridenCopy(cpuShader.get(),
				"#define _NBL_GLSL_WORKGROUP_SIZE_ %d\n", NBL_GLSL_DEFAULT_WORKGROUP_DIM*NBL_GLSL_DEFAULT_WORKGROUP_DIM);

			cpuShaderOverriden->setFilePathHint("../blit_2d.comp");

			auto gpuUnspecShader = logicalDevice->createGPUShader(std::move(cpuShaderOverriden));

			blitSpecShader = logicalDevice->createGPUSpecializedShader(gpuUnspecShader.get(), { nullptr, nullptr, "main" });
		}

		return blitSpecShader;
	}

	inline core::smart_refctd_ptr<video::IGPUComputePipeline> getDefaultBlitPipeline(video::ILogicalDevice* logicalDevice, core::smart_refctd_ptr<video::IGPUPipelineLayout>&& pipelineLayout, core::smart_refctd_ptr<video::IGPUSpecializedShader>&& specShader)
	{
		if (!blitPipeline)
			blitPipeline = logicalDevice->createGPUComputePipeline(nullptr, std::move(pipelineLayout), std::move(specShader));

		return blitPipeline;
	}

	static void updateBlitDescriptorSet(video::ILogicalDevice* logicalDevice, video::IGPUDescriptorSet* ds, core::smart_refctd_ptr<video::IGPUImageView> inImageView, core::smart_refctd_ptr<video::IGPUImageView> outImageView, core::smart_refctd_ptr<video::IGPUBuffer> phaseSupportLUT, core::smart_refctd_ptr<video::IGPUBuffer> alphaHistogramBuffer = nullptr)
	{
		constexpr uint32_t MAX_DESCRIPTOR_COUNT = 5u;

		const auto& bindings = ds->getLayout()->getBindings();
		const uint32_t bindingCount = static_cast<uint32_t>(bindings.size());
		const uint32_t descriptorCount = alphaHistogramBuffer ? bindingCount : bindingCount - 1u;
		assert(descriptorCount < MAX_DESCRIPTOR_COUNT);

		video::IGPUDescriptorSet::SWriteDescriptorSet writes[MAX_DESCRIPTOR_COUNT] = {};
		video::IGPUDescriptorSet::SDescriptorInfo infos[MAX_DESCRIPTOR_COUNT] = {};

		for (uint32_t i = 0u; i < descriptorCount; ++i)
		{
			writes[i].dstSet = ds;
			writes[i].binding = i;
			writes[i].arrayElement = 0u;
			writes[i].count = 1u;
			writes[i].info = &infos[i];
			writes[i].descriptorType = bindings.begin()[i].type;
		}

		// input image
		infos[0].desc = inImageView;
		infos[0].image.imageLayout = asset::EIL_GENERAL; // Todo(achal): Make it not GENERAL, this is a sampled image
		infos[0].image.sampler = nullptr;

		// output image
		infos[1].desc = outImageView;
		infos[1].image.imageLayout = asset::EIL_GENERAL;
		infos[1].image.sampler = nullptr;

		// phase support LUT (cached weights)
		infos[2].desc = phaseSupportLUT;
		infos[2].buffer.offset = 0ull;
		infos[2].buffer.size = phaseSupportLUT->getCachedCreationParams().declaredSize;

		if (alphaHistogramBuffer)
		{
			infos[3].desc = alphaHistogramBuffer;
			infos[3].buffer.offset = 0ull;
			infos[3].buffer.size = alphaHistogramBuffer->getCachedCreationParams().declaredSize;
		}

		logicalDevice->updateDescriptorSets(descriptorCount, writes, 0u, nullptr);
	}

	inline void buildBlitParameters(const core::vectorSIMDu32& inImageExtent, const core::vectorSIMDu32& outImageExtent, const asset::IImage::E_TYPE inImageType, blit_push_constants_t& outPC, dispatch_info_t& outDispatchInfo)
	{
		outPC.inDim.x = inImageExtent.x; outPC.inDim.y = inImageExtent.y; outPC.inDim.z = inImageExtent.z;
		outPC.outDim.x = outImageExtent.x; outPC.outDim.y = outImageExtent.y; outPC.outDim.z = outImageExtent.z;

		core::vectorSIMDf scale = static_cast<core::vectorSIMDf>(inImageExtent).preciseDivision(static_cast<core::vectorSIMDf>(outImageExtent));
		// Todo(achal): Need to take prescaled kernels into account
		const core::vectorSIMDf negativeSupport = core::vectorSIMDf(-0.5f, -0.5f, -0.5f) * scale;
		const core::vectorSIMDf positiveSupport = core::vectorSIMDf(0.5f, 0.5f, 0.5f) * scale;

		outPC.negativeSupport.x = negativeSupport.x; outPC.negativeSupport.y = negativeSupport.y; outPC.negativeSupport.z = negativeSupport.z;
		outPC.positiveSupport.x = positiveSupport.x; outPC.positiveSupport.y = positiveSupport.y; outPC.positiveSupport.z = positiveSupport.z;

		const core::vectorSIMDu32 windowDim = static_cast<core::vectorSIMDu32>(core::ceil(scale) /* * a_factor_here_for_prescaled_kernels */);
		outPC.windowDim.x = windowDim.x; outPC.windowDim.y = windowDim.y; outPC.windowDim.z = windowDim.z;

		const core::vectorSIMDu32 phaseCount = BlitFilter::getPhaseCount(inImageExtent, outImageExtent, inImageType);
		outPC.phaseCount.x = phaseCount.x; outPC.phaseCount.y = phaseCount.y; outPC.phaseCount.z = phaseCount.z;

		const uint32_t windowPixelCount = outPC.windowDim.x * outPC.windowDim.y * outPC.windowDim.z;
		const uint32_t smemPerWindow = windowPixelCount * asset::getTexelOrBlockBytesize(asset::EF_R32G32B32A32_SFLOAT);
		outPC.windowsPerWG = sharedMemorySize / smemPerWindow;

		const uint32_t totalWindowCount = outPC.outDim.x * outPC.outDim.y * outPC.outDim.z;
		const uint32_t wgCount = (totalWindowCount + outPC.windowsPerWG - 1) / outPC.windowsPerWG;

		outDispatchInfo.wgCount[0] = wgCount;
		outDispatchInfo.wgCount[1] = 1u;
		outDispatchInfo.wgCount[2] = 1u;
	}

	template <typename push_constants_t>
	inline void dispatchHelper(video::IGPUCommandBuffer* cmdbuf, const video::IGPUPipelineLayout* pipelineLayout, const push_constants_t& pushConstants, const dispatch_info_t& dispatchInfo)
	{
		cmdbuf->pushConstants(pipelineLayout, asset::IShader::ESS_COMPUTE, 0u, sizeof(push_constants_t), &pushConstants);
		cmdbuf->dispatch(dispatchInfo.wgCount[0], dispatchInfo.wgCount[1], dispatchInfo.wgCount[2]);
	}

	inline void blit(video::IGPUCommandBuffer* cmdbuf, const BlitFilter::CState::E_ALPHA_SEMANTIC alphaSemantic,
		video::IGPUDescriptorSet* alphaTestDS,
		video::IGPUDescriptorSet* blitDS,
		video::IGPUDescriptorSet* normalizationDS,
		const core::vectorSIMDu32& inImageExtent, const asset::IImage::E_TYPE inImageType,
		core::smart_refctd_ptr<video::IGPUImage> outImage,
		const float referenceAlpha = 0.f, core::smart_refctd_ptr<video::IGPUBuffer> alphaTestCounterBuffer = nullptr)
	{
		if (alphaSemantic == BlitFilter::CState::EAS_REFERENCE_OR_COVERAGE)
		{
			cmdbuf->bindDescriptorSets(asset::EPBP_COMPUTE, alphaTestPipeline->getLayout(), 0u, 1u, &alphaTestDS);
			cmdbuf->bindComputePipeline(alphaTestPipeline.get());
			CBlitFilter::alpha_test_push_constants_t alphaTestPC;
			CBlitFilter::dispatch_info_t alphaTestDispatchInfo;
			buildAlphaTestParameters(referenceAlpha, inImageExtent, alphaTestPC, alphaTestDispatchInfo);
			dispatchHelper<CBlitFilter::alpha_test_push_constants_t>(cmdbuf, alphaTestPipeline->getLayout(), alphaTestPC, alphaTestDispatchInfo);
		}

		cmdbuf->bindDescriptorSets(asset::EPBP_COMPUTE, blitPipeline->getLayout(), 0u, 1u, &blitDS);
		cmdbuf->bindComputePipeline(blitPipeline.get());
		CBlitFilter::blit_push_constants_t blitPC;
		CBlitFilter::dispatch_info_t blitDispatchInfo;
		const core::vectorSIMDu32 outImageExtent(outImage->getCreationParameters().extent.width, outImage->getCreationParameters().extent.height, outImage->getCreationParameters().extent.depth, 1u);
		buildBlitParameters(inImageExtent, outImageExtent, inImageType, blitPC, blitDispatchInfo);
		dispatchHelper<CBlitFilter::blit_push_constants_t>(cmdbuf, blitPipeline->getLayout(), blitPC, blitDispatchInfo);

		// After this dispatch ends and finishes writing to outImage, normalize outImage
		if (alphaSemantic == BlitFilter::CState::EAS_REFERENCE_OR_COVERAGE)
		{
			// Memory dependency to ensure the alpha test pass has finished writing to alphaTestCounterBuffer
			video::IGPUCommandBuffer::SBufferMemoryBarrier alphaTestBarrier = {};
			alphaTestBarrier.barrier.srcAccessMask = asset::EAF_SHADER_WRITE_BIT;
			alphaTestBarrier.barrier.dstAccessMask = asset::EAF_SHADER_READ_BIT;
			alphaTestBarrier.srcQueueFamilyIndex = ~0u;
			alphaTestBarrier.dstQueueFamilyIndex = ~0u;
			alphaTestBarrier.buffer = alphaTestCounterBuffer;
			alphaTestBarrier.size = alphaTestCounterBuffer->getCachedCreationParams().declaredSize;

			// Memory dependency to ensure that the previous compute pass has finished writing to the output image
			video::IGPUCommandBuffer::SImageMemoryBarrier readyForNorm = {};
			readyForNorm.barrier.srcAccessMask = asset::EAF_SHADER_WRITE_BIT;
			readyForNorm.barrier.dstAccessMask = static_cast<asset::E_ACCESS_FLAGS>(asset::EAF_SHADER_READ_BIT | asset::EAF_SHADER_WRITE_BIT);
			readyForNorm.oldLayout = asset::EIL_GENERAL;
			readyForNorm.newLayout = asset::EIL_GENERAL;
			readyForNorm.srcQueueFamilyIndex = ~0u;
			readyForNorm.dstQueueFamilyIndex = ~0u;
			readyForNorm.image = outImage;
			readyForNorm.subresourceRange.aspectMask = asset::IImage::EAF_COLOR_BIT;
			readyForNorm.subresourceRange.levelCount = 1u;
			readyForNorm.subresourceRange.layerCount = 1u;
			cmdbuf->pipelineBarrier(asset::EPSF_COMPUTE_SHADER_BIT, asset::EPSF_COMPUTE_SHADER_BIT, asset::EDF_NONE, 0u, nullptr, 1u, &alphaTestBarrier, 1u, &readyForNorm);

			cmdbuf->bindDescriptorSets(asset::EPBP_COMPUTE, normalizationPipeline->getLayout(), 0u, 1u, &normalizationDS);
			cmdbuf->bindComputePipeline(normalizationPipeline.get());
			CBlitFilter::normalization_push_constants_t normPC = {};
			CBlitFilter::dispatch_info_t normDispatchInfo = {};
			buildNormalizationParameters(inImageExtent, outImageExtent, referenceAlpha, normPC, normDispatchInfo);
			dispatchHelper<CBlitFilter::normalization_push_constants_t>(cmdbuf, normalizationPipeline->getLayout(), normPC, normDispatchInfo);
		}
	}

	//! WARNING: This function blocks and stalls the GPU!
	void blit(
		video::ILogicalDevice* logicalDevice,
		video::IGPUQueue* computeQueue,
		const BlitFilter::CState::E_ALPHA_SEMANTIC alphaSemantic,
		video::IGPUDescriptorSet* alphaTestDS,
		video::IGPUDescriptorSet* blitDS,
		video::IGPUDescriptorSet* normalizationDS,
		const core::vectorSIMDu32& inImageExtent, const asset::IImage::E_TYPE inImageType,
		core::smart_refctd_ptr<video::IGPUImage> outImage,
		const float referenceAlpha = 0.f, core::smart_refctd_ptr<video::IGPUBuffer> alphaTestCounterBuffer = nullptr)
	{
		auto cmdpool = logicalDevice->createCommandPool(computeQueue->getFamilyIndex(), video::IGPUCommandPool::ECF_NONE);
		core::smart_refctd_ptr<video::IGPUCommandBuffer> cmdbuf;
		logicalDevice->createCommandBuffers(cmdpool.get(), video::IGPUCommandBuffer::EL_PRIMARY, 1u, &cmdbuf);

		auto fence = logicalDevice->createFence(video::IGPUFence::ECF_UNSIGNALED);

		cmdbuf->begin(video::IGPUCommandBuffer::EU_ONE_TIME_SUBMIT_BIT);
		blit(cmdbuf.get(), alphaSemantic, alphaTestDS, blitDS, normalizationDS, inImageExtent, inImageType, outImage, referenceAlpha, alphaTestCounterBuffer);
		cmdbuf->end();

		video::IGPUQueue::SSubmitInfo submitInfo = {};
		submitInfo.commandBufferCount = 1u;
		submitInfo.commandBuffers = &cmdbuf.get();
		computeQueue->submit(1u, &submitInfo, fence.get());

		logicalDevice->blockForFences(1u, &fence.get());
	}

private:
	core::smart_refctd_ptr<video::IGPUDescriptorSetLayout> alphaTestDSLayout = nullptr;
	core::smart_refctd_ptr<video::IGPUPipelineLayout> alphaTestPipelineLayout = nullptr;
	// Todo(achal): Make this "templated" on (just an array of all possible values, like video::CScanner does) input pixel type (float/vec2/vec3/vec4), if required
	core::smart_refctd_ptr<video::IGPUSpecializedShader> alphaTestSpecShader = nullptr;
	core::smart_refctd_ptr<video::IGPUComputePipeline> alphaTestPipeline = nullptr;

	core::smart_refctd_ptr<video::IGPUDescriptorSetLayout> blitDSLayout = nullptr;
	core::smart_refctd_ptr<video::IGPUPipelineLayout> blitPipelineLayout = nullptr;
	// Todo(achal): Make this "templated" on (just an array of all possible values, like video::CScanner does) input pixel type (float/vec2/vec3/vec4)
	core::smart_refctd_ptr<video::IGPUSpecializedShader> blitSpecShader = nullptr;
	core::smart_refctd_ptr<video::IGPUComputePipeline> blitPipeline = nullptr;

	core::smart_refctd_ptr<video::IGPUDescriptorSetLayout> normalizationDSLayout = nullptr;
	core::smart_refctd_ptr<video::IGPUPipelineLayout> normalizationPipelineLayout = nullptr;
	// Todo(achal): Make this "templated" on (just an array of all possible values, like video::CScanner does) input pixel type (float/vec2/vec3/vec4), if required
	core::smart_refctd_ptr<video::IGPUSpecializedShader> normalizationSpecShader = nullptr;
	core::smart_refctd_ptr<video::IGPUComputePipeline> normalizationPipeline = nullptr;

	const uint32_t sharedMemorySize;

	core::smart_refctd_ptr<video::IGPUSampler> sampler = nullptr;

	core::smart_refctd_ptr<video::IGPUDescriptorSetLayout> getDSLayout(const uint32_t descriptorCount, const asset::E_DESCRIPTOR_TYPE* descriptorTypes, video::ILogicalDevice* logicalDevice, core::smart_refctd_ptr<video::IGPUSampler> sampler) const
	{
		constexpr uint32_t MAX_DESCRIPTOR_COUNT = 100u;
		assert(descriptorCount < MAX_DESCRIPTOR_COUNT);

		video::IGPUDescriptorSetLayout::SBinding bindings[MAX_DESCRIPTOR_COUNT] = {};

		for (uint32_t i = 0u; i < descriptorCount; ++i)
		{
			bindings[i].binding = i;
			bindings[i].count = 1u;
			bindings[i].stageFlags = asset::IShader::ESS_COMPUTE;
			bindings[i].type = descriptorTypes[i];

			if (bindings[i].type == asset::EDT_COMBINED_IMAGE_SAMPLER)
				bindings[i].samplers = &sampler;
		}

		auto dsLayout = logicalDevice->createGPUDescriptorSetLayout(bindings, bindings + descriptorCount);
		return dsLayout;
	}
};

class BlitFilterTestApp : public ApplicationBase
{
	constexpr static uint32_t SC_IMG_COUNT = 3u;
	constexpr static uint64_t MAX_TIMEOUT = 99999999999999ull;
	constexpr static size_t MAX_SMEM_SIZE = 16ull * 1024ull; // it is probably a good idea to expose VkPhysicalDeviceLimits::maxComputeSharedMemorySize

public:
	void onAppInitialized_impl() override
	{
		CommonAPI::InitOutput initOutput;
		CommonAPI::InitWithNoExt(initOutput, video::EAT_VULKAN, "BlitFilterTest");

		system = std::move(initOutput.system);
		window = std::move(initOutput.window);
		windowCb = std::move(initOutput.windowCb);
		apiConnection = std::move(initOutput.apiConnection);
		surface = std::move(initOutput.surface);
		physicalDevice = std::move(initOutput.physicalDevice);
		logicalDevice = std::move(initOutput.logicalDevice);
		utilities = std::move(initOutput.utilities);
		queues = std::move(initOutput.queues);
		swapchain = std::move(initOutput.swapchain);
		commandPools = std::move(initOutput.commandPools);
		assetManager = std::move(initOutput.assetManager);
		cpu2gpuParams = std::move(initOutput.cpu2gpuParams);
		logger = std::move(initOutput.logger);
		inputSystem = std::move(initOutput.inputSystem);

		const BlitFilter::CState::E_ALPHA_SEMANTIC alphaSemantic = BlitFilter::CState::EAS_REFERENCE_OR_COVERAGE; // BlitFilter::CState::EAS_NONE_OR_PREMULTIPLIED;
		const double referenceAlpha = 0.5;

		const char* pathToInputImage = "alpha_test_input.exr"; // "../../media/colorexr.exr";
		core::smart_refctd_ptr<asset::ICPUImage> inImage = nullptr;
		{
			constexpr auto cachingFlags = static_cast<nbl::asset::IAssetLoader::E_CACHING_FLAGS>(nbl::asset::IAssetLoader::ECF_DONT_CACHE_REFERENCES & nbl::asset::IAssetLoader::ECF_DONT_CACHE_TOP_LEVEL);

			asset::IAssetLoader::SAssetLoadParams loadParams(0ull, nullptr, cachingFlags);
			auto cpuImageBundle = assetManager->getAsset(pathToInputImage, loadParams);
			auto cpuImageContents = cpuImageBundle.getContents();
			if (cpuImageContents.empty() || cpuImageContents.begin() == cpuImageContents.end())
				FATAL_LOG("Failed to load the image at path %s\n", pathToInputImage);

			auto asset = *cpuImageContents.begin();
			if (asset->getAssetType() == asset::IAsset::ET_IMAGE_VIEW)
				__debugbreak(); // it would be weird if the loaded image is already an image view

			inImage = core::smart_refctd_ptr_static_cast<asset::ICPUImage>(asset);
		}

		float inAlphaCoverage = 0.f;
		if (alphaSemantic == BlitFilter::CState::EAS_REFERENCE_OR_COVERAGE)
			inAlphaCoverage = computeAlphaCoverage(referenceAlpha, inImage.get());

		// std::array<uint32_t, 3> outImageDim = { inImage->getCreationParameters().extent.width, inImage->getCreationParameters().extent.height, 1u };
		std::array<uint32_t, 3> outImageDim = { inImage->getCreationParameters().extent.width/3, inImage->getCreationParameters().extent.height/7, 1u };

		const core::vectorSIMDf scaleX(1.f, 1.f, 1.f, 1.f);
		const core::vectorSIMDf scaleY(1.f, 1.f, 1.f, 1.f);
		const core::vectorSIMDf scaleZ(1.f, 1.f, 1.f, 1.f);

		auto kernelX = ScaledBoxKernel(scaleX, asset::CBoxImageFilterKernel()); // (-1/2, 1/2)
		auto kernelY = ScaledBoxKernel(scaleY, asset::CBoxImageFilterKernel()); // (-1/2, 1/2)
		auto kernelZ = ScaledBoxKernel(scaleZ, asset::CBoxImageFilterKernel()); // (-1/2, 1/2)

		BlitFilter::state_type blitFilterState(std::move(kernelX), std::move(kernelY), std::move(kernelZ));

		blitFilterState.inOffsetBaseLayer = core::vectorSIMDu32();
		blitFilterState.inExtentLayerCount = core::vectorSIMDu32(0u, 0u, 0u, inImage->getCreationParameters().arrayLayers) + inImage->getMipSize();
		blitFilterState.inImage = inImage.get();

		blitFilterState.outOffsetBaseLayer = core::vectorSIMDu32();
		const uint32_t outImageLayerCount = 1u;
		blitFilterState.outExtentLayerCount = core::vectorSIMDu32(outImageDim[0], outImageDim[1], outImageDim[2], 1u);

		blitFilterState.axisWraps[0] = asset::ISampler::ETC_CLAMP_TO_EDGE;
		blitFilterState.axisWraps[1] = asset::ISampler::ETC_CLAMP_TO_EDGE;
		blitFilterState.axisWraps[2] = asset::ISampler::ETC_CLAMP_TO_EDGE;
		blitFilterState.borderColor = asset::ISampler::E_TEXTURE_BORDER_COLOR::ETBC_FLOAT_OPAQUE_WHITE;

		blitFilterState.enableLUTUsage = true;

		blitFilterState.alphaSemantic = alphaSemantic;
		blitFilterState.alphaChannel = 3u;
		blitFilterState.alphaRefValue = referenceAlpha;

		blitFilterState.scratchMemoryByteSize = BlitFilter::getRequiredScratchByteSize(&blitFilterState);
		blitFilterState.scratchMemory = reinterpret_cast<uint8_t*>(_NBL_ALIGNED_MALLOC(blitFilterState.scratchMemoryByteSize, 32));

		blitFilterState.computePhaseSupportLUT(&blitFilterState);

		// CPU blit
		{
			printf("CPU begin..\n");

			auto outImage = createCPUImage(outImageDim, inImage->getCreationParameters().type, inImage->getCreationParameters().format);
			blitFilterState.outImage = outImage.get();

			if (!BlitFilter::execute(core::execution::par_unseq, &blitFilterState))
				printf("Blit filter just shit the bed\n");

			printf("CPU end..\n");

			float outAlphaCoverage = 0.f;
			if (alphaSemantic == BlitFilter::CState::EAS_REFERENCE_OR_COVERAGE)
				outAlphaCoverage = computeAlphaCoverage(referenceAlpha, outImage.get());

			const char* writePath = "cpu_out.exr";
			{
				// create an image view to write the image to disk
				core::smart_refctd_ptr<asset::ICPUImageView> outImageView = nullptr;
				{
					ICPUImageView::SCreationParams viewParams;
					viewParams.flags = static_cast<ICPUImageView::E_CREATE_FLAGS>(0u);
					viewParams.image = outImage;
					viewParams.format = viewParams.image->getCreationParameters().format;
					viewParams.viewType = getImageViewTypeFromImageType_CPU(viewParams.image->getCreationParameters().type);
					viewParams.subresourceRange.baseArrayLayer = 0u;
					viewParams.subresourceRange.layerCount = outImage->getCreationParameters().arrayLayers;
					viewParams.subresourceRange.baseMipLevel = 0u;
					viewParams.subresourceRange.levelCount = outImage->getCreationParameters().mipLevels;

					outImageView = ICPUImageView::create(std::move(viewParams));
				}

				asset::IAssetWriter::SAssetWriteParams wparams(outImageView.get());
				wparams.logger = logger.get();
				if (!assetManager->writeAsset(writePath, wparams))
					FATAL_LOG("Failed to write cpu image at path %s\n", writePath);
			}
		}

		// GPU blit
		{
			printf("GPU begin..\n");

			constexpr uint32_t NBL_GLSL_DEFAULT_WORKGROUP_DIM = 16u;
			constexpr uint32_t NBL_GLSL_DEFAULT_BIN_COUNT = 256u;
			constexpr size_t NBL_GLSL_DEFAULT_SMEM_SIZE = MAX_SMEM_SIZE;

			auto outImage = createCPUImage(outImageDim, inImage->getCreationParameters().type, inImage->getCreationParameters().format);

			inImage->addImageUsageFlags(asset::ICPUImage::EUF_SAMPLED_BIT);
			outImage->addImageUsageFlags(asset::ICPUImage::EUF_STORAGE_BIT);

			core::smart_refctd_ptr<video::IGPUImage> inImageGPU = nullptr;
			core::smart_refctd_ptr<video::IGPUImage> outImageGPU = nullptr;
			core::smart_refctd_ptr<video::IGPUImageView> inImageView = nullptr;
			core::smart_refctd_ptr<video::IGPUImageView> outImageView = nullptr;

			if (!getGPUImagesAndTheirViews(inImage, outImage, &inImageGPU, &outImageGPU, &inImageView, &outImageView))
				FATAL_LOG("Failed to convert CPU images to GPU images\n");

			CBlitFilter blitFilter(logicalDevice.get());

			core::smart_refctd_ptr<video::IGPUBuffer> alphaTestCounterBuffer = nullptr;
			core::smart_refctd_ptr<video::IGPUBuffer> alphaHistogramBuffer = nullptr;
			core::smart_refctd_ptr<video::IGPUDescriptorSet> alphaTestDS = nullptr;
			core::smart_refctd_ptr<video::IGPUComputePipeline> alphaTestPipeline = nullptr;
			core::smart_refctd_ptr<video::IGPUDescriptorSet> normDS = nullptr;
			core::smart_refctd_ptr<video::IGPUComputePipeline> normPipeline = nullptr;
			if (alphaSemantic == BlitFilter::CState::EAS_REFERENCE_OR_COVERAGE)
			{
				// create alphaTestCounterBuffer
				{
					const size_t neededSize = sizeof(uint32_t);

					video::IGPUBuffer::SCreationParams creationParams = {};
					creationParams.usage = static_cast<video::IGPUBuffer::E_USAGE_FLAGS>(video::IGPUBuffer::EUF_TRANSFER_DST_BIT | video::IGPUBuffer::EUF_STORAGE_BUFFER_BIT);

					alphaTestCounterBuffer = logicalDevice->createDeviceLocalGPUBufferOnDedMem(creationParams, neededSize);

					asset::SBufferRange<video::IGPUBuffer> bufferRange = {};
					bufferRange.offset = 0ull;
					bufferRange.size = alphaTestCounterBuffer->getCachedCreationParams().declaredSize;
					bufferRange.buffer = alphaTestCounterBuffer;

					const uint32_t fillValue = 0u;
					utilities->updateBufferRangeViaStagingBuffer(queues[CommonAPI::InitOutput::EQT_COMPUTE], bufferRange, &fillValue);
				}

				// create alphaHistogramBuffer
				{
					const size_t neededSize = NBL_GLSL_DEFAULT_BIN_COUNT * sizeof(uint32_t);

					video::IGPUBuffer::SCreationParams creationParams = {};
					creationParams.usage = static_cast<video::IGPUBuffer::E_USAGE_FLAGS>(video::IGPUBuffer::EUF_TRANSFER_DST_BIT | video::IGPUBuffer::EUF_STORAGE_BUFFER_BIT);

					alphaHistogramBuffer = logicalDevice->createDeviceLocalGPUBufferOnDedMem(creationParams, neededSize);

					asset::SBufferRange<video::IGPUBuffer> bufferRange = {};
					bufferRange.offset = 0ull;
					bufferRange.size = alphaHistogramBuffer->getCachedCreationParams().declaredSize;
					bufferRange.buffer = alphaHistogramBuffer;

					core::vector<uint32_t> fillValues(NBL_GLSL_DEFAULT_BIN_COUNT, 0u);
					utilities->updateBufferRangeViaStagingBuffer(queues[CommonAPI::InitOutput::EQT_COMPUTE], bufferRange, fillValues.data());
				}

				const auto& alphaTestDSLayout = blitFilter.getDefaultAlphaTestDSLayout(logicalDevice.get());
				const auto& alphaTestCompShader = blitFilter.getDefaultAlphaTestSpecializedShader(logicalDevice.get());
				const auto& alphaTestPipelineLayout = blitFilter.getDefaultAlphaTestPipelineLayout(logicalDevice.get(), core::smart_refctd_ptr(alphaTestDSLayout));
				alphaTestPipeline = blitFilter.getDefaultAlphaTestPipeline(logicalDevice.get(), core::smart_refctd_ptr(alphaTestPipelineLayout), core::smart_refctd_ptr(alphaTestCompShader));
				
				const uint32_t alphaTestDSCount = 1u;
				auto alphaTestDescriptorPool = logicalDevice->createDescriptorPoolForDSLayouts(video::IDescriptorPool::ECF_NONE, &alphaTestDSLayout.get(), &alphaTestDSLayout.get() + 1ull, &alphaTestDSCount);

				alphaTestDS = logicalDevice->createGPUDescriptorSet(alphaTestDescriptorPool.get(), core::smart_refctd_ptr(alphaTestDSLayout));
				CBlitFilter::updateAlphaTestDescriptorSet(logicalDevice.get(), alphaTestDS.get(), inImageView, alphaTestCounterBuffer);

				const auto& normDSLayout = blitFilter.getDefaultNormalizationDSLayout(logicalDevice.get());
				const auto& normCompShader = blitFilter.getDefaultNormalizationSpecializedShader(logicalDevice.get());
				const auto& normPipelineLayout = blitFilter.getDefaultNormalizationPipelineLayout(logicalDevice.get(), core::smart_refctd_ptr(normDSLayout));
				normPipeline = blitFilter.getDefaultNormalizationPipeline(logicalDevice.get(), core::smart_refctd_ptr(normPipelineLayout), core::smart_refctd_ptr(normCompShader));

				const uint32_t normDSCount = 1u;
				auto normDescriptorPool = logicalDevice->createDescriptorPoolForDSLayouts(video::IDescriptorPool::ECF_NONE, &normDSLayout.get(), &normDSLayout.get() + 1ull, &normDSCount);

				normDS = logicalDevice->createGPUDescriptorSet(normDescriptorPool.get(), core::smart_refctd_ptr(normDSLayout));
				CBlitFilter::updateNormalizationDescriptorSet(logicalDevice.get(), normDS.get(), outImageView, alphaHistogramBuffer, alphaTestCounterBuffer);
			}

			const core::vectorSIMDu32 inExtent(inImage->getCreationParameters().extent.width, inImage->getCreationParameters().extent.height, inImage->getCreationParameters().extent.depth);
			const core::vectorSIMDu32 outExtent(outImageDim[0], outImageDim[1], outImageDim[2]);
			core::vectorSIMDf scale = static_cast<core::vectorSIMDf>(inExtent).preciseDivision(static_cast<core::vectorSIMDf>(outExtent));
			
			// kernelX/Y/Z stores absolute values of support so they won't be helpful here
			const core::vectorSIMDf negativeSupport = core::vectorSIMDf(-0.5f, -0.5f, -0.5f)*scale;
			const core::vectorSIMDf positiveSupport = core::vectorSIMDf(0.5f, 0.5f, 0.5f)*scale;

			// I think this formulation is better than ceil(scaledPositiveSupport-scaledNegativeSupport) because if scaledPositiveSupport comes out a tad bit
			// greater than what it should be and if scaledNegativeSupport comes out a tad bit smaller than it should be then the distance between them would
			// become a tad bit greater than it should be and when we take a ceil it'll jump up to the next integer thus giving us 1 more than the actual window
			// size, example: 49x1 -> 7x1.
			// Also, cannot use kernelX/Y/Z.getWindowSize() here because they haven't been scaled (yet) for upscaling/downscaling
			const core::vectorSIMDu32 windowDim = static_cast<core::vectorSIMDu32>(core::ceil(scale*core::vectorSIMDf(scaleX.x, scaleY.y, scaleZ.z)));

			// fail if the window cannot be preloaded into shared memory
			const size_t windowSize = static_cast<size_t>(windowDim.x) * windowDim.y * windowDim.z * asset::getTexelOrBlockBytesize(inImage->getCreationParameters().format);
			if (windowSize > MAX_SMEM_SIZE)
				FATAL_LOG("Failed to blit because supports are too large\n");

			const auto& limits = physicalDevice->getLimits();
			constexpr uint32_t MAX_INVOCATION_COUNT = 1024u;
			// matching workgroup size DIRECTLY to windowDims for now --this does mean that unfortunately my workgroups will be sized weirdly, and sometimes
			// they could be too small as well
			const uint32_t totalInvocationCount = windowDim.x * windowDim.y * windowDim.z;
			if ((totalInvocationCount > MAX_INVOCATION_COUNT) || (windowDim.x > limits.maxWorkgroupSize[0]) || (windowDim.y > limits.maxWorkgroupSize[1]) || (windowDim.z > limits.maxWorkgroupSize[2]))
				FATAL_LOG("Failed to blit because workgroup size limit exceeded\n");

			const core::vectorSIMDu32 phaseCount = BlitFilter::getPhaseCount(inExtent, outExtent, inImage->getCreationParameters().type);
			core::smart_refctd_ptr<video::IGPUBuffer> phaseSupportLUT = nullptr;
			{
				BlitFilter::value_type* lut = reinterpret_cast<BlitFilter::value_type*>(blitFilterState.scratchMemory + BlitFilter::getPhaseSupportLUTByteOffset(&blitFilterState));

				const size_t lutSize = (static_cast<size_t>(phaseCount.x)*windowDim.x + static_cast<size_t>(phaseCount.y)*windowDim.y + static_cast<size_t>(phaseCount.z)*windowDim.z)*sizeof(float)*4ull;

				// lut has the LUT in doubles, I want it in floats
				// Todo(achal): Probably need to pack them as half floats? But they are NOT different for each channel??
				// If we're under std140 layout, wouldn't it be better just make a static array of vec4 inside the uniform block
				// since a static array of floats of the same length would take up the same amount of space?
				core::vector<float> lutInFloats(lutSize / sizeof(float));
				for (uint32_t i = 0u; i < lutInFloats.size()/4; ++i)
				{
					// losing precision here
					lutInFloats[4 * i + 0] = static_cast<float>(lut[i]);
					lutInFloats[4 * i + 1] = static_cast<float>(lut[i]);
					lutInFloats[4 * i + 2] = static_cast<float>(lut[i]);
					lutInFloats[4 * i + 3] = static_cast<float>(lut[i]);
				}

				video::IGPUBuffer::SCreationParams uboCreationParams = {};
				uboCreationParams.usage = static_cast<video::IGPUBuffer::E_USAGE_FLAGS>(video::IGPUBuffer::EUF_UNIFORM_BUFFER_BIT | video::IGPUBuffer::EUF_TRANSFER_DST_BIT);
				phaseSupportLUT = logicalDevice->createDeviceLocalGPUBufferOnDedMem(uboCreationParams, lutSize);

				// fill it up with data
				asset::SBufferRange<video::IGPUBuffer> bufferRange = {};
				bufferRange.offset = 0ull;
				bufferRange.size = lutSize;
				bufferRange.buffer = phaseSupportLUT;
				utilities->updateBufferRangeViaStagingBuffer(queues[CommonAPI::InitOutput::EQT_COMPUTE], bufferRange, lutInFloats.data());
			}

			const auto& blitDSLayout = blitFilter.getDefaultBlitDSLayout();
			const auto& blitShader = blitFilter.getDefaultBlitSpecializedShader(logicalDevice.get(), windowDim);
			const auto& blitPipelineLayout = blitFilter.getDefaultBlitPipelineLayout();
			const auto& blitPipeline = blitFilter.getDefaultBlitPipeline(logicalDevice.get(), core::smart_refctd_ptr(blitPipelineLayout), core::smart_refctd_ptr(blitShader));

			const uint32_t blitDSCount = 1u;
			auto blitDescriptorPool = logicalDevice->createDescriptorPoolForDSLayouts(video::IDescriptorPool::ECF_NONE, &blitDSLayout.get(), &blitDSLayout.get() + 1ull, &blitDSCount);

			auto blitDS = logicalDevice->createGPUDescriptorSet(blitDescriptorPool.get(), core::smart_refctd_ptr(blitDSLayout));
			CBlitFilter::updateBlitDescriptorSet(logicalDevice.get(), blitDS.get(), inImageView, outImageView, phaseSupportLUT, alphaHistogramBuffer);

			blitFilter.blit(logicalDevice.get(), queues[CommonAPI::InitOutput::EQT_COMPUTE], alphaSemantic, alphaTestDS.get(), blitDS.get(), normDS.get(),
				inExtent, inImageGPU->getCreationParameters().type, outImageGPU, referenceAlpha, alphaTestCounterBuffer);

			printf("GPU end..\n");

			const char* writePath = "gpu_out.exr";

			auto outCPUImageView = ext::ScreenShot::createScreenShot(
				logicalDevice.get(),
				queues[CommonAPI::InitOutput::EQT_COMPUTE],
				nullptr,
				outImageView.get(),
				asset::EAF_ALL_IMAGE_ACCESSES_DEVSH,
				asset::EIL_GENERAL);

			float outCoverage = 0.f;
			if (alphaSemantic == BlitFilter::CState::EAS_REFERENCE_OR_COVERAGE)
				outCoverage = computeAlphaCoverage(referenceAlpha, outCPUImageView->getCreationParameters().image.get());

			asset::IAssetWriter::SAssetWriteParams writeParams(outCPUImageView.get());
			assetManager->writeAsset(writePath, writeParams);
			
			__debugbreak();
		}

		_NBL_ALIGNED_FREE(blitFilterState.scratchMemory);
	}

	void onAppTerminated_impl() override
	{
		logicalDevice->waitIdle();
	}

	void workLoopBody() override
	{

	}

	bool keepRunning() override
	{
		return false;
	}

private:
	bool getGPUImagesAndTheirViews(
		core::smart_refctd_ptr<asset::ICPUImage> inCPU,
		core::smart_refctd_ptr<asset::ICPUImage> outCPU,
		core::smart_refctd_ptr<video::IGPUImage>* inGPU,
		core::smart_refctd_ptr<video::IGPUImage>* outGPU,
		core::smart_refctd_ptr<video::IGPUImageView>* inGPUView,
		core::smart_refctd_ptr<video::IGPUImageView>* outGPUView)
	{
		core::smart_refctd_ptr<asset::ICPUImage> tmp[2] = { inCPU, outCPU };
		cpu2gpuParams.beginCommandBuffers();
		auto gpuArray = cpu2gpu.getGPUObjectsFromAssets(tmp, tmp + 2ull, cpu2gpuParams);
		cpu2gpuParams.waitForCreationToComplete();
		if (!gpuArray || gpuArray->size() < 2ull || (!(*gpuArray)[0]))
			return false;

		*inGPU = gpuArray->begin()[0];
		*outGPU = gpuArray->begin()[1];

		// do layout transition to GENERAL
		// (I think it might be a good idea to allow the user to change asset::ICPUImage's initialLayout and have the asset converter
		// do the layout transition for them)
		core::smart_refctd_ptr<video::IGPUCommandBuffer> cmdbuf = nullptr;
		logicalDevice->createCommandBuffers(commandPools[CommonAPI::InitOutput::EQT_COMPUTE].get(), video::IGPUCommandBuffer::EL_PRIMARY, 1u, &cmdbuf);

		auto fence = logicalDevice->createFence(video::IGPUFence::ECF_UNSIGNALED);
		video::IGPUCommandBuffer::SImageMemoryBarrier barriers[2] = {};

		barriers[0].oldLayout = asset::EIL_UNDEFINED;
		barriers[0].newLayout = asset::EIL_GENERAL;
		barriers[0].srcQueueFamilyIndex = ~0u;
		barriers[0].dstQueueFamilyIndex = ~0u;
		barriers[0].image = *inGPU;
		barriers[0].subresourceRange.aspectMask = video::IGPUImage::EAF_COLOR_BIT;
		barriers[0].subresourceRange.levelCount = 1u;
		barriers[0].subresourceRange.layerCount = 1u;

		barriers[1] = barriers[0];
		barriers[1].image = *outGPU;

		cmdbuf->begin(video::IGPUCommandBuffer::EU_ONE_TIME_SUBMIT_BIT);
		cmdbuf->pipelineBarrier(asset::EPSF_TOP_OF_PIPE_BIT, asset::EPSF_BOTTOM_OF_PIPE_BIT, asset::EDF_NONE, 0u, nullptr, 0u, nullptr, 2u, barriers);
		cmdbuf->end();

		video::IGPUQueue::SSubmitInfo submitInfo = {};
		submitInfo.commandBufferCount = 1u;
		submitInfo.commandBuffers = &cmdbuf.get();
		queues[CommonAPI::InitOutput::EQT_COMPUTE]->submit(1u, &submitInfo, fence.get());
		logicalDevice->blockForFences(1u, &fence.get());

		// create views for images
		{
			video::IGPUImageView::SCreationParams inCreationParams = {};
			inCreationParams.image = *inGPU;
			inCreationParams.viewType = getImageViewTypeFromImageType_GPU((*inGPU)->getCreationParameters().type);
			inCreationParams.format = (*inGPU)->getCreationParameters().format;
			inCreationParams.subresourceRange.aspectMask = video::IGPUImage::EAF_COLOR_BIT;
			inCreationParams.subresourceRange.layerCount = 1u;
			inCreationParams.subresourceRange.levelCount = 1u;

			video::IGPUImageView::SCreationParams outCreationParams = inCreationParams;
			outCreationParams.image = *outGPU;
			outCreationParams.format = (*outGPU)->getCreationParameters().format;

			*inGPUView = logicalDevice->createGPUImageView(std::move(inCreationParams));
			*outGPUView = logicalDevice->createGPUImageView(std::move(outCreationParams));
		}

		return true;
	};

	core::smart_refctd_ptr<nbl::ui::IWindowManager> windowManager;
	core::smart_refctd_ptr<nbl::ui::IWindow> window;
	core::smart_refctd_ptr<CommonAPI::CommonAPIEventCallback> windowCb;
	core::smart_refctd_ptr<nbl::video::IAPIConnection> apiConnection;
	core::smart_refctd_ptr<nbl::video::ISurface> surface;
	core::smart_refctd_ptr<nbl::video::IUtilities> utilities;
	core::smart_refctd_ptr<nbl::video::ILogicalDevice> logicalDevice;
	video::IPhysicalDevice* physicalDevice;
	std::array<video::IGPUQueue*, CommonAPI::InitOutput::MaxQueuesCount> queues;
	core::smart_refctd_ptr<nbl::video::ISwapchain> swapchain;
	core::smart_refctd_ptr<video::IGPURenderpass> renderpass = nullptr;
	std::array<nbl::core::smart_refctd_ptr<video::IGPUFramebuffer>, CommonAPI::InitOutput::MaxSwapChainImageCount> fbos;
	std::array<nbl::core::smart_refctd_ptr<nbl::video::IGPUCommandPool>, CommonAPI::InitOutput::MaxQueuesCount> commandPools;
	core::smart_refctd_ptr<nbl::system::ISystem> system;
	core::smart_refctd_ptr<nbl::asset::IAssetManager> assetManager;
	video::IGPUObjectFromAssetConverter::SParams cpu2gpuParams;
	core::smart_refctd_ptr<nbl::system::ILogger> logger;
	core::smart_refctd_ptr<CommonAPI::InputSystem> inputSystem;
	video::IGPUObjectFromAssetConverter cpu2gpu;

public:
	void setWindow(core::smart_refctd_ptr<nbl::ui::IWindow>&& wnd) override
	{
		window = std::move(wnd);
	}
	void setSystem(core::smart_refctd_ptr<nbl::system::ISystem>&& s) override
	{
		system = std::move(s);
	}
	nbl::ui::IWindow* getWindow() override
	{
		return window.get();
	}
	video::IAPIConnection* getAPIConnection() override
	{
		return apiConnection.get();
	}
	video::ILogicalDevice* getLogicalDevice()  override
	{
		return logicalDevice.get();
	}
	video::IGPURenderpass* getRenderpass() override
	{
		return renderpass.get();
	}
	void setSurface(core::smart_refctd_ptr<video::ISurface>&& s) override
	{
		surface = std::move(s);
	}
	void setFBOs(std::vector<core::smart_refctd_ptr<video::IGPUFramebuffer>>& f) override
	{
		for (int i = 0; i < f.size(); i++)
		{
			fbos[i] = core::smart_refctd_ptr(f[i]);
		}
	}
	void setSwapchain(core::smart_refctd_ptr<video::ISwapchain>&& s) override
	{
		swapchain = std::move(s);
	}
	uint32_t getSwapchainImageCount() override
	{
		return SC_IMG_COUNT;
	}
	virtual nbl::asset::E_FORMAT getDepthFormat() override
	{
		return nbl::asset::EF_D32_SFLOAT;
	}

	APP_CONSTRUCTOR(BlitFilterTestApp);
};

NBL_COMMON_API_MAIN(BlitFilterTestApp)

extern "C" {  _declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001; }