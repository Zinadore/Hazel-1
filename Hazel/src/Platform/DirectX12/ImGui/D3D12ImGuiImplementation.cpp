#include "hzpch.h"
#include "D3D12ImGuiImplementation.h"

#include "imgui.h"
#include "examples/imgui_impl_win32.h"
#include "examples/imgui_impl_dx12.h"
#include "examples/imgui_impl_win32.cpp"
#include "examples/imgui_impl_dx12.cpp"

#include "Platform/DirectX12/D3D12Context.h"

#include <GLFW/glfw3.h>



namespace Hazel {
    void D3D12ImGuiImplementation::Init(Window& window)
    {
        
        ctx = static_cast<D3D12Context*>(window.GetContext());


        ImGui_ImplWin32_Init(ctx->m_NativeHandle);
        ImGui_ImplDX12_Init(ctx->m_Device.Get(), ctx->m_NumFrames,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            ctx->m_SRVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
            ctx->m_SRVDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    }

    void D3D12ImGuiImplementation::RenderDrawData(ImDrawData* drawData)
    {
        ImGui_ImplDX12_RenderDrawData(drawData, ctx->m_CommandList.Get());
    }

    void D3D12ImGuiImplementation::NewFrame()
    {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
    }

    void D3D12ImGuiImplementation::Shutdown()
    {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
    }

    void D3D12ImGuiImplementation::UpdateDockedWindows()
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault(NULL, (void*)ctx->m_CommandList.Get());
    }

}
