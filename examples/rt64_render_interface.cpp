//
// RT64
//

#include "rhi/rt64_render_interface.h"

#include <cassert>
#include <cstring>
#include <chrono>
#include <functional>
#include <SDL.h>
#include <SDL_syswm.h>
#include <thread>
#include <random>

#ifdef _WIN64
#include "shaders/RenderInterfaceTestPS.hlsl.dxil.h"
#include "shaders/RenderInterfaceTestRT.hlsl.dxil.h"
#include "shaders/RenderInterfaceTestVS.hlsl.dxil.h"
#include "shaders/RenderInterfaceTestCS.hlsl.dxil.h"
#include "shaders/RenderInterfaceTestPostPS.hlsl.dxil.h"
#include "shaders/RenderInterfaceTestPostVS.hlsl.dxil.h"
#endif
#include "shaders/RenderInterfaceTestPS.hlsl.spirv.h"
#ifndef __APPLE__
#include "shaders/RenderInterfaceTestRT.hlsl.spirv.h"
#endif
#include "shaders/RenderInterfaceTestVS.hlsl.spirv.h"
#include "shaders/RenderInterfaceTestCS.hlsl.spirv.h"
#include "shaders/RenderInterfaceTestPostPS.hlsl.spirv.h"
#include "shaders/RenderInterfaceTestPostVS.hlsl.spirv.h"
#ifdef __APPLE__
#include "shaders/RenderInterfaceTestPS.hlsl.metal.h"
// TODO: Enable when RT is added to Metal.
//#include "shaders/RenderInterfaceTestRT.hlsl.metal.h"
#include "shaders/RenderInterfaceTestVS.hlsl.metal.h"
#include "shaders/RenderInterfaceTestCS.hlsl.metal.h"
#include "shaders/RenderInterfaceTestPostPS.hlsl.metal.h"
#include "shaders/RenderInterfaceTestPostVS.hlsl.metal.h"
#endif

namespace RT64 {
    static const uint32_t BufferCount = 2;
    static const RenderFormat SwapchainFormat = RenderFormat::B8G8R8A8_UNORM;
    static const uint32_t MSAACount = 4;
    static const RenderFormat ColorFormat = RenderFormat::R8G8B8A8_UNORM;
    static const RenderFormat DepthFormat = RenderFormat::D32_FLOAT;

    struct CheckeredTextureGenerator {
        static std::vector<uint8_t> generateCheckeredData(uint32_t width, uint32_t height) {
            std::vector<uint8_t> textureData(width * height * 4);
            const uint32_t squareSize = 32; // Size of each checker square
            
            for (uint32_t y = 0; y < height; y++) {
                for (uint32_t x = 0; x < width; x++) {
                    uint32_t index = (y * width + x) * 4;
                    bool isWhite = ((x / squareSize) + (y / squareSize)) % 2 == 0;
                    uint8_t pixelValue = isWhite ? 255 : 0;
                    
                    textureData[index + 0] = pixelValue;  // R
                    textureData[index + 1] = pixelValue;  // G
                    textureData[index + 2] = pixelValue;  // B
                    textureData[index + 3] = 255;         // A
                }
            }
            
            return textureData;
        }
    };

    struct RasterDescriptorSet : RenderDescriptorSetBase {
        uint32_t gSampler;
        uint32_t gTextures;
        
        std::unique_ptr<RenderSampler> linearSampler;
        
        RasterDescriptorSet(RenderDevice *device, uint32_t textureArraySize) {
            linearSampler = device->createSampler(RenderSamplerDesc());

            const uint32_t TextureArrayUpperRange = 512;
            builder.begin();
            gSampler = builder.addImmutableSampler(1, linearSampler.get());
            gTextures = builder.addTexture(2, TextureArrayUpperRange);
            builder.end(true, textureArraySize);

            create(device);
        }
    };

    struct ComputeDescriptorFirstSet : RenderDescriptorSetBase {
        uint32_t gBlueNoiseTexture;
        uint32_t gSampler;
        uint32_t gTarget;

        std::unique_ptr<RenderSampler> linearSampler;

        ComputeDescriptorFirstSet(RenderDevice *device) {
            linearSampler = device->createSampler(RenderSamplerDesc());

            builder.begin();
            gBlueNoiseTexture = builder.addTexture(1);
            gSampler = builder.addImmutableSampler(2, linearSampler.get());
            builder.end();

            create(device);
        }
    };

    struct ComputeDescriptorSecondSet : RenderDescriptorSetBase {
        uint32_t gTarget;

        ComputeDescriptorSecondSet(RenderDevice *device) {
            builder.begin();
            gTarget = builder.addReadWriteTexture(16);
            builder.end();

            create(device);
        }
    };

    struct RaytracingDescriptorSet : RenderDescriptorSetBase {
        uint32_t gBVH;
        uint32_t gOutput;
        uint32_t gBufferParams;

        RaytracingDescriptorSet(RenderDevice *device) {
            builder.begin();
            gBVH = builder.addAccelerationStructure(0);
            gOutput = builder.addReadWriteTexture(1);
            gBufferParams = builder.addStructuredBuffer(2);
            builder.end();

            create(device);
        }
    };

    struct RasterPushConstant {
        float colorAdd[4] = {};
        uint32_t textureIndex = 0;
    };

    struct ComputePushConstant {
        float Multiply[4] = {};
        uint32_t Resolution[2] = {};
    };

