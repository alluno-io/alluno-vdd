/*
 * Alluno VDD - IddCx virtual display driver for remote desktop streaming.
 * Based on Microsoft IDD sample, extended with dynamic display management,
 * HDR support, and IOCTL control API.
 */

#include "Driver.h"
#include "edid.h"

#include <tuple>
#include <list>
#include <iostream>
#include <thread>
#include <mutex>

#include "AdapterOption.h"
#include "../include/alluno-vdd-ioctl.h"

using namespace std;
using namespace Microsoft::IndirectDisp;
using namespace Microsoft::WRL;
using namespace ALLUNO_VDD;

LUID preferredAdapterLuid{};
bool preferredAdapterChanged = false;

std::mutex monitorListOp;
std::queue<size_t> freeConnectorSlots;
std::list<IndirectMonitorContext*> monitorCtxList;

bool isHDRSupported = false;
bool testMode = false;
DWORD watchdogTimeout = 3; // seconds
DWORD watchdogCountdown = 0;
std::thread watchdogThread;

DWORD MaxVirtualMonitorCount = 10;
IDDCX_BITS_PER_COMPONENT SDRBITS = IDDCX_BITS_PER_COMPONENT_8;
IDDCX_BITS_PER_COMPONENT HDRBITS = IDDCX_BITS_PER_COMPONENT_10;

// Per-connector HDR capability (set before monitor arrival, read by QueryTargetInfo)
static UINT connectorBpc[16] = {};

#pragma region DefaultModes

static const UINT mode_scale_factors[] = {
	// Put 100 at the first for convenience and fool proof
	100,
	50,
	75,
	125,
	150,
};

// Default modes reported for edid-less monitors. The second mode is set as preferred
static const struct VirtualMonitorMode s_DefaultModes[] = {
	{800, 600, 30000},
	{800, 600, 60000},
	{800, 600, 72000},
	{800, 600, 90000},
	{800, 600, 120000},
	{800, 600, 144000},
	{800, 600, 240000},
	{1280, 720, 30000},
	{1280, 720, 60000},
	{1280, 720, 72000},
	{1280, 720, 90000},
	{1280, 720, 120000},
	{1280, 720, 144000},
	{1366, 768, 30000},
	{1366, 768, 60000},
	{1366, 768, 72000},
	{1366, 768, 90000},
	{1366, 768, 120000},
	{1366, 768, 144000},
	{1366, 768, 240000},
	{1920, 1080, 30000},
	{1920, 1080, 60000},
	{1920, 1080, 72000},
	{1920, 1080, 90000},
	{1920, 1080, 120000},
	{1920, 1080, 144000},
	{1920, 1080, 240000},
	{2560, 1440, 30000},
	{2560, 1440, 60000},
	{2560, 1440, 72000},
	{2560, 1440, 90000},
	{2560, 1440, 120000},
	{2560, 1440, 144000},
	{2560, 1440, 240000},
	{3840, 2160, 30000},
	{3840, 2160, 60000},
	{3840, 2160, 72000},
	{3840, 2160, 90000},
	{3840, 2160, 120000},
	{3840, 2160, 144000},
	{3840, 2160, 240000},
};

#pragma endregion

#pragma region helpers

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, DWORD VSync, bool bMonitorMode)
{
	Mode.totalSize.cx = Mode.activeSize.cx = Width;
	Mode.totalSize.cy = Mode.activeSize.cy = Height;

	// See https://docs.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_video_signal_info
	Mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
	Mode.AdditionalSignalInfo.videoStandard = 255;

	DWORD Denominator = 1000;

	// Always use millihertz representation for exact refresh rates
	if (VSync < 1000) {
		VSync *= 1000;
	}

	Mode.vSyncFreq.Numerator = VSync;
	Mode.vSyncFreq.Denominator = Denominator;
	Mode.hSyncFreq.Numerator = VSync * Height;
	Mode.hSyncFreq.Denominator = Denominator;

	Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

	Mode.pixelRate = ((UINT64) VSync) * ((UINT64) Width) * ((UINT64) Height) / Denominator;
}

static IDDCX_MONITOR_MODE CreateIddCxMonitorMode(DWORD Width, DWORD Height, DWORD VSync, IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER)
{
	IDDCX_MONITOR_MODE Mode = {};

	Mode.Size = sizeof(Mode);
	Mode.Origin = Origin;
	FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

	return Mode;
}

static IDDCX_MONITOR_MODE2 CreateIddCxMonitorMode2(DWORD Width, DWORD Height, DWORD VSync, IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER, UINT bpc = 0)
{
	IDDCX_MONITOR_MODE2 Mode = {};

	Mode.Size = sizeof(Mode);
	Mode.Origin = Origin;
	Mode.BitsPerComponent.Rgb = (bpc >= 10) ? (SDRBITS | HDRBITS) : SDRBITS;
	FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

	return Mode;
}

static IDDCX_TARGET_MODE CreateIddCxTargetMode(DWORD Width, DWORD Height, DWORD VSync)
{
	IDDCX_TARGET_MODE Mode = {};

	Mode.Size = sizeof(Mode);
	FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);

	return Mode;
}

static IDDCX_TARGET_MODE2 CreateIddCxTargetMode2(DWORD Width, DWORD Height, DWORD VSync, UINT bpc = 0)
{
	IDDCX_TARGET_MODE2 Mode = {};

	Mode.Size = sizeof(Mode);
	Mode.BitsPerComponent.Rgb = (bpc >= 10) ? (SDRBITS | HDRBITS) : SDRBITS;
	FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);

	return Mode;
}

#pragma endregion

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_UNLOAD AllunoVDDDriverUnload;
EVT_WDF_DRIVER_DEVICE_ADD AllunoVDDDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY AllunoVDDDeviceD0Entry;

EVT_IDD_CX_ADAPTER_INIT_FINISHED AllunoVDDAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES AllunoVDDAdapterCommitModes;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION AllunoVDDParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES AllunoVDDMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES AllunoVDDMonitorQueryModes;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN AllunoVDDMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN AllunoVDDMonitorUnassignSwapChain;

EVT_IDD_CX_ADAPTER_QUERY_TARGET_INFO AllunoVDDAdapterQueryTargetInfo;
EVT_IDD_CX_MONITOR_SET_DEFAULT_HDR_METADATA AllunoVDDMonitorSetDefaultHdrMetadata;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION2 AllunoVDDParseMonitorDescription2;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2 AllunoVDDMonitorQueryModes2;
EVT_IDD_CX_ADAPTER_COMMIT_MODES2 AllunoVDDAdapterCommitModes2;

EVT_IDD_CX_MONITOR_SET_GAMMA_RAMP AllunoVDDMonitorSetGammaRamp;

struct IndirectDeviceContextWrapper
{
	IndirectDeviceContext* pContext;

	void Cleanup()
	{
		delete pContext;
		pContext = nullptr;
	}
};

struct IndirectMonitorContextWrapper
{
	IndirectMonitorContext* pContext;

	void Cleanup()
	{
		delete pContext;
		pContext = nullptr;
	}
};

// This macro creates the methods for accessing an IndirectDeviceContextWrapper as a context for a WDF object
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);

extern "C" BOOL WINAPI DllMain(
	_In_ HINSTANCE hInstance,
	_In_ UINT dwReason,
	_In_opt_ LPVOID lpReserved)
{
	UNREFERENCED_PARAMETER(hInstance);
	UNREFERENCED_PARAMETER(lpReserved);
	UNREFERENCED_PARAMETER(dwReason);

	return TRUE;
}

