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
#include <tchar.h>
#include "detection.h"

using namespace Napi;

// Macro definitions
#define VID_TAG "VID_"
#define PID_TAG "PID_"
#define MAX_THREAD_WINDOW_NAME 64
#define LIBRARY_NAME ("setupapi.dll")
typedef std::basic_string<TCHAR> tstring;

// Platform-specific global variables
namespace
{
    std::atomic_bool isMonitoring{false};
    std::thread listenerThread;
    HDEVNOTIFY hDevNotify = nullptr;
    HWND hwnd = nullptr;

    HINSTANCE hinstLib;
    typedef BOOL(WINAPI *_SetupDiEnumDeviceInfo)(HDEVINFO DeviceInfoSet, DWORD MemberIndex, PSP_DEVINFO_DATA DeviceInfoData);
    typedef HDEVINFO(WINAPI *_SetupDiGetClassDevs)(const GUID *ClassGuid, PCTSTR Enumerator, HWND hwndParent, DWORD Flags);
    typedef BOOL(WINAPI *_SetupDiDestroyDeviceInfoList)(HDEVINFO DeviceInfoSet);
    typedef BOOL(WINAPI *_SetupDiGetDeviceInstanceId)(HDEVINFO DeviceInfoSet, PSP_DEVINFO_DATA DeviceInfoData, PTSTR DeviceInstanceId, DWORD DeviceInstanceIdSize, PDWORD RequiredSize);
    typedef BOOL(WINAPI *_SetupDiGetDeviceRegistryProperty)(HDEVINFO DeviceInfoSet, PSP_DEVINFO_DATA DeviceInfoData, DWORD Property, PDWORD PropertyRegDataType, PBYTE PropertyBuffer, DWORD PropertyBufferSize, PDWORD RequiredSize);
    typedef BOOL(WINAPI *_SetupDiGetDeviceProperty)(HDEVINFO DeviceInfoSet, PSP_DEVINFO_DATA DeviceInfoData, const DEVPROPKEY *PropertyKey, DEVPROPTYPE *PropertyType, PBYTE PropertyBuffer, DWORD PropertyBufferSize, PDWORD RequiredSize, DWORD Flags);

    _SetupDiEnumDeviceInfo DllSetupDiEnumDeviceInfo;
    _SetupDiGetClassDevs DllSetupDiGetClassDevs;
    _SetupDiDestroyDeviceInfoList DllSetupDiDestroyDeviceInfoList;
    _SetupDiGetDeviceInstanceId DllSetupDiGetDeviceInstanceId;
    _SetupDiGetDeviceRegistryProperty DllSetupDiGetDeviceRegistryProperty;
    _SetupDiGetDeviceProperty DllSetupDiGetDeviceProperty;

    GUID GUID_DEVINTERFACE_USB_DEVICE = {
        0xA5DCBF10L, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};

    struct WinDeviceInfo
    {
        std::string deviceId;
        ListResultItem_t deviceData;
    };
}

// Helper function declarations
static std::string WideToUTF8(const std::wstring &wstr);
static std::wstring UTF8ToWide(const std::string &str);
static void ExtractDeviceInfo(HDEVINFO hDevInfo, SP_DEVINFO_DATA *pspDevInfoData, TCHAR *buf, DWORD buffSize, ListResultItem_t *resultItem);
static void NotifyJsCallback(bool isAdded, const WinDeviceInfo &device);
static std::string Utf8Encode(const std::string &str);
static void UpdateDevice(PDEV_BROADCAST_DEVICEINTERFACE pDevInf, WPARAM wParam, DeviceState_t state);
static void ExtractVidPid(const std::string &deviceStr, ListResultItem_t *item);
static void ToUpper(char *buf);
static void NormalizeSlashes(char *buf);

// Local functions
void ToUpper(char *buf)
{
    char *c = buf;
    while (*c != '\0')
    {
        *c = toupper((unsigned char)*c);
        c++;
    }
}

