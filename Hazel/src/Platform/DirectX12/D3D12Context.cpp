#include "hzpch.h"
#include "D3D12Context.h"
#include "D3D12Helpers.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <string>
#include "Hazel/Core/Log.h"
#include "Platform/DirectX12/ComPtr.h"

#define NUM_FRAMES 3

std::string static inline VendorIDToString(VendorID id) {
    switch (id) {
    case AMD:       return "AMD";
    case NVIDIA:    return "NVIDIA Corporation";
    case INTEL:     return "Intel";
    default: return "Unknown Vendor ID";
    }
}

bool CheckTearingSupport()
{
    BOOL allowTearing = FALSE;

    // Rather than create the DXGI 1.5 factory interface directly, we create the
    // DXGI 1.4 interface and query for the 1.5 interface. This is to enable the 
    // graphics debugging tools which will not support the 1.5 factory interface 
    // until a future update.
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory4;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4))))
    {
        Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory4.As(&factory5)))
        {
            if (FAILED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                &allowTearing, sizeof(allowTearing))))
            {
                allowTearing = FALSE;
            }
        }
    }

    return allowTearing == TRUE;
}

namespace Hazel {

    D3D12Context::D3D12Context(Window* window)
        : GraphicsContext(window)
    {
        auto wnd = (GLFWwindow *)window->GetNativeWindow();
        m_NativeHandle = glfwGetWin32Window(wnd);
        HZ_CORE_ASSERT(m_NativeHandle, "HWND is null!");
        DeviceResources = new D3D12DeviceResources(NUM_FRAMES);
    }

    D3D12Context::~D3D12Context()
    {
        delete DeviceResources;
    }

    void D3D12Context::Init()
    {   
        auto width = m_Window->GetWidth();
        auto height = m_Window->GetHeight();
        m_TearingSupported = CheckTearingSupport();

        DeviceResources->EnableDebugLayer();

        // The device
        TComPtr<IDXGIAdapter4> theAdapter = DeviceResources->GetAdapter(false);
        DeviceResources->Device = DeviceResources->CreateDevice(theAdapter);
        NAME_D3D12_OBJECT(DeviceResources->Device);

        BuildFrameResources();

        // The command queue        
        DeviceResources->CommandQueue = DeviceResources->CreateCommandQueue(
            DeviceResources->Device,
            D3D12_COMMAND_LIST_TYPE_DIRECT
        );
        NAME_D3D12_OBJECT(DeviceResources->CommandQueue);
        
        // The Swap Chain
        SwapChainCreationOptions opts = { 0 };
        opts.Width = width;
        opts.Height = height;
        opts.BufferCount = DeviceResources->SwapChainBufferCount;
        opts.TearingSupported = m_TearingSupported;
        opts.Handle = m_NativeHandle;

        DeviceResources->SwapChain = DeviceResources->CreateSwapChain(
            opts,
            DeviceResources->CommandQueue
        );

        m_CurrentBackbufferIndex = DeviceResources
            ->SwapChain
            ->GetCurrentBackBufferIndex();
        
        // Command Objects
        DeviceResources->CommandAllocator = DeviceResources->CreateCommandAllocator(
            DeviceResources->Device,
            D3D12_COMMAND_LIST_TYPE_DIRECT
        );
        NAME_D3D12_OBJECT(DeviceResources->CommandAllocator);

        DeviceResources->CommandList = DeviceResources->CreateCommandList(
            DeviceResources->Device,
            DeviceResources->CommandAllocator,
            D3D12_COMMAND_LIST_TYPE_DIRECT
        );
        NAME_D3D12_OBJECT(DeviceResources->CommandList);

        // The Heaps
        DeviceResources->RTVDescriptorHeap = DeviceResources->CreateDescriptorHeap(
            DeviceResources->Device,
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 
            DeviceResources->SwapChainBufferCount
        );
        NAME_D3D12_OBJECT(DeviceResources->RTVDescriptorHeap);

        m_RTVDescriptorSize = DeviceResources
            ->Device
            ->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        
        DeviceResources->SRVDescriptorHeap = DeviceResources->CreateDescriptorHeap(
            DeviceResources->Device, 
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 
            1, 
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
        );
        NAME_D3D12_OBJECT(DeviceResources->SRVDescriptorHeap);

        DeviceResources->DSVDescriptorHeap = DeviceResources->CreateDescriptorHeap(
            DeviceResources->Device, 
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 
            1
        );
        NAME_D3D12_OBJECT(DeviceResources->DSVDescriptorHeap);

        CreateRenderTargetViews();
        CreateDepthStencil();
        
        m_Viewport.TopLeftX = 0.0f;
        m_Viewport.TopLeftY = 0.0f;
        m_Viewport.Width = static_cast<float>(width);
        m_Viewport.Height = static_cast<float>(height);
        m_Viewport.MinDepth = 0.0f;
        m_Viewport.MaxDepth = 1.0f;

        // Sync
        DeviceResources->Fence = DeviceResources->CreateFence(DeviceResources->Device);

        PerformInitializationTransitions();

        DXGI_ADAPTER_DESC3 desc;
        theAdapter->GetDesc3(&desc);

        std::wstring description(desc.Description);
        std::string str(description.begin(), description.end());
        auto vendorString = VendorIDToString((VendorID)desc.VendorId);
        HZ_CORE_INFO("DirectX 12 Info:");
        HZ_CORE_INFO("  Vendor: {0}", vendorString);
        HZ_CORE_INFO("  Renderer: {0}", str);
        HZ_CORE_INFO("  Version: Direct3D 12.0");
    }

