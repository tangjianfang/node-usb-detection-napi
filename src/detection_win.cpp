#include <napi.h>
#include <windows.h>
#include <dbt.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <iostream>
#include <algorithm>
#include <thread>
#include <atomic>
#include <memory>
#include <string>
#include "detection.h"

using namespace Napi;

// Macro definitions
#define VID_TAG "VID_"
#define PID_TAG "PID_"
#define MAX_THREAD_WINDOW_NAME 64
#define LIBRARY_NAME ("setupapi.dll")

// Platform-specific global variables
namespace {
    std::atomic_bool isMonitoring{ false };
    std::thread listenerThread;
    HDEVNOTIFY hDevNotify = nullptr;
    HWND hwnd = nullptr;

    HINSTANCE hinstLib;
    typedef BOOL(WINAPI* _SetupDiEnumDeviceInfo)(HDEVINFO DeviceInfoSet, DWORD MemberIndex, PSP_DEVINFO_DATA DeviceInfoData);
    typedef HDEVINFO(WINAPI* _SetupDiGetClassDevs)(const GUID* ClassGuid, PCTSTR Enumerator, HWND hwndParent, DWORD Flags);
    typedef BOOL(WINAPI* _SetupDiDestroyDeviceInfoList)(HDEVINFO DeviceInfoSet);
    typedef BOOL(WINAPI* _SetupDiGetDeviceInstanceId)(HDEVINFO DeviceInfoSet, PSP_DEVINFO_DATA DeviceInfoData, PTSTR DeviceInstanceId, DWORD DeviceInstanceIdSize, PDWORD RequiredSize);
    typedef BOOL(WINAPI* _SetupDiGetDeviceRegistryProperty)(HDEVINFO DeviceInfoSet, PSP_DEVINFO_DATA DeviceInfoData, DWORD Property, PDWORD PropertyRegDataType, PBYTE PropertyBuffer, DWORD PropertyBufferSize, PDWORD RequiredSize);
    typedef BOOL(WINAPI* _SetupDiGetDeviceProperty)(HDEVINFO DeviceInfoSet, PSP_DEVINFO_DATA DeviceInfoData, const DEVPROPKEY* PropertyKey, DEVPROPTYPE* PropertyType, PBYTE PropertyBuffer, DWORD PropertyBufferSize, PDWORD RequiredSize, DWORD Flags);

    _SetupDiEnumDeviceInfo DllSetupDiEnumDeviceInfo;
    _SetupDiGetClassDevs DllSetupDiGetClassDevs;
    _SetupDiDestroyDeviceInfoList DllSetupDiDestroyDeviceInfoList;
    _SetupDiGetDeviceInstanceId DllSetupDiGetDeviceInstanceId;
    _SetupDiGetDeviceRegistryProperty DllSetupDiGetDeviceRegistryProperty;
    _SetupDiGetDeviceProperty DllSetupDiGetDeviceProperty;

    GUID GUID_DEVINTERFACE_USB_DEVICE = {
        0xA5DCBF10L, 0x6530, 0x11D2,
        {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}
    };

    struct WinDeviceInfo {
        std::string deviceId;
        ListResultItem_t deviceData;
    };

    // Thread-safe callbacks
    ThreadSafeFunction addedTsFunc;
    ThreadSafeFunction removedTsFunc;
}

// Helper function declarations
static std::string WideToUTF8(const std::wstring& wstr);
static std::wstring UTF8ToWide(const std::string& str);
static void ExtractDeviceInfo(HDEVINFO hDevInfo, SP_DEVINFO_DATA* pspDevInfoData, TCHAR* buf, DWORD buffSize, ListResultItem_t* resultItem);
static void NotifyJsCallback(bool isAdded, const WinDeviceInfo& device);
static std::string Utf8Encode(const std::string& str);

// Local functions
void ToUpper(char* buf) {
    char* c = buf;
    while (*c != '\0') {
        *c = toupper((unsigned char)*c);
        c++;
    }
}

