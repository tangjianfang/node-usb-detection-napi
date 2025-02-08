
#include "detection.h"
#include "deviceList.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#include <sys/param.h>
#include <napi.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

/**********************************
 * Local typedefs
 **********************************/
typedef struct DeviceListItem {
    io_object_t notification;
    IOUSBDeviceInterface** deviceInterface;
    DeviceItem_t* deviceItem;
} stDeviceListItem;

/**********************************
 * Local Variables
 **********************************/
static std::atomic<bool> gInitialDeviceImport{true};
static IONotificationPortRef gNotifyPort;
static io_iterator_t gAddedIter;
static CFRunLoopRef gRunLoop;
static CFMutableDictionaryRef gMatchingDict;
static CFRunLoopSourceRef gRunLoopSource;
static std::atomic<bool> gIsRunning{false};

/**********************************
 * Local Helper Functions prototypes
 **********************************/
static void DeviceRemoved(void *refCon, io_service_t service, natural_t messageType, void *messageArgument);
static void DeviceAdded(void *refCon, io_iterator_t iterator);
static void RunLoopThread();

/**********************************
 * Public Functions
 **********************************/

//================================================================================================
//
//  DeviceRemoved
//
//  This routine will get called whenever any kIOGeneralInterest notification happens.  We are
//  interested in the kIOMessageServiceIsTerminated message so that's what we look for.  Other
//  messages are defined in IOMessage.h.
//
//================================================================================================
static void DeviceRemoved(void *refCon, io_service_t service, natural_t messageType, void *messageArgument) {
    stDeviceListItem* deviceListItem = (stDeviceListItem *) refCon;
    DeviceItem_t* deviceItem = deviceListItem->deviceItem;
    if(messageType == kIOMessageServiceIsTerminated) {
        if(deviceListItem->deviceInterface) {
            (*deviceListItem->deviceInterface)->Release(deviceListItem->deviceInterface);
        }
        printf("[DEBUG]  DeviceRemoved kIOMessageServiceIsTerminated %x  %s\n", messageType, deviceItem->GetKey());
        IOObjectRelease(deviceListItem->notification);

        ListResultItem_t* item = nullptr;
        if(deviceItem) {
            item = CopyElement(&deviceItem->deviceParams);
            RemoveItemFromList(deviceItem);
            delete deviceItem;
        } else {
            item = new ListResultItem_t();
        }

        removedTsFunc.NonBlockingCall(item, [](Napi::Env env, Napi::Function callback, ListResultItem_t* data) {
            Napi::Object obj = Napi::Object::New(env);
            obj.Set("vendorId", data->vendorId);
            obj.Set("productId", data->productId);
            obj.Set("serialNumber", data->serialNumber);
            obj.Set("deviceName", data->deviceName);
            obj.Set("manufacturer", data->manufacturer);
            callback.Call({obj});
            delete data;
        });
    }
}

