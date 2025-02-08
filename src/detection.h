#ifndef _USB_DETECTION_H
#define _USB_DETECTION_H

#include <napi.h>
#include <list>
#include <string>
#include "deviceList.h"

// Function declarations
void Find(const Napi::CallbackInfo& info);
void EIO_Find(napi_env env, void* data);
void EIO_AfterFind(napi_env env, napi_status status, void* data);
void InitDetection();
void StartMonitoring(const Napi::CallbackInfo& info);
void StopMonitoring(const Napi::CallbackInfo& info);
void Start();
void Stop();

// ListBaton struct for passing data in asynchronous operations
struct ListBaton {
    Napi::FunctionReference callback;
    std::list<ListResultItem_t*> results;
    char errorString[1024];
    int vid;
    int pid;

    Napi::Env env;
    Napi::Promise::Deferred deferred;

    ListBaton(Napi::Env env) : vid(0), pid(0), env(env), deferred(Napi::Promise::Deferred::New(env)) {
        errorString[0] = '\0';
    }
};

void RegisterAdded(const Napi::CallbackInfo& info);
void NotifyAdded(ListResultItem_t* it);
void RegisterRemoved(const Napi::CallbackInfo& info);
void NotifyRemoved(ListResultItem_t* it);

// Thread-safe callbacks
extern Napi::ThreadSafeFunction addedTsFunc;
extern Napi::ThreadSafeFunction removedTsFunc;

#endif

#ifdef DEBUG
  #define DEBUG_HEADER fprintf(stderr, "node-usb-detection [%s:%s() %d]: ", __FILE__, __FUNCTION__, __LINE__);
  #define DEBUG_FOOTER fprintf(stderr, "\n");
  #define DEBUG_LOG(...) DEBUG_HEADER fprintf(stderr, __VA_ARGS__); DEBUG_FOOTER
#else
  #define DEBUG_LOG(...)
#endif