    void D3D12Context::SetVSync(bool enabled)
    {
        m_VSyncEnabled = enabled;
    }

    void D3D12Context::NewFrame()
    {
        NextFrameResource();
        // Get from resource
        auto commandAllocator = m_CurrentFrameResource->CommandAllocator;

        ThrowIfFailed(commandAllocator->Reset());
        ThrowIfFailed(DeviceResources->CommandList->Reset(
            commandAllocator.Get(), 
            nullptr)
        );
        
        DeviceResources->CommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());
        DeviceResources->CommandList->RSSetViewports(1, &m_Viewport);
        DeviceResources->CommandList->SetDescriptorHeaps(1, DeviceResources->SRVDescriptorHeap.GetAddressOf());

        auto backBuffer = DeviceResources->BackBuffers[m_CurrentBackbufferIndex];

        CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer.Get(),
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_BARRIER_FLAG_NONE);

        DeviceResources->CommandList->ResourceBarrier(1, &barrier);

    }

    void D3D12Context::SwapBuffers()
    {
        auto backBuffer = DeviceResources->BackBuffers[m_CurrentBackbufferIndex];

        CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, 
            D3D12_RESOURCE_STATE_PRESENT
        );

        DeviceResources->CommandList->ResourceBarrier(1, &barrier);

        ThrowIfFailed(DeviceResources->CommandList->Close());

        ID3D12CommandList* const commandLists[] = {
            DeviceResources->CommandList.Get()
        };
        DeviceResources->CommandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);

        UINT syncInterval = m_VSyncEnabled ? 1 : 0;
        UINT presentFlags = m_TearingSupported && !m_VSyncEnabled ? DXGI_PRESENT_ALLOW_TEARING : 0;
        ThrowIfFailed(DeviceResources->SwapChain->Present(syncInterval, presentFlags));

        // Get new backbuffer index
        m_CurrentBackbufferIndex = DeviceResources->SwapChain->GetCurrentBackBufferIndex();

        // Signal the queue
         m_FenceValue = DeviceResources->Signal(
            DeviceResources->CommandQueue,
            DeviceResources->Fence,
            m_FenceValue
        );

        // Update the resource
        m_CurrentFrameResource->FenceValue = m_FenceValue;
    }

    void D3D12Context::Clear(float * color)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = CurrentBackBufferView();

        DeviceResources->CommandList->ClearRenderTargetView(rtv, color, 0, nullptr);
        DeviceResources->CommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);


    }

    void D3D12Context::CreateRenderTargetViews()
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
            DeviceResources
            ->RTVDescriptorHeap
            ->GetCPUDescriptorHandleForHeapStart()
        );

        for (int i = 0; i < DeviceResources->SwapChainBufferCount; ++i)
        {
            TComPtr<ID3D12Resource> backBuffer;
            ThrowIfFailed(DeviceResources->SwapChain
                ->GetBuffer(i, IID_PPV_ARGS(&backBuffer))
            );

            DeviceResources->Device->CreateRenderTargetView(
                backBuffer.Get(), nullptr, rtvHandle);

            DeviceResources->BackBuffers[i] = backBuffer;
            NAME_D3D12_OBJECT_INDEXED(DeviceResources->BackBuffers, i);

            rtvHandle.Offset(1, m_RTVDescriptorSize);
        }

        m_CurrentBackbufferIndex = DeviceResources->SwapChain->GetCurrentBackBufferIndex();
    }

    void D3D12Context::CreateDepthStencil()
    {
        D3D12_RESOURCE_DESC desc;

        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = m_Window->GetWidth();
        desc.Height = m_Window->GetHeight();
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        desc.SampleDesc = { 1, 0 };
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear;
        clear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        clear.DepthStencil.Depth = 1.0f;
        clear.DepthStencil.Stencil = 0;

        ThrowIfFailed(DeviceResources->Device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            &clear,
            IID_PPV_ARGS(DeviceResources->DepthStencilBuffer.GetAddressOf())
        ));

        DeviceResources->Device->CreateDepthStencilView(
            DeviceResources->DepthStencilBuffer.Get(),
            nullptr,
            DepthStencilView()
        );

        NAME_D3D12_OBJECT(DeviceResources->DepthStencilBuffer);
    }
    
    void D3D12Context::Flush()
    {
        m_FenceValue = DeviceResources->Signal(
            DeviceResources->CommandQueue,
            DeviceResources->Fence,
            m_FenceValue
        );

        DeviceResources->WaitForFenceValue(
            DeviceResources->Fence,
            m_FenceValue
        );
    }
    void D3D12Context::CleanupRenderTargetViews()
    {
        //Flush();

        ThrowIfFailed(DeviceResources->CommandList->Reset(
            DeviceResources->CommandAllocator.Get(),
            nullptr)
        );

        for (UINT i = 0; i < DeviceResources->SwapChainBufferCount; i++)
        {
            DeviceResources->BackBuffers[i].Reset();
        }

        DeviceResources->DepthStencilBuffer.Reset();
    }
    void D3D12Context::ResizeSwapChain()
    {
        auto width = m_Window->GetWidth();
        auto height = m_Window->GetHeight();

        DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
        ThrowIfFailed(DeviceResources->SwapChain->GetDesc(&swapChainDesc));
        ThrowIfFailed(DeviceResources->SwapChain->ResizeBuffers(
            DeviceResources->SwapChainBufferCount, 
            width, 
            height,
            swapChainDesc.BufferDesc.Format, 
            swapChainDesc.Flags)
        );

        m_CurrentBackbufferIndex = DeviceResources->SwapChain->GetCurrentBackBufferIndex();
        m_Viewport.Width = width;
        m_Viewport.Height = height;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE D3D12Context::CurrentBackBufferView() const
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(
            DeviceResources->RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
            m_CurrentBackbufferIndex,
            m_RTVDescriptorSize
        );
    }

    D3D12_CPU_DESCRIPTOR_HANDLE D3D12Context::DepthStencilView() const
    {
        return DeviceResources->DSVDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    }

    void D3D12Context::PerformInitializationTransitions()
    {
        auto commandAllocator = DeviceResources->CommandAllocator;

        commandAllocator->Reset();
        DeviceResources->CommandList->Reset(commandAllocator.Get(), nullptr);

        // Transitions go here
        {
            auto dsBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
                DeviceResources->DepthStencilBuffer.Get(),
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_DEPTH_WRITE
            );

            DeviceResources->CommandList->ResourceBarrier(1, &dsBarrier);
        }

        ThrowIfFailed(DeviceResources->CommandList->Close());

        ID3D12CommandList* const commandLists[] = {
            DeviceResources->CommandList.Get()
        };
        DeviceResources->CommandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);

        Flush();
    }

    void D3D12Context::NextFrameResource()
    {
        m_CurrentBackbufferIndex = DeviceResources->SwapChain->GetCurrentBackBufferIndex();

        m_CurrentFrameResource = FrameResources[m_CurrentBackbufferIndex].get();

        if (m_CurrentFrameResource->FenceValue != 0) {
            DeviceResources->WaitForFenceValue(
                DeviceResources->Fence,
                m_CurrentFrameResource->FenceValue
            );
        }
    }

    void D3D12Context::BuildFrameResources()
    {
        auto count = DeviceResources->SwapChainBufferCount;
        FrameResources.reserve(count);

        for (int i = 0; i < count; i++)
        {
            FrameResources.push_back(std::make_unique<D3D12FrameResource>(
                DeviceResources->Device,
                1
            ));
        }
    }
}