void LoadSettings() {
	HKEY hKey;
	DWORD bufferSize;
	LONG lResult;

	// Open the registry key
	lResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Alluno\\AllunoVDD", 0, KEY_READ, &hKey);
	if (lResult != ERROR_SUCCESS) {
		return;
	}

	// Query gpuName
	wchar_t gpuName[128];
	bufferSize = sizeof(gpuName);
	lResult = RegQueryValueExW(hKey, L"gpuName", NULL, NULL, (LPBYTE)gpuName, &bufferSize);
	if (lResult == ERROR_SUCCESS) {
		AdapterOption adapterOpt = AdapterOption();
		adapterOpt.selectGPU(gpuName);

		preferredAdapterLuid = adapterOpt.adapterLuid;
		preferredAdapterChanged = adapterOpt.hasTargetAdapter;
	}

	// Query Test mode
	DWORD _testMode;
	bufferSize = sizeof(DWORD);
	lResult = RegQueryValueExW(hKey, L"testMode", NULL, NULL, (LPBYTE)&_testMode, &bufferSize);
	if (lResult == ERROR_SUCCESS) {
		testMode = !!_testMode;
	}

	// Query watchdog
	DWORD _watchdogTimeout;
	bufferSize = sizeof(DWORD);
	lResult = RegQueryValueExW(hKey, L"watchdog", NULL, NULL, (LPBYTE)&_watchdogTimeout, &bufferSize);
	if (lResult == ERROR_SUCCESS) {
		watchdogTimeout = _watchdogTimeout;
	}

	// Query Max monitor count
	DWORD _maxMonitorCount;
	bufferSize = sizeof(DWORD);
	lResult = RegQueryValueExW(hKey, L"maxMonitors", NULL, NULL, (LPBYTE)&_maxMonitorCount, &bufferSize);
	if (lResult == ERROR_SUCCESS) {
		MaxVirtualMonitorCount = _maxMonitorCount;
	}

	// Query SDRBits
	DWORD _sdrBits;
	bufferSize = sizeof(DWORD);
	lResult = RegQueryValueExW(hKey, L"sdrBits", NULL, NULL, (LPBYTE)&_sdrBits, &bufferSize);
	if (lResult == ERROR_SUCCESS) {
		if (_sdrBits == 10) {
			SDRBITS = IDDCX_BITS_PER_COMPONENT_10;
		}
	}

	// Query HDRBits
	DWORD _hdrBits;
	bufferSize = sizeof(DWORD);
	lResult = RegQueryValueExW(hKey, L"hdrBits", NULL, NULL, (LPBYTE)&_hdrBits, &bufferSize);
	if (lResult == ERROR_SUCCESS) {
		if (_hdrBits == 12) {
			HDRBITS = IDDCX_BITS_PER_COMPONENT_12;
		}
	}

	// Close the registry key
	RegCloseKey(hKey);
}

void DisconnectAllMonitors() {
	std::lock_guard<std::mutex> lg(monitorListOp);

	if (monitorCtxList.empty()) {
		return;
	}

	for (auto it = monitorCtxList.begin(); it != monitorCtxList.end(); ++it) {
		auto* ctx = *it;
		// Remove the monitor
		if (ctx->connectorId < 16) {
			connectorBpc[ctx->connectorId] = 0;
		}
		freeConnectorSlots.push(ctx->connectorId);
		IddCxMonitorDeparture(ctx->GetMonitor());
	}

	monitorCtxList.clear();
}

std::atomic<bool> watchdogRunning{false};

void RunWatchdog() {
	watchdogRunning = true;
	watchdogCountdown = watchdogTimeout;
	watchdogThread = std::thread([]{
		while (watchdogRunning) {
			Sleep(1000);

			// If watchdog disabled (timeout=0), just idle — don't disconnect
			if (!watchdogTimeout) {
				continue;
			}

			if (!watchdogCountdown || monitorCtxList.empty()) {
				continue;
			}

			watchdogCountdown -= 1;

			if (!watchdogCountdown) {
				DisconnectAllMonitors();
			}
		}
	});
}

void SetHighPriority() {
	SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
	PDRIVER_OBJECT  pDriverObject,
	PUNICODE_STRING pRegistryPath
)
{
	LoadSettings();

	WDF_DRIVER_CONFIG Config;
	NTSTATUS Status;

	WDF_OBJECT_ATTRIBUTES Attributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

	WDF_DRIVER_CONFIG_INIT(&Config,
		AllunoVDDDeviceAdd
	);

	Config.EvtDriverUnload = AllunoVDDDriverUnload;

	Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	RunWatchdog();

	SetHighPriority();

	return Status;
}

_Use_decl_annotations_
void AllunoVDDDriverUnload(_In_ WDFDRIVER) {
	watchdogRunning = false;
	if (watchdogThread.joinable()) {
		watchdogThread.join();
	}
	DisconnectAllMonitors();
}

VOID AllunoVDDIoDeviceControl(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request,
	_In_ size_t OutputBufferLength,
	_In_ size_t InputBufferLength,
	_In_ ULONG IoControlCode
);

_Use_decl_annotations_
NTSTATUS AllunoVDDDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
	NTSTATUS Status = STATUS_SUCCESS;
	WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

	UNREFERENCED_PARAMETER(Driver);

	// Register for power callbacks - only power-on is needed
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
	PnpPowerCallbacks.EvtDeviceD0Entry = AllunoVDDDeviceD0Entry;
	WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

	IDD_CX_CLIENT_CONFIG IddConfig;
	IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

	// If the driver wishes to handle custom IoDeviceControl requests, it's necessary to use this callback since IddCx
	// redirects IoDeviceControl requests to an internal queue.
	IddConfig.EvtIddCxDeviceIoControl = AllunoVDDIoDeviceControl;

	IddConfig.EvtIddCxAdapterInitFinished = AllunoVDDAdapterInitFinished;

	IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = AllunoVDDMonitorGetDefaultModes;
	IddConfig.EvtIddCxMonitorAssignSwapChain = AllunoVDDMonitorAssignSwapChain;
	IddConfig.EvtIddCxMonitorUnassignSwapChain = AllunoVDDMonitorUnassignSwapChain;

	if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo))
	{
		isHDRSupported = true;
		IddConfig.EvtIddCxAdapterQueryTargetInfo = AllunoVDDAdapterQueryTargetInfo;
		IddConfig.EvtIddCxMonitorSetDefaultHdrMetaData = AllunoVDDMonitorSetDefaultHdrMetadata;
		IddConfig.EvtIddCxParseMonitorDescription2 = AllunoVDDParseMonitorDescription2;
		IddConfig.EvtIddCxMonitorQueryTargetModes2 = AllunoVDDMonitorQueryModes2;
		IddConfig.EvtIddCxAdapterCommitModes2 = AllunoVDDAdapterCommitModes2;
		IddConfig.EvtIddCxMonitorSetGammaRamp = AllunoVDDMonitorSetGammaRamp;
	} else {
		IddConfig.EvtIddCxParseMonitorDescription = AllunoVDDParseMonitorDescription;
		IddConfig.EvtIddCxMonitorQueryTargetModes = AllunoVDDMonitorQueryModes;
		IddConfig.EvtIddCxAdapterCommitModes = AllunoVDDAdapterCommitModes;
	}

	Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
	Attr.EvtCleanupCallback = [](WDFOBJECT Object)
	{
		// Automatically cleanup the context when the WDF object is about to be deleted
		auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
		if (pContext)
		{
			pContext->Cleanup();
		}
	};

	WDFDEVICE Device = nullptr;
	Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	Status = WdfDeviceCreateDeviceInterface(
		Device,
		&ALLUNO_VDD_INTERFACE_GUID,
		NULL
	);

	if (!NT_SUCCESS(Status)) {
		return Status;
	}

	Status = IddCxDeviceInitialize(Device);

	// Create a new device context object and attach it to the WDF device object
	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	pContext->pContext = new IndirectDeviceContext(Device);

	return Status;
}

