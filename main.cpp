﻿#include "pch.h"

namespace winrt {
    // Standard WinRT inclusions
    using namespace winrt::Windows::Foundation;
    using namespace winrt::Windows::Foundation::Collections;

    // Windows data streams
    using namespace winrt::Windows::Storage::Streams;

    // JSON Parser
    using namespace winrt::Windows::Data::Json;

    // Direct Display
    using namespace winrt::Windows::Devices::Display;
    using namespace winrt::Windows::Devices::Display::Core;

    // DirectX
    using namespace winrt::Windows::Graphics;
    using namespace winrt::Windows::Graphics::DirectX;
    using namespace winrt::Windows::Graphics::Imaging;
} // namespace winrt

import MonitorControl;

// Parameters passed to each render thread
struct RenderParam
{
    RenderParam(const std::atomic_bool& shouldTerminate) :
        shouldTerminate(shouldTerminate)
    {}

    const std::atomic_bool& shouldTerminate;
    winrt::DisplayDevice device{ nullptr };
    winrt::DisplayTarget target{ nullptr };
    winrt::DisplayPath path{ nullptr };
};

const int SurfaceCount = 1;

class D3D11Renderer
{
    winrt::com_ptr<ID3D11Device5> d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> d3dContext;

    std::array<winrt::com_ptr<ID3D11Texture2D>, SurfaceCount> d3dSurfaces;
    std::array<winrt::com_ptr<ID3D11RenderTargetView>, SurfaceCount> d3dRenderTargets;
    winrt::com_ptr<ID3D11Fence> d3dFence;
    UINT64 fenceValue = 0;
    int frameCount = 0;

public:

    void Create(const winrt::DisplayAdapter& adapter)
    {
        winrt::com_ptr<IDXGIFactory6> factory;
        factory.capture(&CreateDXGIFactory2, 0);

        // Find the GPU that the target is connected to
        winrt::com_ptr<IDXGIAdapter4> dxgiAdapter;
        dxgiAdapter.capture(factory, &IDXGIFactory6::EnumAdapterByLuid, MonitorUtilities::LuidFromAdapterId(adapter.Id()));

        // Create the D3D device and context from the adapter
        D3D_FEATURE_LEVEL featureLevel;
        winrt::com_ptr<ID3D11Device> device;
        winrt::check_hresult(D3D11CreateDevice(dxgiAdapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, device.put(), &featureLevel, d3dContext.put()));
        d3dDevice = device.as<ID3D11Device5>();

        // Create a fence for signalling when rendering work finishes
        d3dFence.capture(d3dDevice, &ID3D11Device5::CreateFence, 0, D3D11_FENCE_FLAG_SHARED);
    }

    void OpenSurfaces(const winrt::DisplayDevice& device, std::array<winrt::DisplaySurface, SurfaceCount>& surfaces)
    {
        auto deviceInterop = device.as<IDisplayDeviceInterop>();

        for (int surfaceIndex = 0; surfaceIndex < SurfaceCount; surfaceIndex++)
        {
            auto surfaceRaw = surfaces[surfaceIndex].as<::IInspectable>();

            // Share the DisplaySurface across devices using a handle
            winrt::handle surfaceHandle;
            winrt::check_hresult(deviceInterop->CreateSharedHandle(surfaceRaw.get(), nullptr, GENERIC_ALL, nullptr, surfaceHandle.put()));

            // Call OpenSharedResource1 on the D3D device to get the ID3D11Texture2D
            d3dSurfaces[surfaceIndex].capture(d3dDevice, &ID3D11Device5::OpenSharedResource1, surfaceHandle.get());

            D3D11_TEXTURE2D_DESC surfaceDesc = {};
            d3dSurfaces[surfaceIndex]->GetDesc(&surfaceDesc);

            D3D11_RENDER_TARGET_VIEW_DESC viewDesc = {};
            viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            viewDesc.Texture2D.MipSlice = 0;
            viewDesc.Format = surfaceDesc.Format;

            // Create a render target view for the surface
            winrt::check_hresult(d3dDevice->CreateRenderTargetView(d3dSurfaces[surfaceIndex].get(), &viewDesc, d3dRenderTargets[surfaceIndex].put()));
        }
    }