    enum class ShaderType {
        VERTEX,
        PIXEL,
        COMPUTE,
#ifndef __APPLE__
        RAY_TRACE,
#endif
        POST_VERTEX,
        POST_PIXEL
    };

    struct ShaderData {
        const void* blob;
        uint64_t size;
        RenderShaderFormat format;
    };

    struct TestContext {
        const RenderInterface *interface = nullptr;
        RenderWindow window;
        uint32_t swapChainTextureIndex = 0;
        std::unique_ptr<RenderDevice> device;
        std::unique_ptr<RenderCommandQueue> commandQueue;
        std::unique_ptr<RenderCommandList> commandList;
        std::unique_ptr<RenderCommandSemaphore> acquireSemaphore;
        std::unique_ptr<RenderCommandSemaphore> drawSemaphore;
        std::unique_ptr<RenderCommandFence> commandFence;
        std::unique_ptr<RenderSwapChain> swapChain;
        std::unique_ptr<RenderFramebuffer> framebuffer;
        std::vector<std::unique_ptr<RenderFramebuffer>> swapFramebuffers;
        std::unique_ptr<RenderSampler> linearSampler;
        std::unique_ptr<RenderSampler> postSampler;
        std::unique_ptr<RasterDescriptorSet> rasterSet;
        std::unique_ptr<ComputeDescriptorFirstSet> computeFirstSet;
        std::unique_ptr<ComputeDescriptorSecondSet> computeSecondSet;
        std::unique_ptr<RaytracingDescriptorSet> rtSet;
        std::unique_ptr<RenderDescriptorSet> postSet;
        std::unique_ptr<RenderPipelineLayout> rasterPipelineLayout;
        std::unique_ptr<RenderPipelineLayout> computePipelineLayout;
        std::unique_ptr<RenderPipelineLayout> rtPipelineLayout;
        std::unique_ptr<RenderPipelineLayout> postPipelineLayout;
        std::unique_ptr<RenderPipeline> rasterPipeline;
        std::unique_ptr<RenderPipeline> computePipeline;
        std::unique_ptr<RenderPipeline> rtPipeline;
        std::unique_ptr<RenderPipeline> postPipeline;
        std::unique_ptr<RenderTexture> colorTargetMS;
        std::unique_ptr<RenderTexture> colorTargetResolved;
        std::unique_ptr<RenderTexture> depthTarget;
        std::unique_ptr<RenderBuffer> uploadBuffer;
        std::unique_ptr<RenderTexture> blueNoiseTexture;
        std::unique_ptr<RenderBuffer> vertexBuffer;
        std::unique_ptr<RenderBuffer> indexBuffer;
        std::unique_ptr<RenderBuffer> rtParamsBuffer;
        std::unique_ptr<RenderBuffer> rtVertexBuffer;
        std::unique_ptr<RenderBuffer> rtScratchBuffer;
        std::unique_ptr<RenderBuffer> rtInstancesBuffer;
        std::unique_ptr<RenderBuffer> rtBottomLevelASBuffer;
        std::unique_ptr<RenderAccelerationStructure> rtBottomLevelAS;
        std::unique_ptr<RenderBuffer> rtTopLevelASBuffer;
        std::unique_ptr<RenderAccelerationStructure> rtTopLevelAS;
        std::unique_ptr<RenderBuffer> rtShaderBindingTableBuffer;
        RenderShaderBindingTableInfo rtShaderBindingTableInfo;
        RenderVertexBufferView vertexBufferView;
        RenderIndexBufferView indexBufferView;
        RenderInputSlot inputSlot;
    };
    
    struct TestBase {
        virtual void initialize(TestContext& ctx) = 0;
        virtual void resize(TestContext& ctx) = 0;
        virtual void draw(TestContext& ctx) = 0;
        virtual void shutdown(TestContext& ctx) {
            ctx.rtParamsBuffer.reset(nullptr);
            ctx.rtVertexBuffer.reset(nullptr);
            ctx.rtScratchBuffer.reset(nullptr);
            ctx.rtInstancesBuffer.reset(nullptr);
            ctx.rtBottomLevelASBuffer.reset(nullptr);
            ctx.rtTopLevelASBuffer.reset(nullptr);
            ctx.rtShaderBindingTableBuffer.reset(nullptr);
            ctx.uploadBuffer.reset(nullptr);
            ctx.blueNoiseTexture.reset(nullptr);
            ctx.vertexBuffer.reset(nullptr);
            ctx.indexBuffer.reset(nullptr);
            ctx.rasterPipeline.reset(nullptr);
            ctx.computePipeline.reset(nullptr);
            ctx.rtPipeline.reset(nullptr);
            ctx.postPipeline.reset(nullptr);
            ctx.rasterPipelineLayout.reset(nullptr);
            ctx.computePipelineLayout.reset(nullptr);
            ctx.rtPipelineLayout.reset(nullptr);
            ctx.postPipelineLayout.reset(nullptr);
            ctx.rtSet.reset(nullptr);
            ctx.rasterSet.reset(nullptr);
            ctx.computeFirstSet.reset(nullptr);
            ctx.computeSecondSet.reset(nullptr);
            ctx.postSet.reset(nullptr);
            ctx.linearSampler.reset(nullptr);
            ctx.postSampler.reset(nullptr);
            ctx.colorTargetMS.reset(nullptr);
            ctx.colorTargetResolved.reset(nullptr);
            ctx.framebuffer.reset(nullptr);
            ctx.swapFramebuffers.clear();
            ctx.commandList.reset(nullptr);
            ctx.drawSemaphore.reset(nullptr);
            ctx.acquireSemaphore.reset(nullptr);
            ctx.commandFence.reset(nullptr);
            ctx.swapChain.reset(nullptr);
            ctx.commandQueue.reset(nullptr);
            ctx.device.reset(nullptr);
        }
    };