void NormalizeSlashes(char *buf)
{
    char *c = buf;
    while (*c != '\0')
    {
        if (*c == '/')
        {
            *c = '\\';
        }
        c++;
    }
}

void ExtractVidPid(const std::string &deviceStr, ListResultItem_t *item)
{
    if (deviceStr.empty())
    {
        return;
    }

    std::string upperStr = deviceStr;
    std::transform(upperStr.begin(), upperStr.end(), upperStr.begin(), ::toupper);

    size_t vidPos = upperStr.find(VID_TAG);
    size_t pidPos = upperStr.find(PID_TAG);

    if (vidPos != std::string::npos)
    {
        item->vendorId = std::stoi(upperStr.substr(vidPos + strlen(VID_TAG), 4), nullptr, 16);
    }

    if (pidPos != std::string::npos)
    {
        item->productId = std::stoi(upperStr.substr(pidPos + strlen(PID_TAG), 4), nullptr, 16);
    }
}

LRESULT CALLBACK DetectCallback(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_DEVICECHANGE)
    {
        if (DBT_DEVICEARRIVAL == wParam || DBT_DEVICEREMOVECOMPLETE == wParam)
        {
            PDEV_BROADCAST_HDR pHdr = (PDEV_BROADCAST_HDR)lParam;
            PDEV_BROADCAST_DEVICEINTERFACE pDevInf;

            if (pHdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
            {
                pDevInf = (PDEV_BROADCAST_DEVICEINTERFACE)pHdr;
                UpdateDevice(pDevInf, wParam, (DBT_DEVICEARRIVAL == wParam) ? DeviceState_Connect : DeviceState_Disconnect);
            }
        }
    }

    return 1;
}