    winrt::DisplayFence GetFence(const winrt::DisplayDevice& device)
    {
        auto deviceInterop = device.as<IDisplayDeviceInterop>();

        // Share the ID3D11Fence across devices using a handle
        winrt::handle fenceHandle;
        winrt::check_hresult(d3dFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, fenceHandle.put()));

        // Call OpenSharedHandle on the DisplayDevice to get a DisplayFence
        winrt::com_ptr<::IInspectable> displayFence;
        displayFence.capture(deviceInterop, &IDisplayDeviceInterop::OpenSharedHandle, fenceHandle.get());

        return displayFence.as<winrt::DisplayFence>();
    }

    UINT64 RenderAndGetFenceValue(int surfaceIndex)
    {
        // TODO: Perform rendering here with D3D11

        // For the sample, we simply render a color pattern using a frame counter. This code is not interesting.
        {
            frameCount++;
            float amount = (float)abs(sin((float)frameCount / 30 * 3.141592));
            float clearColor[4] = { amount * ((frameCount / 30) % 3 == 0), amount * ((frameCount / 30) % 3 == 1), amount * ((frameCount / 30) % 3 == 2), 1 };
            d3dContext->ClearRenderTargetView(d3dRenderTargets[surfaceIndex].get(), clearColor);
        }

        auto context4 = d3dContext.as<ID3D11DeviceContext4>();
        context4->Signal(d3dFence.get(), ++fenceValue);

        return fenceValue;
    }
};

void RenderThread(RenderParam& params)
{
    // It's not necessary to call init_apartment on every thread, but it needs to be called at least once before using WinRT
    winrt::init_apartment();

    D3D11Renderer renderer;
    renderer.Create(params.target.Adapter());

    // Create a display source, which identifies where to render
    winrt::DisplaySource source = params.device.CreateScanoutSource(params.target);

    // Create a task pool for queueing presents
    winrt::DisplayTaskPool taskPool = params.device.CreateTaskPool();

    winrt::SizeInt32 sourceResolution = params.path.SourceResolution().Value();
    winrt::Direct3D11::Direct3DMultisampleDescription multisampleDesc = {};
    multisampleDesc.Count = 1;

    // Create a surface format description for the primaries
    winrt::DisplayPrimaryDescription primaryDesc{
        static_cast<uint32_t>(sourceResolution.Width), static_cast<uint32_t>(sourceResolution.Height),
        params.path.SourcePixelFormat(), winrt::DirectXColorSpace::RgbFullG10NoneP709,
        false,
        multisampleDesc };

    std::array<winrt::DisplaySurface, SurfaceCount> primaries = { nullptr };
    std::array<winrt::DisplayScanout, SurfaceCount> scanouts = { nullptr };

    for (int surfaceIndex = 0; surfaceIndex < SurfaceCount; surfaceIndex++)
    {
        primaries[surfaceIndex] = params.device.CreatePrimary(params.target, primaryDesc);
        scanouts[surfaceIndex] = params.device.CreateSimpleScanout(source, primaries[surfaceIndex], 0, 1);
    }

    renderer.OpenSurfaces(params.device, primaries);

    // Get a fence to wait for render work to complete
    winrt::DisplayFence fence = renderer.GetFence(params.device);

    // Render and present until termination is signalled
    int surfaceIndex = 0;
    while (!params.shouldTerminate)
    {
        UINT64 fenceValue = renderer.RenderAndGetFenceValue(surfaceIndex);

        winrt::DisplayTask task = taskPool.CreateTask();
        task.SetScanout(scanouts[surfaceIndex]);
        task.SetWait(fence, fenceValue);

        taskPool.ExecuteTask(task);

        params.device.WaitForVBlank(source);

        surfaceIndex++;
        if (surfaceIndex >= SurfaceCount)
        {
            surfaceIndex = 0;
        }
    }
}