    // Common utilities
    static ShaderData getShaderData(RenderShaderFormat format, ShaderType type) {
        ShaderData data = {};
        data.format = format;
        
        switch (format) {
#       ifdef _WIN64
            case RenderShaderFormat::DXIL:
                switch (type) {
                    case ShaderType::VERTEX:
                        data.blob = RenderInterfaceTestVSBlobDXIL;
                        data.size = sizeof(RenderInterfaceTestVSBlobDXIL);
                        break;
                    case ShaderType::PIXEL:
                        data.blob = RenderInterfaceTestPSBlobDXIL;
                        data.size = sizeof(RenderInterfaceTestPSBlobDXIL);
                        break;
                    case ShaderType::COMPUTE:
                        data.blob = RenderInterfaceTestCSBlobDXIL;
                        data.size = sizeof(RenderInterfaceTestCSBlobDXIL);
                        break;
                    case ShaderType::RAY_TRACE:
                        data.blob = RenderInterfaceTestRTBlobDXIL;
                        data.size = sizeof(RenderInterfaceTestRTBlobDXIL);
                        break;
                    case ShaderType::POST_VERTEX:
                        data.blob = RenderInterfaceTestPostVSBlobDXIL;
                        data.size = sizeof(RenderInterfaceTestPostVSBlobDXIL);
                        break;
                    case ShaderType::POST_PIXEL:
                        data.blob = RenderInterfaceTestPostPSBlobDXIL;
                        data.size = sizeof(RenderInterfaceTestPostPSBlobDXIL);
                        break;
                }
                break;
#       endif
            case RenderShaderFormat::SPIRV:
                switch (type) {
                    case ShaderType::VERTEX:
                        data.blob = RenderInterfaceTestVSBlobSPIRV;
                        data.size = sizeof(RenderInterfaceTestVSBlobSPIRV);
                        break;
                    case ShaderType::PIXEL:
                        data.blob = RenderInterfaceTestPSBlobSPIRV;
                        data.size = sizeof(RenderInterfaceTestPSBlobSPIRV);
                        break;
                    case ShaderType::COMPUTE:
                        data.blob = RenderInterfaceTestCSBlobSPIRV;
                        data.size = sizeof(RenderInterfaceTestCSBlobSPIRV);
                        break;
#               ifndef __APPLE__
                    case ShaderType::RAY_TRACE:
                        data.blob = RenderInterfaceTestRTBlobSPIRV;
                        data.size = sizeof(RenderInterfaceTestRTBlobSPIRV);
                        break;
#               endif
                    case ShaderType::POST_VERTEX:
                        data.blob = RenderInterfaceTestPostVSBlobSPIRV;
                        data.size = sizeof(RenderInterfaceTestPostVSBlobSPIRV);
                        break;
                    case ShaderType::POST_PIXEL:
                        data.blob = RenderInterfaceTestPostPSBlobSPIRV;
                        data.size = sizeof(RenderInterfaceTestPostPSBlobSPIRV);
                        break;
                }
                break;
#       ifdef __APPLE__
            case RenderShaderFormat::METAL:
                switch (type) {
                    case ShaderType::VERTEX:
                        data.blob = RenderInterfaceTestVSBlobMSL;
                        data.size = sizeof(RenderInterfaceTestVSBlobMSL);
                        break;
                    case ShaderType::PIXEL:
                        data.blob = RenderInterfaceTestPSBlobMSL;
                        data.size = sizeof(RenderInterfaceTestPSBlobMSL);
                        break;
                    case ShaderType::COMPUTE:
                        data.blob = RenderInterfaceTestCSBlobMSL;
                        data.size = sizeof(RenderInterfaceTestCSBlobMSL);
                        break;
                    case ShaderType::POST_VERTEX:
                        data.blob = RenderInterfaceTestPostVSBlobMSL;
                        data.size = sizeof(RenderInterfaceTestPostVSBlobMSL);
                        break;
                    case ShaderType::POST_PIXEL:
                        data.blob = RenderInterfaceTestPostPSBlobMSL;
                        data.size = sizeof(RenderInterfaceTestPostPSBlobMSL);
                        break;
                }
                break;
#       endif
            default:
                assert(false && "Unknown shader format.");
        }

        return data;
    }

    static void createContext(TestContext& ctx, RenderInterface* interface, RenderWindow window) {
        ctx.interface = interface;
        ctx.window = window;
        ctx.device = interface->createDevice();
        ctx.commandQueue = ctx.device->createCommandQueue(RenderCommandListType::DIRECT);
        ctx.commandList = ctx.commandQueue->createCommandList(RenderCommandListType::DIRECT);
        ctx.acquireSemaphore = ctx.device->createCommandSemaphore();
        ctx.drawSemaphore = ctx.device->createCommandSemaphore();
        ctx.commandFence = ctx.device->createCommandFence();
        ctx.swapChain = ctx.commandQueue->createSwapChain(window, BufferCount, SwapchainFormat);
    }