_Use_decl_annotations_
NTSTATUS AllunoVDDDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
	UNREFERENCED_PARAMETER(PreviousState);

	// This function is called by WDF to start the device in the fully-on power state.

	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	pContext->pContext->InitAdapter();

	return STATUS_SUCCESS;
}

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{
}

Direct3DDevice::Direct3DDevice()
{
	AdapterLuid = {};
}

HRESULT Direct3DDevice::Init()
{
	// The DXGI factory could be cached, but if a new render adapter appears on the system, a new factory needs to be
	// created. If caching is desired, check DxgiFactory->IsCurrent() each time and recreate the factory if !IsCurrent.
	HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
	if (FAILED(hr))
	{
		return hr;
	}

	// Find the specified render adapter
	hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
	if (FAILED(hr))
	{
		return hr;
	}

	// Create a D3D device using the render adapter. BGRA support is required by the WHQL test suite.
	hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
	if (FAILED(hr))
	{
		// If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the
		// system is in a transient state.
		return hr;
	}

	return S_OK;
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent)
	: m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent)
{
	m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));

	// Immediately create and run the swap-chain processing thread, passing 'this' as the thread parameter
	m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
	// Alert the swap-chain processing thread to terminate
	SetEvent(m_hTerminateEvent.Get());

	if (m_hThread.Get())
	{
		// Wait for the thread to terminate
		WaitForSingleObject(m_hThread.Get(), INFINITE);
	}
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
	reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
	return 0;
}

void SwapChainProcessor::Run()
{
	// For improved performance, make use of the Multimedia Class Scheduler Service, which will intelligently
	// prioritize this thread for improved throughput in high CPU-load scenarios.
	DWORD AvTask = 0;
	HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"DisplayPostProcessing", &AvTask);

	RunCore();

	// Always delete the swap-chain object when swap-chain processing loop terminates in order to kick the system to
	// provide a new swap-chain if necessary.
	WdfObjectDelete((WDFOBJECT)m_hSwapChain);
	m_hSwapChain = nullptr;

	AvRevertMmThreadCharacteristics(AvTaskHandle);
}

void SwapChainProcessor::RunCore()
{
	// Get the DXGI device interface
	ComPtr<IDXGIDevice> DxgiDevice;
	HRESULT hr = m_Device->Device.As(&DxgiDevice);
	if (FAILED(hr))
	{
		return;
	}

	IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
	SetDevice.pDevice = DxgiDevice.Get();

	hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
	if (FAILED(hr))
	{
		return;
	}

	// Acquire and release buffers in a loop
	for (;;)
	{
		ComPtr<IDXGIResource> AcquiredBuffer;

		IDXGIResource* pSurface;

		if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
			IDARG_IN_RELEASEANDACQUIREBUFFER2 BufferInArgs = {};
			BufferInArgs.Size = sizeof(BufferInArgs);
			IDARG_OUT_RELEASEANDACQUIREBUFFER2 Buffer = {};
			hr = IddCxSwapChainReleaseAndAcquireBuffer2(m_hSwapChain, &BufferInArgs, &Buffer);
			pSurface = Buffer.MetaData.pSurface;
		}
		else
		{
			IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
			hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);
			pSurface = Buffer.MetaData.pSurface;
		}

		// Ask for the next buffer from the producer
		// IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
		// hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);

		// AcquireBuffer immediately returns STATUS_PENDING if no buffer is yet available
		if (hr == E_PENDING)
		{
			// We must wait for a new buffer
			HANDLE WaitHandles [] =
			{
				m_hAvailableBufferEvent,
				m_hTerminateEvent.Get()
			};
			DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, 16);
			if (WaitResult == WAIT_OBJECT_0 || WaitResult == WAIT_TIMEOUT)
			{
				// We have a new buffer, so try the AcquireBuffer again
				continue;
			}
			else if (WaitResult == WAIT_OBJECT_0 + 1)
			{
				// We need to terminate
				break;
			}
			else
			{
				// The wait was cancelled or something unexpected happened
				hr = HRESULT_FROM_WIN32(WaitResult);
				break;
			}
		}
		else if (SUCCEEDED(hr))
		{
			AcquiredBuffer.Attach(pSurface);
			AcquiredBuffer.Reset();

			hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
			if (FAILED(hr))
			{
				break;
			}
		}
		else
		{
			// The swap-chain was likely abandoned (e.g. DXGI_ERROR_ACCESS_LOST), so exit the processing loop
			break;
		}
	}
}

#pragma endregion

#pragma region IndirectDeviceContext

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) :
	m_WdfDevice(WdfDevice)
{
	m_Adapter = {};
	for (size_t i = 0; i < MaxVirtualMonitorCount; i++) {
		freeConnectorSlots.push(i);
	}
}

IndirectDeviceContext::~IndirectDeviceContext()
{
}

void IndirectDeviceContext::InitAdapter()
{
	IDDCX_ADAPTER_CAPS AdapterCaps = {};
	AdapterCaps.Size = sizeof(AdapterCaps);

	if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
		AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16;
	}

	// Declare basic feature support for the adapter (required)
	AdapterCaps.MaxMonitorsSupported = MaxVirtualMonitorCount;
	AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
	AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
	AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

	// Declare your device strings for telemetry (required)
	AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"Alluno Virtual Display Adapter";
	AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"Alluno";
	AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"AllunoVDD";

	// Declare your hardware and firmware versions (required)
	IDDCX_ENDPOINT_VERSION Version = {};
	Version.Size = sizeof(Version);
	Version.MajorVer = 1;
	AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
	AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

	// Initialize a WDF context that can store a pointer to the device context object
	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

	IDARG_IN_ADAPTER_INIT AdapterInit = {};
	AdapterInit.WdfDevice = m_WdfDevice;
	AdapterInit.pCaps = &AdapterCaps;
	AdapterInit.ObjectAttributes = &Attr;

	// Start the initialization of the adapter, which will trigger the AdapterFinishInit callback later
	IDARG_OUT_ADAPTER_INIT AdapterInitOut;
	NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);

	if (NT_SUCCESS(Status))
	{
		// Store a reference to the WDF adapter handle
		m_Adapter = AdapterInitOut.AdapterObject;

		// Store the device context object into the WDF object context
		auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
		pContext->pContext = this;
	}
}

void IndirectDeviceContext::SetRenderAdapter(const LUID& AdapterLuid) {
	IDARG_IN_ADAPTERSETRENDERADAPTER inArgs{AdapterLuid};
	IddCxAdapterSetRenderAdapter(m_Adapter, &inArgs);
}