void NormalizeSlashes(char* buf) {
    char* c = buf;
    while (*c != '\0') {
        if (*c == '/') {
            *c = '\\';
        }
        c++;
    }
}

void extractVidPid(char* buf, ListResultItem_t* item) {
    if (buf == NULL) {
        return;
    }

    ToUpper(buf);

    char* string;
    char* temp;
    char* pidStr, * vidStr;
    int vid = 0;
    int pid = 0;

    string = new char[strlen(buf) + 1];
    memcpy(string, buf, strlen(buf) + 1);

    vidStr = strstr(string, VID_TAG);
    pidStr = strstr(string, PID_TAG);

    if (vidStr != NULL) {
        temp = (char*)(vidStr + strlen(VID_TAG));
        temp[4] = '\0';
        vid = strtol(temp, NULL, 16);
    }

    if (pidStr != NULL) {
        temp = (char*)(pidStr + strlen(PID_TAG));
        temp[4] = '\0';
        pid = strtol(temp, NULL, 16);
    }
    item->vendorId = vid;
    item->productId = pid;

    delete[] string;
}

// Window procedure
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DEVICECHANGE) {
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
            PDEV_BROADCAST_HDR pHdr = reinterpret_cast<PDEV_BROADCAST_HDR>(lParam);
            if (pHdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                auto* pDevInf = reinterpret_cast<PDEV_BROADCAST_DEVICEINTERFACE>(pHdr);

                WinDeviceInfo device;
                device.deviceId = pDevInf->dbcc_name;
                std::replace(device.deviceId.begin(), device.deviceId.end(), '#', '\\');

                NotifyJsCallback(wParam == DBT_DEVICEARRIVAL, device);
            }
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Listener thread
static DWORD WINAPI ListenerThreadMain() {
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"NapiUsbDetection";

    if (!RegisterClassW(&wc)) {
        return GetLastError();
    }

    hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
    if (!hwnd) {
        return GetLastError();
    }

    DEV_BROADCAST_DEVICEINTERFACE_W filter = { 0 };
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_USB_DEVICE;

    hDevNotify = RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!hDevNotify) {
        return GetLastError();
    }

    MSG msg;
    while (isMonitoring) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            WaitMessage();
        }
    }

    return 0;
}

// Load functions from DLL
void LoadFunctions() {
    bool success;
    hinstLib = LoadLibraryA(LIBRARY_NAME);
    if (hinstLib != NULL) {
        DllSetupDiEnumDeviceInfo = (_SetupDiEnumDeviceInfo)GetProcAddress(hinstLib, "SetupDiEnumDeviceInfo");
        DllSetupDiGetClassDevs = (_SetupDiGetClassDevs)GetProcAddress(hinstLib, "SetupDiGetClassDevsA");
        DllSetupDiDestroyDeviceInfoList = (_SetupDiDestroyDeviceInfoList)GetProcAddress(hinstLib, "SetupDiDestroyDeviceInfoList");
        DllSetupDiGetDeviceInstanceId = (_SetupDiGetDeviceInstanceId)GetProcAddress(hinstLib, "SetupDiGetDeviceInstanceIdA");
        DllSetupDiGetDeviceRegistryProperty = (_SetupDiGetDeviceRegistryProperty)GetProcAddress(hinstLib, "SetupDiGetDeviceRegistryPropertyA");
        DllSetupDiGetDeviceProperty = (_SetupDiGetDeviceProperty)GetProcAddress(hinstLib, "SetupDiGetDevicePropertyW");

        success = (
            DllSetupDiEnumDeviceInfo != NULL &&
            DllSetupDiGetClassDevs != NULL &&
            DllSetupDiDestroyDeviceInfoList != NULL &&
            DllSetupDiGetDeviceInstanceId != NULL &&
            DllSetupDiGetDeviceRegistryProperty != NULL &&
            DllSetupDiGetDeviceProperty != NULL
        );
    } else {
        success = false;
    }

    if (!success) {
        printf("Could not load library functions from dll -> abort (Check if %s is available)\r\n", LIBRARY_NAME);
        exit(1);
    }
}