    static void createSwapChain(TestContext& ctx) {
        ctx.swapFramebuffers.clear();
        ctx.swapChain->resize();
        ctx.swapFramebuffers.resize(ctx.swapChain->getTextureCount());
        for (uint32_t i = 0; i < ctx.swapChain->getTextureCount(); i++) {
            const RenderTexture* curTex = ctx.swapChain->getTexture(i);
            ctx.swapFramebuffers[i] = ctx.device->createFramebuffer(RenderFramebufferDesc{&curTex, 1});
        }
    }

    static void createTargets(TestContext& ctx) {
        ctx.colorTargetMS = ctx.device->createTexture(RenderTextureDesc::ColorTarget(ctx.swapChain->getWidth(), ctx.swapChain->getHeight(), ColorFormat, RenderMultisampling(MSAACount), nullptr));
        ctx.colorTargetResolved = ctx.device->createTexture(RenderTextureDesc::ColorTarget(ctx.swapChain->getWidth(), ctx.swapChain->getHeight(), ColorFormat, 1, nullptr, RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS));
        ctx.depthTarget = ctx.device->createTexture(RenderTextureDesc::DepthTarget(ctx.swapChain->getWidth(), ctx.swapChain->getHeight(), DepthFormat, RenderMultisampling(MSAACount)));
        
        const RenderTexture *colorTargetPtr = ctx.colorTargetMS.get();
        ctx.framebuffer = ctx.device->createFramebuffer(RenderFramebufferDesc(&colorTargetPtr, 1, ctx.depthTarget.get()));
    }