NTSTATUS IndirectDeviceContext::CreateMonitor(IndirectMonitorContext*& pMonitorContext, uint8_t* edidData, const GUID& containerId, const VirtualMonitorMode& preferredMode, UINT bitsPerChannel) {
	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);

	IDDCX_MONITOR_INFO MonitorInfo = {};
	MonitorInfo.Size = sizeof(MonitorInfo);
	MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
	MonitorInfo.ConnectorIndex = (UINT)freeConnectorSlots.front();

	MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
	MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
	MonitorInfo.MonitorDescription.DataSize = sizeof(edid_base);
	MonitorInfo.MonitorDescription.pData = edidData;
	MonitorInfo.MonitorContainerId = containerId;

	IDARG_IN_MONITORCREATE MonitorCreate = {};
	MonitorCreate.ObjectAttributes = &Attr;
	MonitorCreate.pMonitorInfo = &MonitorInfo;

	// Create a monitor object with the specified monitor descriptor
	IDARG_OUT_MONITORCREATE MonitorCreateOut;
	NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
	if (NT_SUCCESS(Status))
	{
		freeConnectorSlots.pop();
		// Create a new monitor context object and attach it to the Idd monitor object
		auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject);
		pMonitorContext = new IndirectMonitorContext(MonitorCreateOut.MonitorObject);
		pMonitorContextWrapper->pContext = pMonitorContext;

		pMonitorContext->monitorGuid = containerId;
		pMonitorContext->connectorId = MonitorInfo.ConnectorIndex;
		pMonitorContext->pEdidData = edidData;
		pMonitorContext->preferredMode = preferredMode;
		pMonitorContext->bitsPerChannel = bitsPerChannel ? bitsPerChannel : 8;
		pMonitorContext->m_Adapter = m_Adapter;

		// Set per-connector HDR capability before arrival (used by QueryTargetInfo)
		if (MonitorInfo.ConnectorIndex < 16) {
			connectorBpc[MonitorInfo.ConnectorIndex] = pMonitorContext->bitsPerChannel;
		}

		// Add to list AFTER all fields are set, BEFORE arrival.
		// ParseMonitorDescription/QueryTargetModes iterate this list
		// to find the preferredMode — it must be set before IddCxMonitorArrival.
		monitorCtxList.emplace_back(pMonitorContext);

		// Tell the OS that the monitor has been plugged in
		IDARG_OUT_MONITORARRIVAL ArrivalOut;
		Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
		if (NT_SUCCESS(Status)) {
			pMonitorContext->adapterLuid = ArrivalOut.OsAdapterLuid;
			pMonitorContext->targetId = ArrivalOut.OsTargetId;
		}
	} else {
		// Avoid memory leak
		free(edidData);
	}

	return Status;
}

IndirectMonitorContext::IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor) :
	m_Monitor(Monitor)
{
	// NOTE: Do NOT add to monitorCtxList here — preferredMode and other
	// fields aren't set yet. CreateMonitor adds to the list after setup.
}

IndirectMonitorContext::~IndirectMonitorContext()
{
	m_ProcessingThread.reset();
	if (pEdidData && pEdidData != edid_base) {
		free(pEdidData);
	}
	if (pCustomEdidData) {
		free(pCustomEdidData);
		pCustomEdidData = nullptr;
	}
}

IDDCX_MONITOR IndirectMonitorContext::GetMonitor() const {
	return m_Monitor;
}

void IndirectMonitorContext::AssignSwapChain(const IDDCX_MONITOR& MonitorObject, const IDDCX_SWAPCHAIN& SwapChain, const LUID& RenderAdapter, const HANDLE& NewFrameEvent)
{
	m_ProcessingThread.reset();

	auto Device = make_shared<Direct3DDevice>(RenderAdapter);
	if (FAILED(Device->Init()))
	{
		// It's important to delete the swap-chain if D3D initialization fails, so that the OS knows to generate a new
		// swap-chain and try again.
		WdfObjectDelete(SwapChain);
	}
	else
	{
		// Create a new swap-chain processing thread
		m_ProcessingThread.reset(new SwapChainProcessor(SwapChain, Device, NewFrameEvent));

		//create an event to get notified new cursor data
		HANDLE mouseEvent = CreateEventA(
			nullptr,
			false,
			false,
			"arbitraryMouseEventName"
		);

		if (!mouseEvent)
		{
			//do error handling
			return;
		}

		//set up cursor capabilities
		IDDCX_CURSOR_CAPS cursorInfo = {};
		cursorInfo.Size = sizeof(cursorInfo);
		cursorInfo.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL;
		cursorInfo.AlphaCursorSupport = true;
		cursorInfo.MaxX = 64;
		cursorInfo.MaxY = 64;

		//prepare IddCxMonitorSetupHardwareCursor arguments
		IDARG_IN_SETUP_HWCURSOR hwCursor = {};
		hwCursor.CursorInfo = cursorInfo;
		hwCursor.hNewCursorDataAvailable = mouseEvent; //this event will be called when new cursor data is available

		NTSTATUS Status = IddCxMonitorSetupHardwareCursor(
			MonitorObject, //handle to the monitor we want to enable hardware mouse on
			&hwCursor
		);

		if (FAILED(Status))
		{
			//do error handling
		}
	}
}

void IndirectMonitorContext::UnassignSwapChain()
{
	// Stop processing the last swap-chain
	m_ProcessingThread.reset();
}

#pragma endregion

#pragma region DDI Callbacks

void IndirectDeviceContext::_TestCreateMonitor() {
	auto connectorIndex = freeConnectorSlots.front();
	std::string idx = std::to_string(connectorIndex);
	std::string serialStr = "VDD2408";
	serialStr += idx;
	std::string dispName = "AllunoVDD #";
	dispName += idx;
	GUID containerId;
	CoCreateGuid(&containerId);
	uint8_t* edidData = generate_edid(containerId.Data1, serialStr.c_str(), dispName.c_str());

	VirtualMonitorMode mode{3000 + (DWORD)connectorIndex * 2, 2120 + (DWORD)connectorIndex, 120 + (DWORD)connectorIndex};

	IndirectMonitorContext* pContext;
	CreateMonitor(pContext, edidData, containerId, mode);
}

_Use_decl_annotations_
NTSTATUS AllunoVDDAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
	// UNREFERENCED_PARAMETER(AdapterObject);
	// UNREFERENCED_PARAMETER(pInArgs);

	if (NT_SUCCESS(pInArgs->AdapterInitStatus)) {
		if (preferredAdapterChanged) {
			IDARG_IN_ADAPTERSETRENDERADAPTER inArgs{preferredAdapterLuid};
			IddCxAdapterSetRenderAdapter(AdapterObject, &inArgs);
			preferredAdapterChanged = false;
		}
	}

	if (testMode) {
		auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
		if (NT_SUCCESS(pInArgs->AdapterInitStatus))
		{
			for (size_t i = 0; i < 3; i++) {
				pDeviceContextWrapper->pContext->_TestCreateMonitor();
			}
		}
	}

	// return STATUS_SUCCESS;
	return pInArgs->AdapterInitStatus;
}

