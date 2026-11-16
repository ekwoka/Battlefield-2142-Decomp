// Actual mesh shaders on a controlled surface. The outdoor point-light
// pass is separate from baked lighting in retail (RendDX9 0x100beac6).
#include "TestMain.h"

#ifdef BF2142_RENDER_TESTS
#include "Render/GpuContext.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace dice::hfe::render;

TEST(Render_StaticPointLightPixelsOptIn)
{
	if (!std::getenv("BF2142_RENDER_TEST_DIR")) return;
	CHECK(SDL_Init(SDL_INIT_VIDEO));
	{
		std::string error;
		auto gpu = GpuContext::create(nullptr, error);
		CHECK(gpu);
		if (!gpu) { std::fprintf(stderr, "%s\n", error.c_str()); SDL_Quit(); return; }
		CHECK(gpu->enhancedLitAvailable());
		auto target = gpu->createOffscreenTarget(33, 33);
		Vertex vertices[3] {};
		const float positions[3][2] = {{-1, -1}, {3, -1}, {-1, 3}};
		for (int i = 0; i < 3; ++i) {
			vertices[i].position[0] = positions[i][0];
			vertices[i].position[1] = positions[i][1];
			vertices[i].position[2] = 0.5f;
			vertices[i].normal[2] = -1;
			vertices[i].weight = 1;
		}
		auto* buffer = gpu->createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices, sizeof(vertices));
		CHECK(buffer);
		TransformUniforms transforms {};
		transforms.partsUsed = 1;
		// Both shader blocks start with ShadowUniforms; the additional
		// enhanced fields are zero (no AO or shadow contribution).
		std::array<float, 256> shadows {};
		const auto draw = [&](LightingUniforms light, std::array<uint8_t, 4> baked, uint8_t albedo = 128,
			std::array<uint8_t, 4> gi = {255, 255, 255, 255}) {
			const uint8_t diffuse[4] = {albedo, albedo, albedo, 255};
			auto* texture = gpu->createRgbaTexture(1, 1, diffuse);
			auto* lightmap = gpu->createRgbaTexture(1, 1, baked.data());
			auto* giMap = gpu->createRgbaTexture(1, 1, gi.data());
			CHECK(texture && lightmap);
			gpu->flushUploads();
			auto* commands = SDL_AcquireGPUCommandBuffer(gpu->device());
			SDL_GPUColorTargetInfo color {};
			color.texture = target.color;
			color.load_op = SDL_GPU_LOADOP_CLEAR;
			color.store_op = SDL_GPU_STOREOP_STORE;
			SDL_GPUDepthStencilTargetInfo depth {};
			depth.texture = target.depth;
			depth.clear_depth = 1;
			depth.load_op = SDL_GPU_LOADOP_CLEAR;
			depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
			depth.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
			depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
			auto* pass = SDL_BeginGPURenderPass(commands, &color, 1, &depth);
			SDL_BindGPUGraphicsPipeline(pass, gpu->meshPipeline());
			SDL_GPUBufferBinding binding {buffer, 0};
			SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
			SDL_PushGPUVertexUniformData(commands, 0, &transforms, pushUniformSize(transforms));
			SDL_PushGPUFragmentUniformData(commands, 0, &light, sizeof(light));
			SDL_PushGPUFragmentUniformData(commands, 1, shadows.data(), sizeof(shadows));
			SDL_GPUTextureSamplerBinding samplers[kMeshSamplers + 1];
			for (auto& sampler : samplers) sampler = {gpu->whiteTexture(), gpu->clampSampler()};
			samplers[0].texture = texture;
			samplers[1].texture = lightmap;
			samplers[4].texture = giMap;
			samplers[5] = samplers[6] = {gpu->emptyShadowMap(), gpu->shadowSampler()};
			samplers[8].texture = samplers[9].texture = gpu->flatNormalTexture();
			samplers[10].texture = gpu->whiteCubeTexture();
			SDL_BindGPUFragmentSamplers(pass, 0, samplers, kMeshSamplers + (gpu->enhancedLit() ? 1 : 0));
			SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
			SDL_EndGPURenderPass(pass);
			CHECK(SDL_SubmitGPUCommandBuffer(commands));
			std::vector<uint8_t> pixels;
			CHECK(gpu->readPixels(target, pixels));
			std::array<int, 3> rgb {};
			const size_t center = (16 * target.width + 16) * 4;
			CHECK(pixels.size() > center + 2);
			if (pixels.size() > center + 2)
				for (int c = 0; c < 3; ++c) rgb[c] = pixels[center + c];
			SDL_ReleaseGPUTexture(gpu->device(), texture);
			SDL_ReleaseGPUTexture(gpu->device(), lightmap);
			SDL_ReleaseGPUTexture(gpu->device(), giMap);
			return rgb;
		};
		const std::array<uint8_t, 4> mapA {0, 64, 128, 255}, mapB {0, 192, 255, 255};
		for (bool enhanced : {false, true}) {
			gpu->setEnhancedLit(enhanced);
			CHECK(gpu->enhancedLit() == enhanced);
			LightingUniforms light {};
			light.material[1] = 1; // keep the baked base lighting
			light.material[2] = kMaterialNoShadows;
			light.skyColor[0] = light.skyColor[1] = light.skyColor[2] = 0.2f;
			light.sunDirection[2] = -1;
			light.eye[2] = -9.5f;
			light.eye[3] = 1; // static mesh, uniform gloss
			light.lightCount[1] = 1; // static point-light pass
			light.lightPosition[0][2] = -9.5f;
			light.lightPosition[0][3] = 20;
			light.lightColor[0][0] = 0.3f;
			light.lightColor[0][1] = 0.15f;
			light.lightColor[0][2] = 0.05f;
			const auto baseA = draw(light, mapA), baseB = draw(light, mapB);
			light.lightCount[0] = 1;
			const auto litA = draw(light, mapA), litB = draw(light, mapB);
			for (int c = 0; c < 3; ++c) {
				// Changing the baked map still affects the base, but must
				// never recolour or attenuate the added outdoor light.
				CHECK(baseB[c] > baseA[c] + 10);
				CHECK(std::abs((litA[c] - baseA[c]) - (litB[c] - baseB[c])) <= 2);
			}
			CHECK(litA[0] - baseA[0] > 50); // zero baked red cannot remove red fire light
			CHECK(litA[0] - baseA[0] > (litA[1] - baseA[1]) * 1.8f);
			std::fprintf(stderr, "  %s static light: RGB increments %d/%d/%d, changed baked base %d -> %d\n",
				enhanced ? "enhanced" : "ordinary", litA[0] - baseA[0], litA[1] - baseA[1], litA[2] - baseA[2], baseA[0], baseB[0]);

			// RaShaderSTM's point-light return saturates AFTER multiplying
			// by fog visibility. Saturate first then final-fog-mix is wrong
			// for a bright light, even though weak fire lights look similar.
			light.skyColor[0] = light.skyColor[1] = light.skyColor[2] = 0;
			light.lightColor[0][0] = 4;
			light.lightColor[0][1] = light.lightColor[0][2] = 0;
			light.fogRange[3] = 0.25f; // constant 25% visibility
			const auto fogged = draw(light, mapA);
			CHECK(std::abs(fogged[0] - 192) <= 2); // 2 * (128/255) * saturate(4 * .75 * .25)
			CHECK(fogged[1] == 0 && fogged[2] == 0);
			std::fprintf(stderr, "  %s bright fogged point light: R %d (expected 192)\n", enhanced ? "enhanced" : "ordinary", fogged[0]);

			// Specular belongs to the light, is saturated with the diffuse,
			// and is multiplied by the material colour in this retail pass.
			light.fogRange[3] = 1;
			light.lightColor[0][0] = 0;
			light.lightSpot[0][0] = 0.3f;
			light.lightSpot[0][1] = 0.1f;
			light.specular[3] = 0.5f;
			const auto spec = draw(light, mapA), darkSpec = draw(light, mapA, 64);
			CHECK(std::abs(spec[0] - 29) <= 2 && std::abs(spec[1] - 10) <= 2);
			CHECK(std::abs(spec[0] - darkSpec[0] * 2) <= 2);

			// Titan interiors retain their separate lightmap mask; changing
			// the outdoor pass must not make their lights unmasked too.
			light.material[2] = kMaterialTitanInterior;
			light.specular[3] = 0;
			light.lightColor[0][0] = 0.3f;
			const auto interiorDark = draw(light, {0, 0, 0, 255});
			const auto interiorLit = draw(light, {0, 128, 0, 255});
			CHECK(interiorDark[0] == 0);
			CHECK(interiorLit[0] > 100);

			// A bundled wreck's GI map masks its base lighting and specular,
			// but RaShaderBM's separate point-light variant disables GI.
			light.material[1] = 0;
			light.material[2] = kMaterialGIMap + kMaterialNoShadows;
			light.eye[3] = 3;
			light.lightCount[1] = 2;
			light.lightPosition[0][3] = 60;
			light.skyColor[0] = light.skyColor[1] = light.skyColor[2] = 0.4f;
			light.lightCount[0] = 0;
			const auto giBase = draw(light, mapA, 128, {0, 0, 0, 255});
			const auto cleanBase = draw(light, mapA);
			light.lightCount[0] = 1;
			const auto giLit = draw(light, mapA, 128, {0, 0, 0, 255});
			const auto cleanLit = draw(light, mapA);
			for (int c = 0; c < 3; ++c) {
				CHECK(giBase[c] == 0 && cleanBase[c] > 40);
				CHECK(giLit[c] - giBase[c] > 80);
				CHECK(std::abs((giLit[c] - giBase[c]) - (cleanLit[c] - cleanBase[c])) <= 2);
			}
		}
		SDL_ReleaseGPUBuffer(gpu->device(), buffer);
		gpu->releaseTarget(target);
	}
	SDL_Quit();
}
#endif
