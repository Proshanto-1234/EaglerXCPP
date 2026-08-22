#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <stdexcept>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

class GPUChunkSerializer {
private:
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_deviceContext;
    ComPtr<ID3D11ComputeShader> m_computeShader;
    ComPtr<ID3D11Buffer> m_inputBuffer;
    ComPtr<ID3D11Buffer> m_outputBuffer;
    ComPtr<ID3D11Buffer> m_stagingBuffer;
    ComPtr<ID3D11UnorderedAccessView> m_inputUAV;
    ComPtr<ID3D11UnorderedAccessView> m_outputUAV;
    bool m_isInitialized = false;
    bool m_isAvailable = false;

    static constexpr const char* COMPUTE_SHADER_SOURCE = R"(
        RWByteAddressBuffer inputBlocks : register(u0);
        RWByteAddressBuffer outputPacked : register(u1);

        [numthreads(256, 1, 1)]
        void ChunkPackKernel(uint3 DTid : SV_DispatchThreadID)
        {
            uint gid = DTid.x;
            if (gid >= 4096) return;

            uint blockId = (inputBlocks.Load(gid) & 0xFFu) << 4u;

            uint bitPos = gid * 13u;
            uint dwordIndex = bitPos / 32u;
            uint bitOffset = bitPos % 32u;

            uint shiftedVal = blockId << bitOffset;
            outputPacked.InterlockedOr(dwordIndex * 4u, shiftedVal);

            if (bitOffset > 19u) {
                uint overflowVal = blockId >> (32u - bitOffset);
                outputPacked.InterlockedOr((dwordIndex + 1u) * 4u, overflowVal);
            }
        }
    )";

public:
    GPUChunkSerializer() = default;
    ~GPUChunkSerializer() = default;

    bool Initialize() {
        if (m_isInitialized) return m_isAvailable;

        D3D_FEATURE_LEVEL featureLevel;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_SINGLETHREADED, nullptr, 0,
            D3D11_SDK_VERSION, m_device.GetAddressOf(),
            &featureLevel, m_deviceContext.GetAddressOf()
        );

        if (FAILED(hr)) {
            m_isInitialized = true;
            m_isAvailable = false;
            return false;
        }

        ComPtr<ID3DBlob> shaderBlob, errorBlob;
        hr = D3DCompile(
            COMPUTE_SHADER_SOURCE, strlen(COMPUTE_SHADER_SOURCE),
            nullptr, nullptr, nullptr, "ChunkPackKernel", "cs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            shaderBlob.GetAddressOf(), errorBlob.GetAddressOf()
        );

        if (FAILED(hr)) {
            m_isInitialized = true;
            m_isAvailable = false;
            return false;
        }

        hr = m_device->CreateComputeShader(
            shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(),
            nullptr, m_computeShader.GetAddressOf()
        );

        if (FAILED(hr)) return false;

        // Buffers setup
        D3D11_BUFFER_DESC inputDesc = {};
        inputDesc.ByteWidth = 4096;
        inputDesc.Usage = D3D11_USAGE_DEFAULT;
        inputDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        m_device->CreateBuffer(&inputDesc, nullptr, m_inputBuffer.GetAddressOf());

        D3D11_BUFFER_DESC outputDesc = {};
        outputDesc.ByteWidth = 6656; // 832 * 8 bytes
        outputDesc.Usage = D3D11_USAGE_DEFAULT;
        outputDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        m_device->CreateBuffer(&outputDesc, nullptr, m_outputBuffer.GetAddressOf());

        D3D11_BUFFER_DESC stagingDesc = {};
        stagingDesc.ByteWidth = 6656;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        m_device->CreateBuffer(&stagingDesc, nullptr, m_stagingBuffer.GetAddressOf());

        // UAV Setup
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;

        uavDesc.Buffer.NumElements = 4096 / 4;
        m_device->CreateUnorderedAccessView(m_inputBuffer.Get(), &uavDesc, m_inputUAV.GetAddressOf());

        uavDesc.Buffer.NumElements = 6656 / 4;
        m_device->CreateUnorderedAccessView(m_outputBuffer.Get(), &uavDesc, m_outputUAV.GetAddressOf());

        m_isInitialized = true;
        m_isAvailable = true;
        return true;
    }

    bool IsAvailable() const { return m_isAvailable; }

    bool SerializeChunk(const uint8_t* blockData, uint64_t* packedOutput) {
        if (!m_isAvailable || !m_deviceContext) return false;

        m_deviceContext->UpdateSubresource(m_inputBuffer.Get(), 0, nullptr, blockData, 0, 0);

        uint32_t clearValues[4] = { 0, 0, 0, 0 };
        m_deviceContext->ClearUnorderedAccessViewUint(m_outputUAV.Get(), clearValues);

        m_deviceContext->CSSetShader(m_computeShader.Get(), nullptr, 0);
        ID3D11UnorderedAccessView* uavs[] = { m_inputUAV.Get(), m_outputUAV.Get() };
        m_deviceContext->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

        // Dispatch 16 thread groups (16 * 256 = 4096 threads)
        m_deviceContext->Dispatch(16, 1, 1);

        m_deviceContext->CopyResource(m_stagingBuffer.Get(), m_outputBuffer.Get());

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        // Spin briefly or fall back if GPU is busy to avoid blocking the CPU network thread
        HRESULT hr = m_deviceContext->Map(m_stagingBuffer.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            // GPU hasn't finished yet; fall back immediately to CPU path to keep latency low
            ID3D11UnorderedAccessView* nullUAVs[] = { nullptr, nullptr };
            m_deviceContext->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
            return false;
        }

        if (FAILED(hr)) return false;

        memcpy(packedOutput, mapped.pData, 6656);
        m_deviceContext->Unmap(m_stagingBuffer.Get(), 0);

        ID3D11UnorderedAccessView* nullUAVs[] = { nullptr, nullptr };
        m_deviceContext->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);

        return true;
    }
};