    static void createRasterShader(TestContext& ctx) {
        const uint32_t textureArraySize = 3;
        ctx.rasterSet = std::make_unique<RasterDescriptorSet>(ctx.device.get(), 3);
        
        RenderPipelineLayoutBuilder layoutBuilder;
        layoutBuilder.begin(false, true);
        layoutBuilder.addPushConstant(0, 0, sizeof(RasterPushConstant), RenderShaderStageFlag::PIXEL);
        layoutBuilder.addDescriptorSet(ctx.rasterSet->builder);
        layoutBuilder.end();
        
        ctx.rasterPipelineLayout = layoutBuilder.create(ctx.device.get());
        
        // Pick shader format depending on the render interface's requirements.
        const RenderInterfaceCapabilities &interfaceCapabilities = ctx.interface->getCapabilities();
        const RenderShaderFormat shaderFormat = interfaceCapabilities.shaderFormat;
        
        ShaderData psData = getShaderData(shaderFormat, ShaderType::PIXEL);
        ShaderData vsData = getShaderData(shaderFormat, ShaderType::VERTEX);
        ShaderData postPsData = getShaderData(shaderFormat, ShaderType::POST_PIXEL);
        ShaderData postVsData = getShaderData(shaderFormat, ShaderType::POST_VERTEX);
        
        const uint32_t FloatsPerVertex = 4;
        
        ctx.inputSlot = RenderInputSlot(0, sizeof(float) * FloatsPerVertex);
        
        std::vector<RenderInputElement> inputElements;
        inputElements.emplace_back(RenderInputElement("POSITION", 0, 0, RenderFormat::R32G32_FLOAT, 0, 0));
        inputElements.emplace_back(RenderInputElement("TEXCOORD", 0, 1, RenderFormat::R32G32_FLOAT, 0, sizeof(float) * 2));
        
        std::unique_ptr<RenderShader> pixelShader = ctx.device->createShader(psData.blob, psData.size, "PSMain", shaderFormat);
        std::unique_ptr<RenderShader> vertexShader = ctx.device->createShader(vsData.blob, vsData.size, "VSMain", shaderFormat);
        
        RenderGraphicsPipelineDesc graphicsDesc;
        graphicsDesc.inputSlots = &ctx.inputSlot;
        graphicsDesc.inputSlotsCount = 1;
        graphicsDesc.inputElements = inputElements.data();
        graphicsDesc.inputElementsCount = uint32_t(inputElements.size());
        graphicsDesc.pipelineLayout = ctx.rasterPipelineLayout.get();
        graphicsDesc.pixelShader = pixelShader.get();
        graphicsDesc.vertexShader = vertexShader.get();
        graphicsDesc.renderTargetFormat[0] = ColorFormat;
        graphicsDesc.renderTargetBlend[0] = RenderBlendDesc::Copy();
        graphicsDesc.depthTargetFormat = DepthFormat;
        graphicsDesc.renderTargetCount = 1;
        graphicsDesc.multisampling.sampleCount = MSAACount;
        ctx.rasterPipeline = ctx.device->createGraphicsPipeline(graphicsDesc);

        ctx.postSampler = ctx.device->createSampler(RenderSamplerDesc());
        const RenderSampler *postSamplerPtr = ctx.postSampler.get();
        
        // Create the post processing pipeline
        std::vector<RenderDescriptorRange> postDescriptorRanges = {
            RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 1, 1),
            RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 2, 1, &postSamplerPtr)
        };
        
        RenderDescriptorSetDesc postDescriptorSetDesc(postDescriptorRanges.data(), uint32_t(postDescriptorRanges.size()));
        ctx.postSet = ctx.device->createDescriptorSet(postDescriptorSetDesc);
        ctx.postPipelineLayout = ctx.device->createPipelineLayout(RenderPipelineLayoutDesc(nullptr, 0, &postDescriptorSetDesc, 1, false, true));
        
        inputElements.clear();
        inputElements.emplace_back(RenderInputElement("POSITION", 0, 0, RenderFormat::R32G32_FLOAT, 0, 0));
        
        std::unique_ptr<RenderShader> postPixelShader = ctx.device->createShader(postPsData.blob, postPsData.size, "PSMain", postPsData.format);
        std::unique_ptr<RenderShader> postVertexShader = ctx.device->createShader(postVsData.blob, postVsData.size, "VSMain", postVsData.format);
        
        RenderGraphicsPipelineDesc postDesc;
        postDesc.inputSlots = nullptr;
        postDesc.inputSlotsCount = 0;
        postDesc.inputElements = nullptr;
        postDesc.inputElementsCount = 0;
        postDesc.pipelineLayout = ctx.postPipelineLayout.get();
        postDesc.pixelShader = postPixelShader.get();
        postDesc.vertexShader = postVertexShader.get();
        postDesc.renderTargetFormat[0] = SwapchainFormat;
        postDesc.renderTargetBlend[0] = RenderBlendDesc::Copy();
        postDesc.renderTargetCount = 1;
        ctx.postPipeline = ctx.device->createGraphicsPipeline(postDesc);
    }

    static void uploadTexture(TestContext& ctx) {
        // Upload a texture.
        const uint32_t Width = 512;
        const uint32_t Height = 512;
        const uint32_t RowLength = Width;
        const RenderFormat Format = RenderFormat::R8G8B8A8_UNORM;
        const uint32_t BufferSize = RowLength * Height * RenderFormatSize(Format);
        ctx.uploadBuffer = ctx.device->createBuffer(RenderBufferDesc::UploadBuffer(BufferSize));
        ctx.blueNoiseTexture = ctx.device->createTexture(RenderTextureDesc::Texture2D(Width, Height, 1, Format));
        ctx.rasterSet->setTexture(ctx.rasterSet->gTextures + 2, ctx.blueNoiseTexture.get(), RenderTextureLayout::SHADER_READ);
        
        // Copy to upload buffer.
        void *bufferData = ctx.uploadBuffer->map();
        auto noiseData = CheckeredTextureGenerator::generateCheckeredData(Width, Height);
        memcpy(bufferData, noiseData.data(), BufferSize);
        ctx.uploadBuffer->unmap();
        
        // Run command list to copy the upload buffer to the texture.
        ctx.commandList->begin();
        ctx.commandList->barriers(RenderBarrierStage::COPY,
                                   RenderBufferBarrier(ctx.uploadBuffer.get(), RenderBufferAccess::READ),
                                   RenderTextureBarrier(ctx.blueNoiseTexture.get(), RenderTextureLayout::COPY_DEST)
                                   );
        
        ctx.commandList->copyTextureRegion(
                                            RenderTextureCopyLocation::Subresource(ctx.blueNoiseTexture.get()),
                                            RenderTextureCopyLocation::PlacedFootprint(ctx.uploadBuffer.get(), Format, Width, Height, 1, RowLength));
        
        ctx.commandList->barriers(RenderBarrierStage::GRAPHICS_AND_COMPUTE, RenderTextureBarrier(ctx.blueNoiseTexture.get(), RenderTextureLayout::SHADER_READ));
        ctx.commandList->end();
        ctx.commandQueue->executeCommandLists(ctx.commandList.get(), ctx.commandFence.get());
        ctx.commandQueue->waitForCommandFence(ctx.commandFence.get());
    }

    static void createVertexBuffer(TestContext& ctx) {
        const uint32_t VertexCount = 3;
        const uint32_t FloatsPerVertex = 4;
        const float Vertices[VertexCount * FloatsPerVertex] = {
            -0.5f, -0.25f, 0.0f, 0.0f,
            0.5f, -0.25f, 1.0f, 0.0f,
            0.25f, 0.25f, 0.0f, 1.0f
        };
        
        const uint32_t Indices[3] = {
            0, 1, 2
        };
        
        ctx.vertexBuffer = ctx.device->createBuffer(RenderBufferDesc::VertexBuffer(sizeof(Vertices), RenderHeapType::UPLOAD));
        void *dstData = ctx.vertexBuffer->map();
        memcpy(dstData, Vertices, sizeof(Vertices));
        ctx.vertexBuffer->unmap();
        ctx.vertexBufferView = RenderVertexBufferView(ctx.vertexBuffer.get(), sizeof(Vertices));
        
        ctx.indexBuffer = ctx.device->createBuffer(RenderBufferDesc::IndexBuffer(sizeof(Indices), RenderHeapType::UPLOAD));
        dstData = ctx.indexBuffer->map();
        memcpy(dstData, Indices, sizeof(Indices));
        ctx.indexBuffer->unmap();
        ctx.indexBufferView = RenderIndexBufferView(ctx.indexBuffer.get(), sizeof(Indices), RenderFormat::R32_UINT);
    }

    static void createComputePipeline(TestContext& ctx) {
        ctx.computeFirstSet = std::make_unique<ComputeDescriptorFirstSet>(ctx.device.get());
        ctx.computeSecondSet = std::make_unique<ComputeDescriptorSecondSet>(ctx.device.get());
        ctx.computeFirstSet->setTexture(ctx.computeFirstSet->gBlueNoiseTexture, ctx.blueNoiseTexture.get(), RenderTextureLayout::SHADER_READ);
        
        RenderPipelineLayoutBuilder layoutBuilder;
        layoutBuilder.begin();
        layoutBuilder.addPushConstant(0, 0, sizeof(ComputePushConstant), RenderShaderStageFlag::COMPUTE);
        layoutBuilder.addDescriptorSet(ctx.computeFirstSet->builder);
        layoutBuilder.addDescriptorSet(ctx.computeSecondSet->builder);
        layoutBuilder.end();
        
        ctx.computePipelineLayout = layoutBuilder.create(ctx.device.get());
        
        ShaderData computeData = getShaderData(ctx.interface->getCapabilities().shaderFormat, ShaderType::COMPUTE);
        std::unique_ptr<RenderShader> computeShader = ctx.device->createShader(computeData.blob, computeData.size, "CSMain", computeData.format);
        RenderComputePipelineDesc computeDesc;
        computeDesc.computeShader = computeShader.get();
        computeDesc.pipelineLayout = ctx.computePipelineLayout.get();
        ctx.computePipeline = ctx.device->createComputePipeline(computeDesc);
    }

    static void presentSwapChain(TestContext& ctx) {
        const RenderCommandList* cmdList = ctx.commandList.get();
        RenderCommandSemaphore* waitSemaphore = ctx.acquireSemaphore.get();
        RenderCommandSemaphore* signalSemaphore = ctx.drawSemaphore.get();
        
        ctx.commandQueue->executeCommandLists(&cmdList, 1, &waitSemaphore, 1, &signalSemaphore, 1, ctx.commandFence.get());
        ctx.swapChain->present(ctx.swapChainTextureIndex, &signalSemaphore, 1);
        ctx.commandQueue->waitForCommandFence(ctx.commandFence.get());
    }

    static void initializeRenderTargets(TestContext& ctx) {
        const uint32_t width = ctx.swapChain->getWidth();
        const uint32_t height = ctx.swapChain->getHeight();
        const RenderViewport viewport(0.0f, 0.0f, float(width), float(height));
        const RenderRect scissor(0, 0, width, height);
        
        ctx.commandList->setViewports(viewport);
        ctx.commandList->setScissors(scissor);
        ctx.commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(ctx.colorTargetMS.get(), RenderTextureLayout::COLOR_WRITE));
        ctx.commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(ctx.depthTarget.get(), RenderTextureLayout::DEPTH_WRITE));
        ctx.commandList->setFramebuffer(ctx.framebuffer.get());
        
        // Clear full screen
        ctx.commandList->clearColor(0, RenderColor(0.0f, 0.0f, 0.5f)); // Clear to blue
        
        // Clear with rects
        std::vector<RenderRect> clearRects = {
            {0, 0, 100, 100},
            {200, 200, 300, 300},
            {400, 400, 500, 500}
        };
        ctx.commandList->clearColor(0, RenderColor(0.0f, 1.0f, 0.5f), clearRects.data(), clearRects.size()); // Clear to green
        
        // Clear depth buffer
        ctx.commandList->clearDepth();
    }

    static void resolveMultisampledTexture(TestContext& ctx) {
        ctx.commandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(ctx.colorTargetMS.get(), RenderTextureLayout::RESOLVE_SOURCE));
        ctx.commandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(ctx.colorTargetResolved.get(), RenderTextureLayout::RESOLVE_DEST));
        ctx.commandList->resolveTexture(ctx.colorTargetResolved.get(), ctx.colorTargetMS.get());
    }

    static void applyPostProcessToSwapChain(TestContext& ctx) {
        const uint32_t width = ctx.swapChain->getWidth();
        const uint32_t height = ctx.swapChain->getHeight();
        const RenderViewport viewport(0.0f, 0.0f, float(width), float(height));
        const RenderRect scissor(0, 0, width, height);
        
        ctx.swapChain->acquireTexture(ctx.acquireSemaphore.get(), &ctx.swapChainTextureIndex);
        RenderTexture *swapChainTexture = ctx.swapChain->getTexture(ctx.swapChainTextureIndex);
        RenderFramebuffer *swapFramebuffer = ctx.swapFramebuffers[ctx.swapChainTextureIndex].get();
        ctx.commandList->setViewports(viewport);
        ctx.commandList->setScissors(scissor);
        ctx.commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(swapChainTexture, RenderTextureLayout::COLOR_WRITE));
        ctx.commandList->setFramebuffer(swapFramebuffer);
        
        ctx.commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(ctx.colorTargetResolved.get(), RenderTextureLayout::SHADER_READ));
        ctx.commandList->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f));
        ctx.commandList->setPipeline(ctx.postPipeline.get());
        ctx.commandList->setGraphicsPipelineLayout(ctx.postPipelineLayout.get());
        ctx.postSet->setTexture(0, ctx.colorTargetResolved.get(), RenderTextureLayout::SHADER_READ);
        ctx.commandList->setGraphicsDescriptorSet(ctx.postSet.get(), 0);
        ctx.commandList->drawInstanced(3, 1, 0, 0);
        ctx.commandList->barriers(RenderBarrierStage::NONE, RenderTextureBarrier(swapChainTexture, RenderTextureLayout::PRESENT));
    }

    static void setupRasterPipeline(TestContext& ctx) {
        RasterPushConstant pushConstant;
        pushConstant.colorAdd[0] = 0.5f;
        pushConstant.colorAdd[1] = 0.25f;
        pushConstant.colorAdd[2] = 0.0f;
        pushConstant.colorAdd[3] = 0.0f;
        pushConstant.textureIndex = 2;
        ctx.commandList->setPipeline(ctx.rasterPipeline.get());
        ctx.commandList->setGraphicsPipelineLayout(ctx.rasterPipelineLayout.get());
        ctx.commandList->setGraphicsPushConstants(0, &pushConstant);
    }

    static void drawRasterShader(TestContext& ctx) {
        ctx.commandList->setVertexBuffers(0, &ctx.vertexBufferView, 1, &ctx.inputSlot);
        ctx.commandList->setIndexBuffer(&ctx.indexBufferView);
        ctx.commandList->drawInstanced(3, 1, 0, 0);
        ctx.commandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(ctx.depthTarget.get(), RenderTextureLayout::DEPTH_READ));
    }

    static void dispatchCompute(TestContext& ctx) {
        const uint32_t GroupCount = 8;
        const uint32_t width = ctx.swapChain->getWidth();
        const uint32_t height = ctx.swapChain->getHeight();
        
        ComputePushConstant pushConstant;
        pushConstant.Resolution[0] = width;
        pushConstant.Resolution[1] = height;
        pushConstant.Multiply[0] = 0.5f;
        pushConstant.Multiply[1] = 0.5f;
        pushConstant.Multiply[2] = 1.0f;
        pushConstant.Multiply[3] = 1.0f;
        ctx.commandList->setPipeline(ctx.computePipeline.get());
        ctx.commandList->setComputePipelineLayout(ctx.computePipelineLayout.get());
        ctx.commandList->setComputePushConstants(0, &pushConstant);
        ctx.commandList->setComputeDescriptorSet(ctx.computeFirstSet->get(), 0);
        ctx.commandList->setComputeDescriptorSet(ctx.computeSecondSet->get(), 1);
        ctx.commandList->dispatch((width + GroupCount - 1) / GroupCount, (height + GroupCount - 1) / GroupCount, 1);
    }

    struct ClearTest : public TestBase {
        void initialize(TestContext& ctx) override {
            resize(ctx);
        }

        void resize(TestContext& ctx) override {
            createSwapChain(ctx);
            createTargets(ctx);
        }

        void draw(TestContext& ctx) override {
            ctx.commandList->begin();
            initializeRenderTargets(ctx);
            resolveMultisampledTexture(ctx);
            applyPostProcessToSwapChain(ctx);
            ctx.commandList->end();
            presentSwapChain(ctx);
        }
    };

    struct RasterTest : public TestBase {
        void initialize(TestContext& ctx) override {
            createRasterShader(ctx);
            createVertexBuffer(ctx);
            resize(ctx);
        }
        
        void resize(TestContext& ctx) override {
            createSwapChain(ctx);
            createTargets(ctx);
        }

        void draw(TestContext& ctx) override {
            ctx.commandList->begin();
            initializeRenderTargets(ctx);
            setupRasterPipeline(ctx);
            drawRasterShader(ctx);
            resolveMultisampledTexture(ctx);
            applyPostProcessToSwapChain(ctx);
            ctx.commandList->end();
            presentSwapChain(ctx);
        }
    };

    struct TextureTest : public TestBase {
        void initialize(TestContext& ctx) override {
            createRasterShader(ctx);
            uploadTexture(ctx);
            createVertexBuffer(ctx);
            resize(ctx);
        }
        
        void resize(TestContext& ctx) override {
            createSwapChain(ctx);
            createTargets(ctx);
        }

        void draw(TestContext& ctx) override {
            ctx.commandList->begin();
            initializeRenderTargets(ctx);
            setupRasterPipeline(ctx);
            ctx.commandList->setGraphicsDescriptorSet(ctx.rasterSet->get(), 0);
            drawRasterShader(ctx);
            resolveMultisampledTexture(ctx);
            applyPostProcessToSwapChain(ctx);
            ctx.commandList->end();
            presentSwapChain(ctx);
        }
    };

    struct ComputeTest : public TestBase {
        void initialize(TestContext& ctx) override {
            createRasterShader(ctx);
            uploadTexture(ctx);
            createVertexBuffer(ctx);
            createComputePipeline(ctx);
            resize(ctx);
        }
        
        void resize(TestContext& ctx) override {
            createSwapChain(ctx);
            createTargets(ctx);
            ctx.computeSecondSet->setTexture(ctx.computeSecondSet->gTarget, ctx.colorTargetResolved.get(), RenderTextureLayout::GENERAL);
        }

        void draw(TestContext& ctx) override {
            ctx.commandList->begin();
            initializeRenderTargets(ctx);
            setupRasterPipeline(ctx);
            ctx.commandList->setGraphicsDescriptorSet(ctx.rasterSet->get(), 0);
            drawRasterShader(ctx);
            resolveMultisampledTexture(ctx);
            ctx.commandList->barriers(RenderBarrierStage::COMPUTE, RenderTextureBarrier(ctx.colorTargetResolved.get(), RenderTextureLayout::GENERAL));
            dispatchCompute(ctx);
            applyPostProcessToSwapChain(ctx);
            ctx.commandList->end();
            presentSwapChain(ctx);
        }
    };

    // Test registration and management
    using TestSetupFunc = std::function<std::unique_ptr<TestBase>()>;
    static std::vector<TestSetupFunc> g_Tests;
    static std::unique_ptr<TestBase> g_CurrentTest;
    static uint32_t g_CurrentTestIndex = 2;
    
    void RegisterTests() {
        g_Tests.push_back([]() { return std::make_unique<ClearTest>(); });
        g_Tests.push_back([]() { return std::make_unique<RasterTest>(); });
        g_Tests.push_back([]() { return std::make_unique<TextureTest>(); });
        g_Tests.push_back([]() { return std::make_unique<ComputeTest>(); });
    }

    // Update platform specific code to use the new test framework