DWORD WINAPI ListenerThread()
{
    char className[MAX_THREAD_WINDOW_NAME];
    _snprintf_s(className, MAX_THREAD_WINDOW_NAME, "ListnerThreadUsbDetection_%d", GetCurrentThreadId());
    printf("Registering window class\n");
    WNDCLASSA wincl = {0};
    wincl.hInstance = GetModuleHandle(0);
    wincl.lpszClassName = className;
    wincl.lpfnWndProc = DetectCallback;

    if (!RegisterClassA(&wincl))
    {
        DWORD le = GetLastError();
        printf("RegisterClassA() failed [Error: %x]\r\n", le);
        return 1;
    }

    HWND hwnd = CreateWindowExA(WS_EX_TOPMOST, className, className, 0, 0, 0, 0, 0, NULL, 0, 0, 0);
    if (!hwnd)
    {
        DWORD le = GetLastError();
        printf("CreateWindowExA() failed [Error: %x]\r\n", le);
        return 1;
    }

    DEV_BROADCAST_DEVICEINTERFACE_A notifyFilter = {0};
    notifyFilter.dbcc_size = sizeof(notifyFilter);
    notifyFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    notifyFilter.dbcc_classguid = GUID_DEVINTERFACE_USB_DEVICE;

    HDEVNOTIFY hDevNotify = RegisterDeviceNotificationA(hwnd, &notifyFilter, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!hDevNotify)
    {
        DWORD le = GetLastError();
        printf("RegisterDeviceNotificationA() failed [Error: %x]\r\n", le);
        return 1;
    }

    MSG msg;
    while (isMonitoring)
    {
        BOOL bRet = GetMessage(&msg, hwnd, 0, 0);
        if ((bRet == 0) || (bRet == -1))
        {
            break;
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}

// Load functions from DLL
void LoadFunctions()
{
    bool success;
    hinstLib = LoadLibraryA(LIBRARY_NAME);
    if (hinstLib != NULL)
    {
        DllSetupDiEnumDeviceInfo = (_SetupDiEnumDeviceInfo)GetProcAddress(hinstLib, "SetupDiEnumDeviceInfo");
        DllSetupDiGetClassDevs = (_SetupDiGetClassDevs)GetProcAddress(hinstLib, "SetupDiGetClassDevsA");
        DllSetupDiDestroyDeviceInfoList = (_SetupDiDestroyDeviceInfoList)GetProcAddress(hinstLib, "SetupDiDestroyDeviceInfoList");
        DllSetupDiGetDeviceInstanceId = (_SetupDiGetDeviceInstanceId)GetProcAddress(hinstLib, "SetupDiGetDeviceInstanceIdA");
        DllSetupDiGetDeviceRegistryProperty = (_SetupDiGetDeviceRegistryProperty)GetProcAddress(hinstLib, "SetupDiGetDeviceRegistryPropertyA");
        DllSetupDiGetDeviceProperty = (_SetupDiGetDeviceProperty)GetProcAddress(hinstLib, "SetupDiGetDevicePropertyW");

        success = (DllSetupDiEnumDeviceInfo != NULL &&
                   DllSetupDiGetClassDevs != NULL &&
                   DllSetupDiDestroyDeviceInfoList != NULL &&
                   DllSetupDiGetDeviceInstanceId != NULL &&
                   DllSetupDiGetDeviceRegistryProperty != NULL &&
                   DllSetupDiGetDeviceProperty != NULL);
    }
    else
    {
        success = false;
    }

    if (!success)
    {
        printf("Could not load library functions from dll -> abort (Check if %s is available)\r\n", LIBRARY_NAME);
        exit(1);
    }
}

// Build initial device list
void BuildInitialDeviceList()
{
    DWORD dwFlag = (DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    GUID guid = GUID_DEVINTERFACE_USB_DEVICE;
    HDEVINFO hDevInfo = DllSetupDiGetClassDevs(&guid, NULL, NULL, dwFlag);

    if (INVALID_HANDLE_VALUE == hDevInfo)
    {
        return;
    }

    SP_DEVINFO_DATA *pspDevInfoData = (SP_DEVINFO_DATA *)HeapAlloc(GetProcessHeap(), 0, sizeof(SP_DEVINFO_DATA));
    if (pspDevInfoData)
    {
        pspDevInfoData->cbSize = sizeof(SP_DEVINFO_DATA);
        for (int i = 0; DllSetupDiEnumDeviceInfo(hDevInfo, i, pspDevInfoData); i++)
        {
            DWORD nSize = 0;
            TCHAR buf[MAX_PATH];

            if (!DllSetupDiGetDeviceInstanceId(hDevInfo, pspDevInfoData, buf, sizeof(buf), &nSize))
            {
                break;
            }
            NormalizeSlashes(buf);

            DeviceItem_t *item = new DeviceItem_t();
            item->deviceState = DeviceState_Connect;

            DWORD DataT;
            DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_LOCATION_INFORMATION, &DataT, (PBYTE)buf, MAX_PATH, &nSize);
            DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_HARDWAREID, &DataT, (PBYTE)(buf + nSize - 1), MAX_PATH - nSize, &nSize);

            AddItemToList(buf, item);
            ExtractDeviceInfo(hDevInfo, pspDevInfoData, buf, MAX_PATH, &item->deviceParams);
        }

        HeapFree(GetProcessHeap(), 0, pspDevInfoData);
    }

    if (hDevInfo)
    {
        DllSetupDiDestroyDeviceInfoList(hDevInfo);
    }
}

void UpdateDevice(PDEV_BROADCAST_DEVICEINTERFACE pDevInf, WPARAM wParam, DeviceState_t state)
{
    // dbcc_name:
    // \\?\USB#Vid_04e8&Pid_503b#0002F9A9828E0F06#{a5dcbf10-6530-11d2-901f-00c04fb951ed}
    // convert to
    // USB\Vid_04e8&Pid_503b\0002F9A9828E0F06
    tstring szDevId = pDevInf->dbcc_name + 4;
    auto idx = szDevId.rfind(_T('#'));

    if (idx != tstring::npos)
        szDevId.resize(idx);
    std::replace(begin(szDevId), end(szDevId), _T('#'), _T('\\'));
    auto to_upper = [](TCHAR ch)
    { return std::use_facet<std::ctype<TCHAR>>(std::locale()).toupper(ch); };
    transform(begin(szDevId), end(szDevId), begin(szDevId), to_upper);

    tstring szClass;
    idx = szDevId.find(_T('\\'));
    if (idx != tstring::npos)
        szClass = szDevId.substr(0, idx);
    // if we are adding device, we only need present devices
    // otherwise, we need all devices
    DWORD dwFlag = DBT_DEVICEARRIVAL != wParam ? DIGCF_ALLCLASSES : (DIGCF_ALLCLASSES | DIGCF_PRESENT);
    HDEVINFO hDevInfo = DllSetupDiGetClassDevs(NULL, szClass.c_str(), NULL, dwFlag);
    if (INVALID_HANDLE_VALUE == hDevInfo)
    {
        return;
    }

    SP_DEVINFO_DATA *pspDevInfoData = (SP_DEVINFO_DATA *)HeapAlloc(GetProcessHeap(), 0, sizeof(SP_DEVINFO_DATA));
    if (pspDevInfoData)
    {
        pspDevInfoData->cbSize = sizeof(SP_DEVINFO_DATA);
        for (int i = 0; DllSetupDiEnumDeviceInfo(hDevInfo, i, pspDevInfoData); i++)
        {
            DWORD nSize = 0;
            TCHAR buf[MAX_PATH];

            if (!DllSetupDiGetDeviceInstanceId(hDevInfo, pspDevInfoData, buf, sizeof(buf), &nSize))
            {
                break;
            }

            NormalizeSlashes(buf);

            if (szDevId == buf)
            {
                DWORD DataT;
                DWORD nSize;
                DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_LOCATION_INFORMATION, &DataT, (PBYTE)buf, MAX_PATH, &nSize);
                DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_HARDWAREID, &DataT, (PBYTE)(buf + nSize - 1), MAX_PATH - nSize, &nSize);

                WinDeviceInfo deviceInfoChange;
                deviceInfoChange.deviceId = buf;
                printf("Device change: %s\n", buf);

                if (state == DeviceState_Connect)
                {
                    DeviceItem_t *device = new DeviceItem_t();

                    AddItemToList(buf, device);
                    ExtractDeviceInfo(hDevInfo, pspDevInfoData, buf, MAX_PATH, &device->deviceParams);

                    deviceInfoChange.deviceData = device->deviceParams;
                }
                else
                {

                    ListResultItem_t *item = NULL;
                    if (IsItemAlreadyStored(buf))
                    {
                        DeviceItem_t *deviceItem = GetItemFromList(buf);
                        if (deviceItem)
                        {
                            item = CopyElement(&deviceItem->deviceParams);
                        }
                        RemoveItemFromList(deviceItem);
                        delete deviceItem;
                    }

                    if (item == NULL)
                    {
                        item = new ListResultItem_t();
                        ExtractDeviceInfo(hDevInfo, pspDevInfoData, buf, MAX_PATH, item);
                    }

                    deviceInfoChange.deviceData = *item;
                }

                NotifyJsCallback(wParam == DBT_DEVICEARRIVAL, deviceInfoChange);
                break;
            }
        }

        HeapFree(GetProcessHeap(), 0, pspDevInfoData);
    }

    if (hDevInfo)
    {
        DllSetupDiDestroyDeviceInfoList(hDevInfo);
    }
}

