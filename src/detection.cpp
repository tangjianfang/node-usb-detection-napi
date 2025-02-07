#include "detection.h"
#define OBJECT_ITEM_LOCATION_ID "locationId"
#define OBJECT_ITEM_VENDOR_ID "vendorId"
#define OBJECT_ITEM_PRODUCT_ID "productId"
#define OBJECT_ITEM_DEVICE_NAME "deviceName"
#define OBJECT_ITEM_MANUFACTURER "manufacturer"
#define OBJECT_ITEM_SERIAL_NUMBER "serialNumber"
#define OBJECT_ITEM_DEVICE_ADDRESS "deviceAddress"


static Napi::ThreadSafeFunction addedTsFunc;
static Napi::ThreadSafeFunction removedTsFunc;

// Notify JS callback for added device
void NotifyAdded(ListResultItem_t* it) {
    if (!it || !addedTsFunc) return;

    addedTsFunc.BlockingCall(it, [](Napi::Env env, Napi::Function jsCallback, ListResultItem_t* it) {
        if (!it) return;

        Napi::Object item = Napi::Object::New(env);
        item.Set(OBJECT_ITEM_LOCATION_ID, it->locationId);
        item.Set(OBJECT_ITEM_VENDOR_ID, it->vendorId);
        item.Set(OBJECT_ITEM_PRODUCT_ID, it->productId);
        item.Set(OBJECT_ITEM_DEVICE_NAME, Napi::String::New(env, it->deviceName));
        item.Set(OBJECT_ITEM_MANUFACTURER, Napi::String::New(env, it->manufacturer));
        item.Set(OBJECT_ITEM_SERIAL_NUMBER, Napi::String::New(env, it->serialNumber));
        item.Set(OBJECT_ITEM_DEVICE_ADDRESS, it->deviceAddress);

        jsCallback.Call({ item });
    });

    addedTsFunc.Release();
}

// Notify JS callback for removed device
void NotifyRemoved(ListResultItem_t* it) {
    if (!it || !removedTsFunc) return; // Check if 'it' or 'removedTsFunc' is null

    // Using N-API's ThreadSafeFunction
    removedTsFunc.BlockingCall(it, [](Napi::Env env, Napi::Function jsCallback, ListResultItem_t* it) {
        // Create a new Napi::Object to store item data
        Napi::Object item = Napi::Object::New(env);

        // Set properties of the item object
        item.Set(OBJECT_ITEM_LOCATION_ID, it->locationId);
        item.Set(OBJECT_ITEM_VENDOR_ID, it->vendorId);
        item.Set(OBJECT_ITEM_PRODUCT_ID, it->productId);
        item.Set(OBJECT_ITEM_DEVICE_NAME, Napi::String::New(env, it->deviceName));
        item.Set(OBJECT_ITEM_MANUFACTURER, Napi::String::New(env, it->manufacturer));
        item.Set(OBJECT_ITEM_SERIAL_NUMBER, Napi::String::New(env, it->serialNumber));
        item.Set(OBJECT_ITEM_DEVICE_ADDRESS, it->deviceAddress);

        jsCallback.Call({item});

        delete it;
    });

    removedTsFunc.Release();
}

void Find(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    ListBaton* baton = new ListBaton(env);

    size_t argIndex = 0;
    if (info.Length() > argIndex && info[argIndex].IsNumber()) {
        baton->vid = info[argIndex++].As<Napi::Number>().Int32Value();
    }
    if (info.Length() > argIndex && info[argIndex].IsNumber()) {
        baton->pid = info[argIndex++].As<Napi::Number>().Int32Value();
    }
    if (info.Length() > argIndex && info[argIndex].IsFunction()) {
        baton->callback.Reset(info[argIndex].As<Napi::Function>(), 1);
    }

    napi_value resource_name;
    napi_create_string_utf8(env, "USBDetection:Find", NAPI_AUTO_LENGTH, &resource_name);

    napi_async_work work;
    napi_create_async_work(
        env,
        nullptr,
        resource_name,
        EIO_Find,
        EIO_AfterFind,
        baton,
        &work
    );

    napi_queue_async_work(env, work);
}

// After find operation
void EIO_AfterFind(napi_env env, napi_status status, void* data) {
    ListBaton* baton = static_cast<ListBaton*>(data);
    Napi::HandleScope scope(env);
    Napi::Env napiEnv = Napi::Env(env);  // 将 napi_env 转换为 Napi::Env

    if (baton->errorString[0]) {
        Napi::Error error = Napi::Error::New(napiEnv, baton->errorString);
        if (baton->callback.IsEmpty()) {
            baton->deferred.Reject(error.Value());
        } else {
            baton->callback.Call({error.Value()});
        }
    } else {
        Napi::Array result = Napi::Array::New(napiEnv, baton->results.size());
        int i = 0;
        for (auto& item : baton->results) {
            Napi::Object obj = Napi::Object::New(napiEnv);
            obj.Set(OBJECT_ITEM_VENDOR_ID, item->vendorId);
            obj.Set(OBJECT_ITEM_PRODUCT_ID, item->productId);
            obj.Set(OBJECT_ITEM_SERIAL_NUMBER, item->serialNumber);
            obj.Set(OBJECT_ITEM_DEVICE_NAME, item->deviceName);
            obj.Set(OBJECT_ITEM_MANUFACTURER, item->manufacturer);
            result[i++] = obj;
            delete item;
        }

        if (baton->callback.IsEmpty()) {
            baton->deferred.Resolve(result);
        } else {
            baton->callback.Call({napiEnv.Null(), result});
        }
    }

    delete baton;
}

void StartMonitoring(const Napi::CallbackInfo& args) {
    PlatformStartMonitoring();
}

void StopMonitoring(const Napi::CallbackInfo& args) {
    PlatformStopMonitoring();
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("find", Napi::Function::New(env, Find));
    exports.Set("registerAdded", Napi::Function::New(env, RegisterAdded));
    exports.Set("registerRemoved", Napi::Function::New(env, RegisterRemoved));
    exports.Set("startMonitoring", Napi::Function::New(env, StartMonitoring));
    exports.Set("stopMonitoring", Napi::Function::New(env, StopMonitoring));

	PlatformInit();
    return exports;
}

NODE_API_MODULE(detection, Init)