#if defined(_WIN64)
    static LRESULT CALLBACK TestWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CLOSE:
            PostQuitMessage(0);
            break;
        case WM_SIZE:
            if (g_CurrentTest) g_CurrentTest->resize();
            return 0;
        case WM_PAINT:
            if (g_CurrentTest) g_CurrentTest->execute();
            return 0;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        return 0;
    }

    static HWND TestCreateWindow() {
        // Register window class.
        WNDCLASS wc;
        memset(&wc, 0, sizeof(WNDCLASS));
        wc.lpfnWndProc = TestWndProc;
        wc.hInstance = GetModuleHandle(0);
        wc.hbrBackground = (HBRUSH)(COLOR_BACKGROUND);
        wc.lpszClassName = "RenderInterfaceTest";
        RegisterClass(&wc);

        // Create window.
        const int Width = 1280;
        const int Height = 720;
        RECT rect;
        UINT dwStyle = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        rect.left = (GetSystemMetrics(SM_CXSCREEN) - Width) / 2;
        rect.top = (GetSystemMetrics(SM_CYSCREEN) - Height) / 2;
        rect.right = rect.left + Width;
        rect.bottom = rect.top + Height;
        AdjustWindowRectEx(&rect, dwStyle, 0, 0);

        return CreateWindow(wc.lpszClassName, "Render Interface Test", dwStyle, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, 0, 0, wc.hInstance, NULL);
    }

    void RenderInterfaceTest(RenderInterface *renderInterface) {
        RegisterTests();
        HWND hwnd = TestCreateWindow();
        
        g_CurrentTest = g_Tests[g_CurrentTestIndex]();
        g_CurrentTest->initialize(renderInterface, hwnd);

        MSG msg = {};
        while (msg.message != WM_QUIT) {
            if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }

        g_CurrentTest->shutdown();
        DestroyWindow(hwnd);
    }