// Platform initialization
void PlatformInit()
{
    LoadFunctions();
    BuildInitialDeviceList();
}

// Find operation
void EIO_Find(napi_env env, void *data)
{
    ListBaton *baton = static_cast<ListBaton *>(data);

    try
    {
        CreateFilteredList(&baton->results, baton->vid, baton->pid);
    }
    catch (const std::exception &e)
    {
        strncpy(baton->errorString, e.what(), sizeof(baton->errorString) - 1);
    }
}

// Start monitoring
void Start()
{
    if (isMonitoring.exchange(true))
        return;

    printf("Starting monitoring\n");
    listenerThread = std::thread([]
                                 {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        printf("Starting listener thread\n");
        ListenerThread();
        // ListenerThreadMain();
        printf("Listener thread stopped\n");
        CoUninitialize(); });
}

// Stop monitoring
void Stop()
{
    if (!isMonitoring.exchange(false))
        return;

    if (hDevNotify)
    {
        UnregisterDeviceNotification(hDevNotify);
        hDevNotify = nullptr;
    }

    if (hwnd)
    {
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }

    if (listenerThread.joinable())
    {
        PostThreadMessage(GetThreadId(listenerThread.native_handle()), WM_QUIT, 0, 0);
        listenerThread.join();
    }
}