//================================================================================================
//
//  DeviceAdded
//
//  This routine is the callback for our IOServiceAddMatchingNotification.  When we get called
//  we will look at all the devices that were added and we will:
//
//  1.  Create some private data to relate to each device (in this case we use the service's name
//      and the location ID of the device
//  2.  Submit an IOServiceAddInterestNotification of type kIOGeneralInterest for this device,
//      using the refCon field to store a pointer to our private data.  When we get called with
//      this interest notification, we can grab the refCon and access our private data.
//
//================================================================================================
static void DeviceAdded(void *refCon, io_iterator_t iterator) {
    kern_return_t kr;
    io_service_t usbDevice;
    IOCFPlugInInterface **plugInInterface = nullptr;
    SInt32 score;
    HRESULT res;
    printf("[DEBUG]  DeviceAdded\n");
    while((usbDevice = IOIteratorNext(iterator))) {
        io_name_t deviceName;
        CFStringRef deviceNameAsCFString;
        UInt32 locationID;
        UInt16 vendorId;
        UInt16 productId;
        UInt16 addr;

        DeviceItem_t* deviceItem = new DeviceItem_t();

        kr = IORegistryEntryGetName(usbDevice, deviceName);
        if(KERN_SUCCESS != kr) {
            deviceName[0] = '\0';
        }

        deviceNameAsCFString = CFStringCreateWithCString(kCFAllocatorDefault, deviceName, kCFStringEncodingASCII);

        if(deviceNameAsCFString) {
            char deviceNameStr[MAXPATHLEN];
            if(CFStringGetCString(deviceNameAsCFString, deviceNameStr, sizeof(deviceNameStr), kCFStringEncodingUTF8)) {
                deviceItem->deviceParams.deviceName = deviceNameStr;
            }
            CFRelease(deviceNameAsCFString);
        }

        CFStringRef manufacturerAsCFString = (CFStringRef)IORegistryEntrySearchCFProperty(
            usbDevice,
            kIOServicePlane,
            CFSTR(kUSBVendorString),
            kCFAllocatorDefault,
            kIORegistryIterateRecursively
        );

        if(manufacturerAsCFString) {
            char manufacturer[MAXPATHLEN];
            if(CFStringGetCString(manufacturerAsCFString, manufacturer, sizeof(manufacturer), kCFStringEncodingUTF8)) {
                deviceItem->deviceParams.manufacturer = manufacturer;
            }
            CFRelease(manufacturerAsCFString);
        }

        CFStringRef serialNumberAsCFString = (CFStringRef) IORegistryEntrySearchCFProperty(
            usbDevice,
            kIOServicePlane,
            CFSTR(kUSBSerialNumberString),
            kCFAllocatorDefault,
            kIORegistryIterateRecursively
        );

        if(serialNumberAsCFString) {
            char serialNumber[MAXPATHLEN];
            if(CFStringGetCString(serialNumberAsCFString, serialNumber, sizeof(serialNumber), kCFStringEncodingUTF8)) {
                deviceItem->deviceParams.serialNumber = serialNumber;
            }
            CFRelease(serialNumberAsCFString);
        }

        // Now, get the locationID of this device. In order to do this, we need to create an IOUSBDeviceInterface
        // for our device. This will create the necessary connections between our userland application and the
        // kernel object for the USB Device.
        kr = IOCreatePlugInInterfaceForService(usbDevice, 
            kIOUSBDeviceUserClientTypeID,
            kIOCFPlugInInterfaceID,
            &plugInInterface,
            &score
        );

        if(kr != kIOReturnSuccess || !plugInInterface) {
            IOObjectRelease(usbDevice);
            continue;
        }

        stDeviceListItem *deviceListItem = new stDeviceListItem();
        res = (*plugInInterface)->QueryInterface(
            plugInInterface,
            CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
            (LPVOID*) &deviceListItem->deviceInterface
        );
        (*plugInInterface)->Release(plugInInterface);

        if(res != S_OK || !deviceListItem->deviceInterface) {
            delete deviceListItem;
            IOObjectRelease(usbDevice);
            continue;
        }

        // Now that we have the IOUSBDeviceInterface, we can call the routines in IOUSBLib.h.
        // In this case, fetch the locationID. The locationID uniquely identifies the device
        // and will remain the same, even across reboots, so long as the bus topology doesn't change.
        kr = (*deviceListItem->deviceInterface)->GetLocationID(deviceListItem->deviceInterface, &locationID);
        if(kr != KERN_SUCCESS) {
            (*deviceListItem->deviceInterface)->Release(deviceListItem->deviceInterface);
            delete deviceListItem;
            IOObjectRelease(usbDevice);
            continue;
        }
        deviceItem->deviceParams.locationId = locationID;

        kr = (*deviceListItem->deviceInterface)->GetDeviceAddress(deviceListItem->deviceInterface, &addr);
        if(kr != KERN_SUCCESS) {
            (*deviceListItem->deviceInterface)->Release(deviceListItem->deviceInterface);
            delete deviceListItem;
            IOObjectRelease(usbDevice);
            continue;
        }
        deviceItem->deviceParams.deviceAddress = addr;

        kr = (*deviceListItem->deviceInterface)->GetDeviceVendor(deviceListItem->deviceInterface, &vendorId);
        if(kr != KERN_SUCCESS) {
            (*deviceListItem->deviceInterface)->Release(deviceListItem->deviceInterface);
            delete deviceListItem;
            IOObjectRelease(usbDevice);
            continue;
        }
        deviceItem->deviceParams.vendorId = vendorId;

        kr = (*deviceListItem->deviceInterface)->GetDeviceProduct(deviceListItem->deviceInterface, &productId);
        if(kr != KERN_SUCCESS) {
            (*deviceListItem->deviceInterface)->Release(deviceListItem->deviceInterface);
            delete deviceListItem;
            IOObjectRelease(usbDevice);
            continue;
        }
        deviceItem->deviceParams.productId = productId;

        io_string_t pathName;
        IORegistryEntryGetPath(usbDevice, kIOServicePlane, pathName);
        CFStringRef pathCFString = CFStringCreateWithCString(kCFAllocatorDefault, pathName, kCFStringEncodingASCII);
        char cPathName[MAXPATHLEN];
        if(pathCFString) {
            CFStringGetCString(pathCFString, cPathName, sizeof(cPathName), kCFStringEncodingUTF8);
            CFRelease(pathCFString);
        }

        AddItemToList(cPathName, deviceItem);
        deviceListItem->deviceItem = deviceItem;

        if(!gInitialDeviceImport.load()) {
            addedTsFunc.NonBlockingCall( &deviceItem->deviceParams, [](Napi::Env env, Napi::Function callback, ListResultItem_t* data) {
                Napi::Object obj = Napi::Object::New(env);
                obj.Set("vendorId", data->vendorId);
                obj.Set("productId", data->productId);
                obj.Set("serialNumber", data->serialNumber);
                obj.Set("deviceName", data->deviceName);
                obj.Set("manufacturer", data->manufacturer);
                callback.Call({obj});
            });
        }

        // Register for an interest notification of this device being removed. Use a reference to our
        // private data as the refCon which will be passed to the notification callback.
        kr = IOServiceAddInterestNotification(
            gNotifyPort,
            usbDevice,
            kIOGeneralInterest,
            DeviceRemoved,
            deviceListItem,
            &deviceListItem->notification
        );
        if(kr != KERN_SUCCESS) {
            printf("[DEBUG]  IOServiceAddInterestNotification kr != KERN_SUCCESS\n");
        }
        IOObjectRelease(usbDevice);
    }
}