#elif defined(__ANDROID__)
    void RenderInterfaceTest(RenderInterface* renderInterface) {
        assert(false);
    }
#elif defined(__linux__)
    void RenderInterfaceTest(RenderInterface* renderInterface) {
        RegisterTests();
        Display* display = XOpenDisplay(nullptr);
        int blackColor = BlackPixel(display, DefaultScreen(display));
        Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 
            0, 0, 1280, 720, 0, blackColor, blackColor);
        

          XSelectInput(display, window, StructureNotifyMask);
        // Map the window and wait for the notify event to come in.
        XMapWindow(display, window);
        while (true) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == MapNotify) {
                break;
            }
        }

        // Set up the delete window protocol.
        Atom wmDeleteMessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display, window, &wmDeleteMessage, 1);

        g_CurrentTest = g_Tests[g_CurrentTestIndex]();
        g_CurrentTest->initialize(renderInterface, {display, window});
        g_CurrentTest->resize();
        g_CurrentTest->execute();

        // Loop until the window is closed.
        std::chrono::system_clock::time_point prev_frame = std::chrono::system_clock::now();
        bool running = true;
        while (running) {
            if (XPending(display) > 0) {
                XEvent event;
                XNextEvent(display, &event);

                switch (event.type) {
                    case Expose:
                        g_CurrentTest->execute();
                        break;

                    case ClientMessage:
                        if (event.xclient.data.l[0] == wmDeleteMessage)
                            running = false;
                        break;

                    default:
                        break;
                }
            }

            using namespace std::chrono_literals;
            std::this_thread::sleep_for(1ms);
            auto now_time = std::chrono::system_clock::now();
            if (now_time - prev_frame > 16666us) {
                prev_frame = now_time;
                g_CurrentTest->draw();
            }
        }

        g_CurrentTest->shutdown();
        XDestroyWindow(display, window);
    }