static void NotifyJsCallback(bool isAdded, const WinDeviceInfo &device)
{
    auto tsFunc = isAdded ? addedTsFunc : removedTsFunc;
    if (!tsFunc)
        return;

    auto deviceData = std::make_shared<ListResultItem_t>(device.deviceData);

    tsFunc.BlockingCall(deviceData.get(), [deviceData](Napi::Env env, Napi::Function callback, ListResultItem_t *data)
                        {
        Napi::Object obj = Napi::Object::New(env);
        obj.Set("vendorId", data->vendorId);
        obj.Set("productId", data->productId);
        obj.Set("serialNumber", data->serialNumber);
        obj.Set("deviceName", data->deviceName);
        obj.Set("manufacturer", data->manufacturer);
        callback.Call({ obj }); });
}

std::string TrimNullTerminator(const std::string &str)
{
    size_t end = str.find('\0');
    if (end != std::string::npos)
    {
        return str.substr(0, end);
    }
    return str;
}

std::string Utf8Encode(const std::string &str)
{
    if (str.empty())
    {
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

void ExtractDeviceInfo(HDEVINFO hDevInfo, SP_DEVINFO_DATA *pspDevInfoData, TCHAR *buf, DWORD buffSize, ListResultItem_t *resultItem)
{
    DWORD DataT;
    DWORD nSize;
    static int dummy = 1;

    resultItem->locationId = 0;
    resultItem->deviceAddress = dummy++;

    // Device found
    if (DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_FRIENDLYNAME, &DataT, (PBYTE)buf, buffSize, &nSize))
    {
        resultItem->deviceName = Utf8Encode(buf);
    }
    else if (DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_DEVICEDESC, &DataT, (PBYTE)buf, buffSize, &nSize))
    {
        resultItem->deviceName = Utf8Encode(buf);
    }
    if (DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_MFG, &DataT, (PBYTE)buf, buffSize, &nSize))
    {
        resultItem->manufacturer = Utf8Encode(buf);
    }
    if (DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_HARDWAREID, &DataT, (PBYTE)buf, buffSize, &nSize))
    {
        // Use this to extract VID / PID
        ExtractVidPid(buf, resultItem);
    }

    // remove '\x00' terminator
    resultItem->deviceName = TrimNullTerminator(resultItem->deviceName);
    resultItem->manufacturer = TrimNullTerminator(resultItem->manufacturer);

    // Extract Serial Number
    DWORD dwCapabilities = 0x0;
    if (DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_CAPABILITIES, &DataT, (PBYTE)&dwCapabilities, sizeof(dwCapabilities), &nSize))
    {
        if ((dwCapabilities & CM_DEVCAP_UNIQUEID) == CM_DEVCAP_UNIQUEID)
        {
            if (DllSetupDiGetDeviceInstanceId(hDevInfo, pspDevInfoData, buf, buffSize, &nSize))
            {
                std::string deviceInstanceId = buf;
                size_t serialNumberIndex = deviceInstanceId.find_last_of("\\");
                if (serialNumberIndex != std::string::npos)
                {
                    resultItem->serialNumber = deviceInstanceId.substr(serialNumberIndex + 1);
                }
            }
        }
    }
}