void RefreshTarget(winrt::DisplayManager& displayManager, winrt::DisplayTarget& displayTarget)
{
    if (displayManager && displayTarget && displayTarget.IsStale())
    {
        auto currentTargets = displayManager.GetCurrentTargets();
        for (auto&& currentTarget : currentTargets)
        {
            if (currentTarget.IsConnected() && currentTarget.IsSame(displayTarget))
            {
                displayTarget = currentTarget;
                return;
            }
        }

        // The selected target is no longer showing up as current or connected!
        throw winrt::hresult_changed_state();
    }
}

inline BOOL IsTestSigningOn()
{
    SYSTEM_CODEINTEGRITY_INFORMATION codeIntegrityInfo = {};

    HMODULE handle = GetModuleHandle(L"ntdll.dll");
    if(!handle)
    {
        throw winrt::hresult_error();
    }
    using NtQuerySystemInformation = NTSTATUS(WINAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    NtQuerySystemInformation ntQuerySystemInformation = reinterpret_cast<NtQuerySystemInformation>(GetProcAddress(handle, "NtQuerySystemInformation"));
    codeIntegrityInfo.Length = sizeof(codeIntegrityInfo);
    return (NT_SUCCESS(ntQuerySystemInformation(SystemCodeIntegrityInformation, &codeIntegrityInfo, sizeof(codeIntegrityInfo), nullptr)) &&
        (codeIntegrityInfo.CodeIntegrityOptions & CODEINTEGRITY_OPTION_TESTSIGN));
}

int main()
{
    winrt::init_apartment();

    if (!IsTestSigningOn())
    {
        std::cerr << "Test signing must be on for this to work!";
        throw winrt::hresult_error();
    }

    auto displayManager = winrt::DisplayManager::Create(winrt::DisplayManagerOptions::None);

    std::cout << "Choose one of the monitors below:" << std::endl;
    auto currentTargets = displayManager.GetCurrentTargets();
    std::vector<winrt::DisplayTarget> targets;
    for (auto&& target : currentTargets)
    {
        auto monitor = target.TryGetMonitor();
        if (monitor)
        {
            targets.push_back(target);
            std::wcout << targets.size() << L". " << monitor.DisplayName().c_str() << std::endl;
        }
    }

    int targetSelection;
    std::cin >> targetSelection;

    if (targets.size() < targetSelection || targetSelection < 1)
    {
        std::cerr << "Invalid display target index input!";
        throw winrt::hresult_invalid_argument();
    }

    auto target = targets[targetSelection-1];

    std::unique_ptr<MonitorUtilities::MonitorControl> monitorControl;

    if (target.UsageKind() != winrt::DisplayMonitorUsageKind::SpecialPurpose)
    {
        monitorControl = std::make_unique<MonitorUtilities::MonitorControl>(
            MonitorUtilities::LuidFromAdapterId(target.Adapter().Id()), target.AdapterRelativeId());

        // If we change the UsageKind for the displayTarget, make sure to go refresh the target
        winrt::DisplayTarget refreshedTarget = nullptr;
        for (auto&& currTarget : displayManager.GetCurrentTargets())
        {
            if (target.IsSame(currTarget))
            {
                refreshedTarget = currTarget;
                break;
            }
        }

        if (!refreshedTarget)
        {
            std::cerr << "Unable to find the target after refreshing.";
            return -1;
        }

        target = refreshedTarget;
    }

    winrt::DisplayPath targetPath{ nullptr };
    winrt::DisplayState targetState{ nullptr };
    winrt::DisplayDevice targetDevice{ nullptr };

    auto targetList = winrt::single_threaded_vector<winrt::DisplayTarget>();
    targetList.Append(target);
    auto result = displayManager.TryAcquireTargetsAndCreateEmptyState(targetList);

    // TryAcquireTargetsAndCreateEmptyState is a very flaky call - so try it again a few times if it fails
    const uint32_t retries = 20;
    const uint32_t retryDelay = 500;
    for (uint32_t i = 0; i < retries && result.ErrorCode() != winrt::DisplayManagerResult::Success; i++, Sleep(retryDelay))
    {
        RefreshTarget(displayManager, target);

        targetList.Clear();
        targetList.Append(target);
        result = displayManager.TryAcquireTargetsAndCreateEmptyState(targetList);
    }

    if (result.ErrorCode() != winrt::DisplayManagerResult::Success)
    {
        std::cerr << "Unable to acquire target. Error " << result.ExtendedErrorCode();
        return -1;
    }

    targetState = result.State();
    targetPath = targetState.ConnectTarget(target);
    targetDevice = displayManager.CreateDisplayDevice(target.Adapter());

    // Set some values that we know we want
    targetPath.IsInterlaced(false);
    targetPath.Scaling(winrt::DisplayPathScaling::Identity);

    // We only look at BGRA 8888 modes in this example
    targetPath.SourcePixelFormat(winrt::DirectXPixelFormat::R16G16B16A16Float);

    auto wireFormat = winrt::DisplayWireFormat::DisplayWireFormat(
        winrt::DisplayWireFormatPixelEncoding::Rgb444,
        10,
        winrt::DisplayWireFormatColorSpace::BT2020,
        winrt::DisplayWireFormatEotf::HdrSmpte2084,
        winrt::DisplayWireFormatHdrMetadata::Hdr10
    );

    targetPath.WireFormat(wireFormat);

    // Get a list of modes for only the preferred resolution
    winrt::IVectorView<winrt::DisplayModeInfo> modes = targetPath.FindModes(winrt::DisplayModeQueryOptions::OnlyPreferredResolution);

    // Find e.g. the mode with a refresh rate closest to 60 Hz
    winrt::DisplayModeInfo bestMode{ nullptr };
    double bestModeDiff = INFINITY;
    for (auto&& mode : modes)
    {
        auto vSync = mode.PresentationRate().VerticalSyncRate;
        double vSyncDouble = (double)vSync.Numerator / vSync.Denominator;

        double modeDiff = abs(vSyncDouble - 60);
        if (modeDiff < bestModeDiff)
        {
            bestMode = mode;
            bestModeDiff = modeDiff;
        }
    }

    if (!bestMode)
    {
        // Failed to find a mode
        std::cerr << "Failed to find a valid mode";
        return -1;
    }

    // Set the properties on the path
    targetPath.ApplyPropertiesFromMode(bestMode);

    // Now that we've decided on modes to use for all of the targets, apply all the modes in one-shot
    auto applyResult = targetState.TryApply(winrt::DisplayStateApplyOptions::None);
    check_hresult(applyResult.ExtendedErrorCode());

    // Re-read the current state to see the final state that was applied (with all properties)
    auto acquireResult = displayManager.TryAcquireTargetsAndReadCurrentState(targetList);
    check_hresult(acquireResult.ExtendedErrorCode());
    auto state = acquireResult.State();

    std::atomic_bool shouldCancelRenderThreads;

    std::unique_ptr<RenderParam> params = std::make_unique<RenderParam>(shouldCancelRenderThreads);

    // Create a device to present with
    params->device = displayManager.CreateDisplayDevice(target.Adapter());

    params->target = target;
    params->path = state.GetPathForTarget(target);

    auto wireformat = params->path.WireFormat();

    std::thread renderThread([params = std::move(params)]()
        {
            RenderThread(*params);
        });

    std::cout << "Press any key to stop...";
    (void)getchar();
    (void)getchar();

    // Trigger all render threads to terminate
    shouldCancelRenderThreads = true;

    renderThread.join();

    return 0;
}