#elif defined(__APPLE__)
    void RenderInterfaceTest(RenderInterface* renderInterface) {
        RegisterTests();
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
            return;
        }

        SDL_Window *window = SDL_CreateWindow("Render Interface Test", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_METAL);
        if (window == nullptr) {
            fprintf(stderr, "SDL_CreateWindow Error: %s\n", SDL_GetError());
            SDL_Quit();
            return;
        }

        // Setup Metal view.
        SDL_MetalView view = SDL_Metal_CreateView(window);

        // SDL_Window's handle can be used directly if needed
        SDL_SysWMinfo wmInfo;
        SDL_VERSION(&wmInfo.version);
        SDL_GetWindowWMInfo(window, &wmInfo);
        
        TestContext g_TestContext;
        createContext(g_TestContext, renderInterface, { wmInfo.info.cocoa.window, SDL_Metal_GetLayer(view) });

        g_CurrentTest = g_Tests[g_CurrentTestIndex]();
        g_CurrentTest->initialize(g_TestContext);

        std::chrono::system_clock::time_point prev_frame = std::chrono::system_clock::now();
        bool running = true;
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                    case SDL_QUIT:
                        running = false;
                        break;
                    case SDL_WINDOWEVENT:
                        if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                            g_CurrentTest->resize(g_TestContext);
                        }
                        break;
                }
            }

            using namespace std::chrono_literals;
            std::this_thread::sleep_for(1ms);
            auto now_time = std::chrono::system_clock::now();
            if (now_time - prev_frame > 16666us) {
                prev_frame = now_time;
                g_CurrentTest->draw(g_TestContext);
            }
        }

        g_CurrentTest->shutdown(g_TestContext);
        SDL_Metal_DestroyView(view);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
#endif
};