_Use_decl_annotations_
NTSTATUS AllunoVDDAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
	UNREFERENCED_PARAMETER(AdapterObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AllunoVDDAdapterCommitModes2(
	IDDCX_ADAPTER AdapterObject,
	const IDARG_IN_COMMITMODES2* pInArgs
)
{
	UNREFERENCED_PARAMETER(AdapterObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AllunoVDDParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
	if (pInArgs->MonitorDescription.DataSize != sizeof(edid_base))
		return STATUS_INVALID_PARAMETER;

	VirtualMonitorMode* pPreferredMode = nullptr;

	for (auto &it: monitorCtxList) {
		if (memcmp(pInArgs->MonitorDescription.pData, it->pEdidData, sizeof(edid_base)) == 0) {
			if (it->preferredMode.Width) {
				pPreferredMode = &it->preferredMode;
			}
			break;
		}
	}

	if (pPreferredMode) {
		// Preferred mode set: only report preferred + scaled variants (no defaults).
		// This prevents Windows from picking a stale saved resolution.
		pOutArgs->MonitorModeBufferOutputCount = std::size(mode_scale_factors) * 2;
	} else {
		pOutArgs->MonitorModeBufferOutputCount = std::size(s_DefaultModes);
	}

	if (pInArgs->MonitorModeBufferInputCount < pOutArgs->MonitorModeBufferOutputCount)
	{
		// Return success if there was no buffer, since the caller was only asking for a count of modes
		return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
	}
	else
	{
		if (pPreferredMode) {
			auto width = pPreferredMode->Width;
			auto height = pPreferredMode->Height;
			auto vsync = pPreferredMode->VSync;

			for (uint8_t idx = 0; idx < std::size(mode_scale_factors); idx++) {
				auto scalc_factor = mode_scale_factors[idx];
				auto _width = width * scalc_factor / 100;
				auto _height = height * scalc_factor / 100;

				pInArgs->pMonitorModes[idx * 2] = CreateIddCxMonitorMode(
					_width,
					_height,
					vsync,
					IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR
				);

				pInArgs->pMonitorModes[idx * 2 + 1] = CreateIddCxMonitorMode(
					_width,
					_height,
					vsync * 2,
					IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR
				);
			}
			pOutArgs->PreferredMonitorModeIdx = 0;
		} else {
			float vsyncMultiplier = 1;

			for (DWORD ModeIndex = 0; ModeIndex < std::size(s_DefaultModes); ModeIndex++) {
				auto vsyncTarget = s_DefaultModes[ModeIndex].VSync;
				if (vsyncMultiplier != 1 && !(vsyncTarget % 1000)) {
					vsyncTarget = (DWORD)(vsyncTarget * vsyncMultiplier);
				}
				pInArgs->pMonitorModes[ModeIndex] = CreateIddCxMonitorMode(
					s_DefaultModes[ModeIndex].Width,
					s_DefaultModes[ModeIndex].Height,
					vsyncTarget,
					IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR
				);
			}
			pOutArgs->PreferredMonitorModeIdx = 1;
		}

		return STATUS_SUCCESS;
	}
}

_Use_decl_annotations_
NTSTATUS AllunoVDDParseMonitorDescription2(
	const IDARG_IN_PARSEMONITORDESCRIPTION2* pInArgs,
	IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs
)
{
	if (pInArgs->MonitorDescription.DataSize != sizeof(edid_base))
		return STATUS_INVALID_PARAMETER;

	VirtualMonitorMode* pPreferredMode = nullptr;
	UINT monitorBpc = 8;

	for (auto &it: monitorCtxList) {
		if (memcmp(pInArgs->MonitorDescription.pData, it->pEdidData, sizeof(edid_base)) == 0) {
			monitorBpc = it->bitsPerChannel;
			if (it->preferredMode.Width) {
				pPreferredMode = &it->preferredMode;
			}
			break;
		}
	}

	if (pPreferredMode) {
		pOutArgs->MonitorModeBufferOutputCount = std::size(mode_scale_factors) * 2;
	} else {
		pOutArgs->MonitorModeBufferOutputCount = std::size(s_DefaultModes);
	}

	if (pInArgs->MonitorModeBufferInputCount < pOutArgs->MonitorModeBufferOutputCount)
	{
		return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
	}
	else
	{
		if (pPreferredMode) {
			auto width = pPreferredMode->Width;
			auto height = pPreferredMode->Height;
			auto vsync = pPreferredMode->VSync;

			for (uint8_t idx = 0; idx < std::size(mode_scale_factors); idx++) {
				auto scalc_factor = mode_scale_factors[idx];
				auto _width = width * scalc_factor / 100;
				auto _height = height * scalc_factor / 100;

				pInArgs->pMonitorModes[idx * 2] = CreateIddCxMonitorMode2(
					_width,
					_height,
					vsync,
					IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR,
					monitorBpc
				);

				pInArgs->pMonitorModes[idx * 2 + 1] = CreateIddCxMonitorMode2(
					_width,
					_height,
					vsync * 2,
					IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR,
					monitorBpc
				);
			}

			pOutArgs->PreferredMonitorModeIdx = 0;
		} else {
			for (DWORD ModeIndex = 0; ModeIndex < std::size(s_DefaultModes); ModeIndex++) {
				pInArgs->pMonitorModes[ModeIndex] = CreateIddCxMonitorMode2(
					s_DefaultModes[ModeIndex].Width,
					s_DefaultModes[ModeIndex].Height,
					s_DefaultModes[ModeIndex].VSync,
					IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR,
					monitorBpc
				);
			}
			pOutArgs->PreferredMonitorModeIdx = 1;
		}

		return STATUS_SUCCESS;
	}
}

_Use_decl_annotations_
NTSTATUS AllunoVDDMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
	auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
	bool hasPreferred = pMonitorContextWrapper->pContext->preferredMode.Width > 0;

	if (hasPreferred) {
		pOutArgs->DefaultMonitorModeBufferOutputCount = std::size(mode_scale_factors) * 2;
		pOutArgs->PreferredMonitorModeIdx = 0;
	} else {
		pOutArgs->DefaultMonitorModeBufferOutputCount = std::size(s_DefaultModes);
		pOutArgs->PreferredMonitorModeIdx = 1;
	}

	if (pInArgs->DefaultMonitorModeBufferInputCount == 0) {
		return STATUS_SUCCESS;
	} else if (pInArgs->DefaultMonitorModeBufferInputCount < pOutArgs->DefaultMonitorModeBufferOutputCount) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	if (hasPreferred) {
		auto width = pMonitorContextWrapper->pContext->preferredMode.Width;
		auto height = pMonitorContextWrapper->pContext->preferredMode.Height;
		auto vsync = pMonitorContextWrapper->pContext->preferredMode.VSync;

		for (uint8_t idx = 0; idx < std::size(mode_scale_factors); idx++) {
			auto scalc_factor = mode_scale_factors[idx];
			auto _width = width * scalc_factor / 100;
			auto _height = height * scalc_factor / 100;

			pInArgs->pDefaultMonitorModes[idx * 2] = CreateIddCxMonitorMode(
				_width, _height, vsync,
				IDDCX_MONITOR_MODE_ORIGIN_DRIVER
			);
			pInArgs->pDefaultMonitorModes[idx * 2 + 1] = CreateIddCxMonitorMode(
				_width, _height, vsync * 2,
				IDDCX_MONITOR_MODE_ORIGIN_DRIVER
			);
		}
	} else {
		for (DWORD ModeIndex = 0; ModeIndex < std::size(s_DefaultModes); ModeIndex++) {
			pInArgs->pDefaultMonitorModes[ModeIndex] = CreateIddCxMonitorMode(
				s_DefaultModes[ModeIndex].Width,
				s_DefaultModes[ModeIndex].Height,
				s_DefaultModes[ModeIndex].VSync,
				IDDCX_MONITOR_MODE_ORIGIN_DRIVER
			);
		}
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AllunoVDDMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
	auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
	bool hasPreferred = pMonitorContextWrapper->pContext->preferredMode.Width > 0;

	if (hasPreferred) {
		pOutArgs->TargetModeBufferOutputCount = std::size(mode_scale_factors) * 2;
	} else {
		pOutArgs->TargetModeBufferOutputCount = (UINT) std::size(s_DefaultModes);
	}

	if (pInArgs->TargetModeBufferInputCount >= pOutArgs->TargetModeBufferOutputCount) {
		vector<IDDCX_TARGET_MODE> TargetModes;

		if (hasPreferred) {
			auto width = pMonitorContextWrapper->pContext->preferredMode.Width;
			auto height = pMonitorContextWrapper->pContext->preferredMode.Height;
			auto vsync = pMonitorContextWrapper->pContext->preferredMode.VSync;

			for (uint8_t idx = 0; idx < std::size(mode_scale_factors); idx++) {
				auto scalc_factor = mode_scale_factors[idx];
				auto _width = width * scalc_factor / 100;
				auto _height = height * scalc_factor / 100;

				TargetModes.push_back(CreateIddCxTargetMode(_width, _height, vsync));
				TargetModes.push_back(CreateIddCxTargetMode(_width, _height, vsync * 2));
			}
		} else {
			for (size_t i = 0; i < std::size(s_DefaultModes); i++) {
				TargetModes.push_back(CreateIddCxTargetMode(
					s_DefaultModes[i].Width,
					s_DefaultModes[i].Height,
					s_DefaultModes[i].VSync
				));
			}
		}

		copy(TargetModes.begin(), TargetModes.end(), pInArgs->pTargetModes);
	} else if (pInArgs->TargetModeBufferInputCount != 0) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AllunoVDDMonitorQueryModes2(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES2* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
	auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
	UINT monitorBpc = pMonitorContextWrapper->pContext->bitsPerChannel;
	bool hasPreferred = pMonitorContextWrapper->pContext->preferredMode.Width > 0;

	if (hasPreferred) {
		pOutArgs->TargetModeBufferOutputCount = std::size(mode_scale_factors) * 2;
	} else {
		pOutArgs->TargetModeBufferOutputCount = (UINT) std::size(s_DefaultModes);
	}

	if (pInArgs->TargetModeBufferInputCount >= pOutArgs->TargetModeBufferOutputCount) {
		vector<IDDCX_TARGET_MODE2> TargetModes;

		if (hasPreferred) {
			auto width = pMonitorContextWrapper->pContext->preferredMode.Width;
			auto height = pMonitorContextWrapper->pContext->preferredMode.Height;
			auto vsync = pMonitorContextWrapper->pContext->preferredMode.VSync;

			for (uint8_t idx = 0; idx < std::size(mode_scale_factors); idx++) {
				auto scalc_factor = mode_scale_factors[idx];
				auto _width = width * scalc_factor / 100;
				auto _height = height * scalc_factor / 100;

				TargetModes.push_back(CreateIddCxTargetMode2(_width, _height, vsync, monitorBpc));
				TargetModes.push_back(CreateIddCxTargetMode2(_width, _height, vsync * 2, monitorBpc));
			}
		} else {
			for (size_t i = 0; i < std::size(s_DefaultModes); i++) {
				TargetModes.push_back(CreateIddCxTargetMode2(
					s_DefaultModes[i].Width,
					s_DefaultModes[i].Height,
					s_DefaultModes[i].VSync,
					monitorBpc
				));
			}
		}

		copy(TargetModes.begin(), TargetModes.end(), pInArgs->pTargetModes);
	} else if (pInArgs->TargetModeBufferInputCount != 0) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AllunoVDDMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
	auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);

	if (preferredAdapterChanged) {
		if (memcmp(&pInArgs->RenderAdapterLuid, &preferredAdapterLuid, sizeof(LUID))) {
			IDARG_IN_ADAPTERSETRENDERADAPTER inArgs{preferredAdapterLuid};
			IddCxAdapterSetRenderAdapter(pMonitorContextWrapper->pContext->m_Adapter, &inArgs);
			return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
		}
		preferredAdapterChanged = false;
	}

	pMonitorContextWrapper->pContext->AssignSwapChain(MonitorObject, pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AllunoVDDMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
	auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
	pMonitorContextWrapper->pContext->UnassignSwapChain();
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AllunoVDDAdapterQueryTargetInfo(
	IDDCX_ADAPTER AdapterObject,
	IDARG_IN_QUERYTARGET_INFO* pInArgs,
	IDARG_OUT_QUERYTARGET_INFO* pOutArgs
)
{
	UNREFERENCED_PARAMETER(AdapterObject);

	UINT bpc = 8;
	if (pInArgs->ConnectorIndex < 16) {
		bpc = connectorBpc[pInArgs->ConnectorIndex];
	}

	if (bpc >= 10) {
		pOutArgs->TargetCaps = IDDCX_TARGET_CAPS_HIGH_COLOR_SPACE | IDDCX_TARGET_CAPS_WIDE_COLOR_SPACE;
		pOutArgs->DitheringSupport.Rgb = HDRBITS;
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AllunoVDDMonitorSetDefaultHdrMetadata(
	IDDCX_MONITOR MonitorObject,
	const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA* pInArgs
)
{
	UNREFERENCED_PARAMETER(MonitorObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AllunoVDDMonitorSetGammaRamp(
	IDDCX_MONITOR MonitorObject,
	const IDARG_IN_SET_GAMMARAMP* pInArgs
)
{
	UNREFERENCED_PARAMETER(MonitorObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

#pragma endregion

VOID AllunoVDDIoDeviceControl(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request,
	_In_ size_t OutputBufferLength,
	_In_ size_t InputBufferLength,
	_In_ ULONG IoControlCode
)
{
	// Reset watchdog
	if (IoControlCode != IOCTL_ALLUNO_VDD_GET_WATCHDOG) {
		watchdogCountdown = watchdogTimeout;
	}

	NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
	size_t bytesReturned = 0;

	switch (IoControlCode) {
	case IOCTL_ALLUNO_VDD_ADD_DISPLAY: {
		if (freeConnectorSlots.empty()) {
			Status = STATUS_TOO_MANY_NODES;
			break;
		}

		if (InputBufferLength < sizeof(ALLUNO_VDD_ADD_PARAMS) || OutputBufferLength < sizeof(ALLUNO_VDD_ADD_RESULT)) {
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		ALLUNO_VDD_ADD_PARAMS* params;
		ALLUNO_VDD_ADD_RESULT* output;
		Status = WdfRequestRetrieveInputBuffer(Request, sizeof(ALLUNO_VDD_ADD_PARAMS), (PVOID*)&params, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ALLUNO_VDD_ADD_RESULT), (PVOID*)&output, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		bool guidFound = false;

		for (auto it = monitorCtxList.begin(); it != monitorCtxList.end(); ++it) {
			auto* ctx = *it;
			if (ctx->monitorGuid == params->MonitorGuid) {
				guidFound = true;
				output->AdapterLuid = ctx->adapterLuid;
				output->TargetId = ctx->targetId;
				bytesReturned = sizeof(ALLUNO_VDD_ADD_RESULT);
				break;
			}
		}

		if (guidFound) {
			Status = STATUS_SUCCESS;
			break;
		}

		// Validate and add the virtual display
		if (params->Width > 0 && params->Height > 0 && params->RefreshRate > 0) {
			std::lock_guard<std::mutex> lg(monitorListOp);

			auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(Device);

			IndirectMonitorContext* pMonitorContext;
			uint8_t* edidData = generate_edid(params->MonitorGuid.Data1, params->SerialNumber, params->DeviceName);
			VirtualMonitorMode preferredMode = {params->Width, params->Height, params->RefreshRate};
			if (preferredMode.VSync < 1000) {
				preferredMode.VSync *= 1000;
			}

			// Override VSync with fractional rate if provided
			if (params->VsyncNumerator > 0 && params->VsyncDenominator > 0) {
				// Convert fraction to millihertz: (Num / Den) * 1000
				preferredMode.VSync = (DWORD)((UINT64)params->VsyncNumerator * 1000 / params->VsyncDenominator);
			}

			Status = pDeviceContextWrapper->pContext->CreateMonitor(pMonitorContext, edidData, params->MonitorGuid, preferredMode, params->BitsPerChannel);

			if (!NT_SUCCESS(Status)) {
				break;
			}

			// Store extended fields on the context
			pMonitorContext->hdrMode = params->HdrMode;
			pMonitorContext->vsyncNumerator = params->VsyncNumerator;
			pMonitorContext->vsyncDenominator = params->VsyncDenominator;

			output->AdapterLuid = pMonitorContext->adapterLuid;
			output->TargetId = pMonitorContext->targetId;
			bytesReturned = sizeof(ALLUNO_VDD_ADD_RESULT);
		}
		else {
			Status = STATUS_INVALID_PARAMETER;
		}

		break;
	}
	case IOCTL_ALLUNO_VDD_REMOVE_DISPLAY: {
		if (InputBufferLength < sizeof(ALLUNO_VDD_REMOVE_PARAMS)) {
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		ALLUNO_VDD_REMOVE_PARAMS* params;
		Status = WdfRequestRetrieveInputBuffer(Request, sizeof(ALLUNO_VDD_REMOVE_PARAMS), (PVOID*)&params, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		Status = STATUS_NOT_FOUND;

		std::lock_guard<std::mutex> lg(monitorListOp);

		for (auto it = monitorCtxList.begin(); it != monitorCtxList.end(); ++it) {
			auto* ctx = *it;
			if (ctx->monitorGuid == params->MonitorGuid) {
				// Remove the monitor
				if (ctx->connectorId < 16) {
					connectorBpc[ctx->connectorId] = 0;
				}
				freeConnectorSlots.push(ctx->connectorId);
				IddCxMonitorDeparture(ctx->GetMonitor());
				monitorCtxList.erase(it);
				Status = STATUS_SUCCESS;
				break;
			}
		}

		break;
	}
	case IOCTL_ALLUNO_VDD_SET_RENDER_ADAPTER: {
		ALLUNO_VDD_SET_ADAPTER_PARAMS* params;
		Status = WdfRequestRetrieveInputBuffer(Request, sizeof(ALLUNO_VDD_SET_ADAPTER_PARAMS), (PVOID*)&params, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(Device);

		preferredAdapterLuid = params->AdapterLuid;
		pDeviceContextWrapper->pContext->SetRenderAdapter(params->AdapterLuid);
		preferredAdapterChanged = true;

		break;
	}
	case IOCTL_ALLUNO_VDD_GET_WATCHDOG: {
		Status = STATUS_SUCCESS;
		ALLUNO_VDD_WATCHDOG_PARAMS* output;

		Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ALLUNO_VDD_WATCHDOG_PARAMS), (PVOID*)&output, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		output->TimeoutMs = watchdogTimeout * 1000;
		output->CountdownMs = watchdogCountdown * 1000;
		bytesReturned = sizeof(ALLUNO_VDD_WATCHDOG_PARAMS);
		break;
	}
	case IOCTL_ALLUNO_VDD_UPDATE_MODE: {
		if (InputBufferLength < sizeof(ALLUNO_VDD_UPDATE_MODE_PARAMS)) {
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		ALLUNO_VDD_UPDATE_MODE_PARAMS* params;
		Status = WdfRequestRetrieveInputBuffer(Request, sizeof(ALLUNO_VDD_UPDATE_MODE_PARAMS), (PVOID*)&params, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		Status = STATUS_NOT_FOUND;

		std::lock_guard<std::mutex> lg(monitorListOp);

		for (auto it = monitorCtxList.begin(); it != monitorCtxList.end(); ++it) {
			auto* ctx = *it;
			if (ctx->monitorGuid == params->MonitorGuid) {
				// Update fields (skip 0 values = no change)
				if (params->Width > 0) ctx->preferredMode.Width = params->Width;
				if (params->Height > 0) ctx->preferredMode.Height = params->Height;
				if (params->RefreshRate > 0) {
					ctx->preferredMode.VSync = params->RefreshRate;
					if (ctx->preferredMode.VSync < 1000) {
						ctx->preferredMode.VSync *= 1000;
					}
				}
				if (params->BitsPerChannel > 0) ctx->bitsPerChannel = params->BitsPerChannel;
				if (params->HdrMode != 0xFF) ctx->hdrMode = params->HdrMode;

				// Override VSync with fractional rate if provided
				if (params->VsyncNumerator > 0 && params->VsyncDenominator > 0) {
					ctx->preferredMode.VSync = (DWORD)((UINT64)params->VsyncNumerator * 1000 / params->VsyncDenominator);
					ctx->vsyncNumerator = params->VsyncNumerator;
					ctx->vsyncDenominator = params->VsyncDenominator;
				}

				// Reconnect: departure + re-create + arrival with same connectorId and GUID
				auto connId = ctx->connectorId;
				auto guid = ctx->monitorGuid;
				auto mode = ctx->preferredMode;
				auto bpc = ctx->bitsPerChannel;
				auto hdr = ctx->hdrMode;
				auto vN = ctx->vsyncNumerator;
				auto vD = ctx->vsyncDenominator;

				// Save custom EDID info before departure
				bool hasCustom = ctx->hasCustomEdid;
				uint8_t* customData = ctx->pCustomEdidData;
				UINT customSize = ctx->customEdidSize;

				// Depart old monitor
				freeConnectorSlots.push(connId);
				IddCxMonitorDeparture(ctx->GetMonitor());
				monitorCtxList.erase(it);

				// Create new monitor with same GUID
				auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(Device);
				IndirectMonitorContext* pNewCtx;

				uint8_t* edidData;
				if (hasCustom && customData) {
					edidData = (uint8_t*)malloc(customSize);
					if (edidData) {
						memcpy(edidData, customData, customSize);
					}
				} else {
					edidData = generate_edid(guid.Data1, "", "Alluno");
				}

				Status = pDeviceContextWrapper->pContext->CreateMonitor(pNewCtx, edidData, guid, mode, bpc);
				if (NT_SUCCESS(Status)) {
					pNewCtx->hdrMode = hdr;
					pNewCtx->vsyncNumerator = vN;
					pNewCtx->vsyncDenominator = vD;
					if (hasCustom && customData) {
						pNewCtx->hasCustomEdid = true;
						pNewCtx->pCustomEdidData = customData;
						pNewCtx->customEdidSize = customSize;
					}
				} else {
					// Free custom EDID data if reconnect failed
					if (hasCustom && customData) {
						free(customData);
					}
				}

				break;
			}
		}

		break;
	}
	case IOCTL_ALLUNO_VDD_LIST_DISPLAYS: {
		if (OutputBufferLength < sizeof(ALLUNO_VDD_LIST_RESULT)) {
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		ALLUNO_VDD_LIST_RESULT* output;
		Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ALLUNO_VDD_LIST_RESULT), (PVOID*)&output, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		memset(output, 0, sizeof(ALLUNO_VDD_LIST_RESULT));

		{
			std::lock_guard<std::mutex> lg(monitorListOp);
			UINT idx = 0;
			for (auto it = monitorCtxList.begin(); it != monitorCtxList.end() && idx < ALLUNO_VDD_MAX_DISPLAYS; ++it, ++idx) {
				auto* ctx = *it;
				ALLUNO_VDD_DISPLAY_INFO& info = output->Displays[idx];
				info.MonitorGuid = ctx->monitorGuid;
				info.Width = ctx->preferredMode.Width;
				info.Height = ctx->preferredMode.Height;
				info.RefreshRate = ctx->preferredMode.VSync;
				info.BitsPerChannel = ctx->bitsPerChannel;
				info.HdrMode = ctx->hdrMode;
				info.AdapterLuid = ctx->adapterLuid;
				info.TargetId = ctx->targetId;
				info.Active = TRUE;
				info.VsyncNumerator = ctx->vsyncNumerator;
				info.VsyncDenominator = ctx->vsyncDenominator;
			}
			output->Count = idx;
		}

		bytesReturned = sizeof(ALLUNO_VDD_LIST_RESULT);
		Status = STATUS_SUCCESS;
		break;
	}
	case IOCTL_ALLUNO_VDD_REMOVE_ALL: {
		DisconnectAllMonitors();
		Status = STATUS_SUCCESS;
		break;
	}
	case IOCTL_ALLUNO_VDD_SET_WATCHDOG: {
		if (InputBufferLength < sizeof(ALLUNO_VDD_SET_WATCHDOG_PARAMS)) {
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		ALLUNO_VDD_SET_WATCHDOG_PARAMS* params;
		Status = WdfRequestRetrieveInputBuffer(Request, sizeof(ALLUNO_VDD_SET_WATCHDOG_PARAMS), (PVOID*)&params, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		// Convert milliseconds to seconds (min 1 if non-zero)
		DWORD newTimeout = params->TimeoutMs / 1000;
		if (params->TimeoutMs > 0 && newTimeout == 0) {
			newTimeout = 1;
		}
		watchdogTimeout = newTimeout;
		watchdogCountdown = newTimeout;
		Status = STATUS_SUCCESS;
		break;
	}
	case IOCTL_ALLUNO_VDD_SET_HDR: {
		if (InputBufferLength < sizeof(ALLUNO_VDD_SET_HDR_PARAMS)) {
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		ALLUNO_VDD_SET_HDR_PARAMS* params;
		Status = WdfRequestRetrieveInputBuffer(Request, sizeof(ALLUNO_VDD_SET_HDR_PARAMS), (PVOID*)&params, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		Status = STATUS_NOT_FOUND;

		std::lock_guard<std::mutex> lg(monitorListOp);

		for (auto it = monitorCtxList.begin(); it != monitorCtxList.end(); ++it) {
			auto* ctx = *it;
			if (ctx->monitorGuid == params->MonitorGuid) {
				// Update HDR fields
				ctx->bitsPerChannel = params->BitsPerChannel ? params->BitsPerChannel : ctx->bitsPerChannel;
				ctx->hdrMode = params->HdrMode;

				if (params->HasMetadata) {
					ctx->hasHdrMetadata = true;
					ctx->hdrMetadata = params->Metadata;
				}

				// Trigger mode re-negotiation via departure/arrival cycle
				auto connId = ctx->connectorId;
				auto guid = ctx->monitorGuid;
				auto mode = ctx->preferredMode;
				auto bpc = ctx->bitsPerChannel;
				auto hdr = ctx->hdrMode;
				auto vN = ctx->vsyncNumerator;
				auto vD = ctx->vsyncDenominator;
				bool hasMetadata = ctx->hasHdrMetadata;
				ALLUNO_VDD_HDR_METADATA metadata = ctx->hdrMetadata;

				bool hasCustom = ctx->hasCustomEdid;
				uint8_t* customData = ctx->pCustomEdidData;
				UINT customSize = ctx->customEdidSize;

				freeConnectorSlots.push(connId);
				IddCxMonitorDeparture(ctx->GetMonitor());
				monitorCtxList.erase(it);

				auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(Device);
				IndirectMonitorContext* pNewCtx;

				uint8_t* edidData;
				if (hasCustom && customData) {
					edidData = (uint8_t*)malloc(customSize);
					if (edidData) {
						memcpy(edidData, customData, customSize);
					}
				} else {
					edidData = generate_edid(guid.Data1, "", "Alluno");
				}

				Status = pDeviceContextWrapper->pContext->CreateMonitor(pNewCtx, edidData, guid, mode, bpc);
				if (NT_SUCCESS(Status)) {
					pNewCtx->hdrMode = hdr;
					pNewCtx->vsyncNumerator = vN;
					pNewCtx->vsyncDenominator = vD;
					pNewCtx->hasHdrMetadata = hasMetadata;
					pNewCtx->hdrMetadata = metadata;
					if (hasCustom && customData) {
						pNewCtx->hasCustomEdid = true;
						pNewCtx->pCustomEdidData = customData;
						pNewCtx->customEdidSize = customSize;
					}
				} else {
					if (hasCustom && customData) {
						free(customData);
					}
				}

				break;
			}
		}

		break;
	}
	case IOCTL_ALLUNO_VDD_SET_CUSTOM_EDID: {
		if (InputBufferLength < sizeof(ALLUNO_VDD_SET_CUSTOM_EDID_PARAMS)) {
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		ALLUNO_VDD_SET_CUSTOM_EDID_PARAMS* params;
		Status = WdfRequestRetrieveInputBuffer(Request, sizeof(ALLUNO_VDD_SET_CUSTOM_EDID_PARAMS), (PVOID*)&params, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		// Validate EDID size (must be 128 or 256)
		if (params->EdidSize != 128 && params->EdidSize != 256) {
			Status = STATUS_INVALID_PARAMETER;
			break;
		}

		Status = STATUS_NOT_FOUND;

		std::lock_guard<std::mutex> lg(monitorListOp);

		for (auto it = monitorCtxList.begin(); it != monitorCtxList.end(); ++it) {
			auto* ctx = *it;
			if (ctx->monitorGuid == params->MonitorGuid) {
				// Free old custom EDID if any
				if (ctx->pCustomEdidData) {
					free(ctx->pCustomEdidData);
					ctx->pCustomEdidData = nullptr;
				}

				// Allocate and copy new EDID
				ctx->pCustomEdidData = (uint8_t*)malloc(params->EdidSize);
				if (!ctx->pCustomEdidData) {
					Status = STATUS_INSUFFICIENT_RESOURCES;
					break;
				}

				memcpy(ctx->pCustomEdidData, params->EdidData, params->EdidSize);
				ctx->customEdidSize = params->EdidSize;
				ctx->hasCustomEdid = true;

				Status = STATUS_SUCCESS;
				break;
			}
		}

		break;
	}
	case IOCTL_ALLUNO_VDD_PING: {
		Status = STATUS_SUCCESS;
		break;
	}
	case IOCTL_ALLUNO_VDD_GET_VERSION: {
		if (OutputBufferLength < sizeof(ALLUNO_VDD_VERSION)) {
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		ALLUNO_VDD_VERSION* output;
		Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ALLUNO_VDD_VERSION), (PVOID*)&output, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		output->Major = ALLUNO_VDD_VERSION_MAJOR;
		output->Minor = ALLUNO_VDD_VERSION_MINOR;
		output->Patch = ALLUNO_VDD_VERSION_PATCH;
		output->Build = ALLUNO_VDD_VERSION_BUILD;
		bytesReturned = sizeof(ALLUNO_VDD_VERSION);
		Status = STATUS_SUCCESS;
		break;
	}
	default:
		break;
	}

	WdfRequestCompleteWithInformation(Request, Status, bytesReturned);
}