// Build initial device list
void BuildInitialDeviceList() {
    DWORD dwFlag = (DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    GUID guid = GUID_DEVINTERFACE_USB_DEVICE;
    HDEVINFO hDevInfo = DllSetupDiGetClassDevs(&guid, NULL, NULL, dwFlag);

    if (INVALID_HANDLE_VALUE == hDevInfo) {
        return;
    }

    SP_DEVINFO_DATA* pspDevInfoData = (SP_DEVINFO_DATA*)HeapAlloc(GetProcessHeap(), 0, sizeof(SP_DEVINFO_DATA));
    if (pspDevInfoData) {
        pspDevInfoData->cbSize = sizeof(SP_DEVINFO_DATA);
        for (int i = 0; DllSetupDiEnumDeviceInfo(hDevInfo, i, pspDevInfoData); i++) {
            DWORD nSize = 0;
            TCHAR buf[MAX_PATH];

            if (!DllSetupDiGetDeviceInstanceId(hDevInfo, pspDevInfoData, buf, sizeof(buf), &nSize)) {
                break;
            }
            NormalizeSlashes(buf);

            DeviceItem_t* item = new DeviceItem_t();
            item->deviceState = DeviceState_Connect;

            DWORD DataT;
            DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_LOCATION_INFORMATION, &DataT, (PBYTE)buf, MAX_PATH, &nSize);
            DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_HARDWAREID, &DataT, (PBYTE)(buf + nSize - 1), MAX_PATH - nSize, &nSize);

            AddItemToList(buf, item);
            ExtractDeviceInfo(hDevInfo, pspDevInfoData, buf, MAX_PATH, &item->deviceParams);
        }

        HeapFree(GetProcessHeap(), 0, pspDevInfoData);
    }

    if (hDevInfo) {
        DllSetupDiDestroyDeviceInfoList(hDevInfo);
    }
}

// Platform initialization
void PlatformInit() {
    LoadFunctions();
    BuildInitialDeviceList();
}

// Find operation
void EIO_Find(napi_env env, void* data) {
    ListBaton* baton = static_cast<ListBaton*>(data);

    try {
        CreateFilteredList(&baton->results, baton->vid, baton->pid);
    } catch (const std::exception& e) {
        strncpy(baton->errorString, e.what(), sizeof(baton->errorString) - 1);
    }
}

// Start monitoring
void PlatformStartMonitoring() {
    if (isMonitoring.exchange(true)) return;

    listenerThread = std::thread([] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ListenerThreadMain();
        CoUninitialize();
    });
}

// Stop monitoring
void PlatformStopMonitoring() {
    if (!isMonitoring.exchange(false)) return;

    if (hDevNotify) {
        UnregisterDeviceNotification(hDevNotify);
        hDevNotify = nullptr;
    }

    if (hwnd) {
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }

    if (listenerThread.joinable()) {
        PostThreadMessage(GetThreadId(listenerThread.native_handle()), WM_QUIT, 0, 0);
        listenerThread.join();
    }

    addedTsFunc.Release();
    removedTsFunc.Release();
}

// Register added callback
void RegisterAdded(const CallbackInfo& info) {
    if (info.Length() < 1 || !info[0].IsFunction()) {
        throw Error::New(info.Env(), "A function parameter needs to be passed in.");
    }
    addedTsFunc = ThreadSafeFunction::New(info.Env(), info[0].As<Function>(), "AddedCallback", 0, 1);
}