void Start() {
    if(gIsRunning.exchange(true)) return;
    std::thread(RunLoopThread).detach();
}

void Stop() {
    if(!gIsRunning.exchange(false)) return;
    if(gRunLoop) {
        CFRunLoopPerformBlock(gRunLoop, kCFRunLoopCommonModes, ^{
            CFRunLoopStop(gRunLoop);
        });
        CFRunLoopWakeUp(gRunLoop);
    }
}

void InitDetection() {
    gMatchingDict = IOServiceMatching(kIOUSBDeviceClassName);
    if(!gMatchingDict) return;

    gNotifyPort = IONotificationPortCreate(kIOMainPortDefault);
    if(!gNotifyPort) {
        CFRelease(gMatchingDict);
        return;
    }

    // Create a notification port and add its run loop event source to our run loop
    // Now set up a notification to be called when a device is first matched by I/O Kit.
    kern_return_t kr = IOServiceAddMatchingNotification(
        gNotifyPort,
        kIOFirstMatchNotification,
        gMatchingDict,
        DeviceAdded,
        nullptr,
        &gAddedIter
    );

    if(kr != KERN_SUCCESS) {
        IONotificationPortDestroy(gNotifyPort);
        return;
    }

    DeviceAdded(nullptr, gAddedIter);
    gInitialDeviceImport.store(false);
}

void EIO_Find(napi_env env, void* data) {
    ListBaton* baton = static_cast<ListBaton*>(data);
    try {
        CreateFilteredList(&baton->results, baton->vid, baton->pid);
    } catch (const std::exception &e) {
        strncpy(baton->errorString, e.what(), sizeof(baton->errorString)-1);
    }
}

static void RunLoopThread() {
    gRunLoopSource = IONotificationPortGetRunLoopSource(gNotifyPort);
    gRunLoop = CFRunLoopGetCurrent();
    CFRunLoopAddSource(gRunLoop, gRunLoopSource, kCFRunLoopDefaultMode);

    while(gIsRunning.load()) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
    }

    // Clean up
    CFRunLoopRemoveSource(gRunLoop, gRunLoopSource, kCFRunLoopDefaultMode);
    CFRelease(gRunLoopSource);
    IOObjectRelease(gAddedIter);
    IONotificationPortDestroy(gNotifyPort);
}