// Register removed callback
void RegisterRemoved(const CallbackInfo& info) {
    if (info.Length() < 1 || !info[0].IsFunction()) {
        throw Error::New(info.Env(), "A function parameter needs to be passed in.");
    }
    removedTsFunc = ThreadSafeFunction::New(info.Env(), info[0].As<Function>(), "RemovedCallback", 0, 1);
}

// Helper function implementations
static std::string WideToUTF8(const std::wstring& wstr) {
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string str(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], size, nullptr, nullptr);
    str.pop_back(); // Remove the null terminator
    return str;
}

static std::wstring UTF8ToWide(const std::string& str) {
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring wstr(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], size);
    wstr.pop_back(); // Remove the null terminator
    return wstr;
}

static void NotifyJsCallback(bool isAdded, const WinDeviceInfo& device) {
    auto tsFunc = isAdded ? addedTsFunc : removedTsFunc;
    if (!tsFunc) return;

    auto deviceData = std::make_shared<ListResultItem_t>(device.deviceData);

    tsFunc.BlockingCall(deviceData.get(), [deviceData](Napi::Env env, Napi::Function callback, ListResultItem_t* data) {
        Napi::Object obj = Napi::Object::New(env);
        obj.Set("vendorId", data->vendorId);
        obj.Set("productId", data->productId);
        obj.Set("serialNumber", data->serialNumber);
        obj.Set("deviceName", data->deviceName);
        obj.Set("manufacturer", data->manufacturer);
        callback.Call({ obj });
    });
}

void ExtractDeviceInfo(HDEVINFO hDevInfo, SP_DEVINFO_DATA* pspDevInfoData, TCHAR* buf, DWORD buffSize, ListResultItem_t* resultItem) {
    DWORD DataT;
    DWORD nSize;
    static int dummy = 1;

    resultItem->locationId = 0;
    resultItem->deviceAddress = dummy++;

    // Device found
    if (DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_FRIENDLYNAME, &DataT, (PBYTE)buf, buffSize, &nSize)) {
        resultItem->deviceName = Utf8Encode(buf);
    } else if (DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_DEVICEDESC, &DataT, (PBYTE)buf, buffSize, &nSize)) {
        resultItem->deviceName = Utf8Encode(buf);
    }
    if (DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_MFG, &DataT, (PBYTE)buf, buffSize, &nSize)) {
        resultItem->manufacturer = Utf8Encode(buf);
    }
    if (DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_HARDWAREID, &DataT, (PBYTE)buf, buffSize, &nSize)) {
        // Use this to extract VID / PID
        extractVidPid(buf, resultItem);
    }

    // Extract Serial Number
    DWORD dwCapabilities = 0x0;
    if (DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_CAPABILITIES, &DataT, (PBYTE)&dwCapabilities, sizeof(dwCapabilities), &nSize)) {
        if ((dwCapabilities & CM_DEVCAP_UNIQUEID) == CM_DEVCAP_UNIQUEID) {
            if (DllSetupDiGetDeviceInstanceId(hDevInfo, pspDevInfoData, buf, buffSize, &nSize)) {
                std::string deviceInstanceId = buf;
                size_t serialNumberIndex = deviceInstanceId.find_last_of("\\");
                if (serialNumberIndex != std::string::npos) {
                    resultItem->serialNumber = deviceInstanceId.substr(serialNumberIndex + 1);
                }
            }
        }
    }
}

std::string Utf8Encode(const std::string& str) {
    if (str.empty()) {
        return std::string();
    }

    // System default code page to wide character
    int wstr_size = MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, NULL, 0);
    std::wstring wstr_tmp(wstr_size, 0);
    MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, &wstr_tmp[0], wstr_size);

    // Wide character to Utf8
    int str_size = WideCharToMultiByte(CP_UTF8, 0, &wstr_tmp[0], (int)wstr_tmp.size(), NULL, 0, NULL, NULL);
    std::string str_utf8(str_size, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr_tmp[0], (int)wstr_tmp.size(), &str_utf8[0], str_size, NULL, NULL);

    return str_utf8;
}