/* KitKat vendor OMX plugin (libstagefrighthw.so): exposes the Venus hardware codecs through the
 * Android 11 Qualcomm OMX core (libOmxCore + libOmxVdec/Venc), loaded into mediaserver by
 * libhyb. The OMX IL structures and the Android extensions KitKat uses (useAndroidNativeBuffer,
 * storeMetaDataInBuffers...) have the same layout in both versions: components are passed
 * through as they are. */
#define LOG_TAG "QcomOMXPlugin"
#include <dlfcn.h>
#include <utils/Log.h>
#include <utils/String8.h>
#include <utils/Vector.h>
#include <media/hardware/OMXPluginBase.h>
#include "../hyb/hyb.h"

namespace android {

typedef OMX_ERRORTYPE (*InitFunc)();
typedef OMX_ERRORTYPE (*DeinitFunc)();
typedef OMX_ERRORTYPE (*ComponentNameEnumFunc)(OMX_STRING, OMX_U32, OMX_U32);
typedef OMX_ERRORTYPE (*GetHandleFunc)(OMX_HANDLETYPE *, OMX_STRING, OMX_PTR, const OMX_CALLBACKTYPE *);
typedef OMX_ERRORTYPE (*FreeHandleFunc)(OMX_HANDLETYPE *);
typedef OMX_ERRORTYPE (*GetRolesOfComponentFunc)(OMX_STRING, OMX_U32 *, OMX_U8 **);

struct QcomOMXPlugin : public OMXPluginBase {
    QcomOMXPlugin();
    virtual ~QcomOMXPlugin();
    virtual OMX_ERRORTYPE makeComponentInstance(const char *name, const OMX_CALLBACKTYPE *callbacks,
                                                OMX_PTR appData, OMX_COMPONENTTYPE **component);
    virtual OMX_ERRORTYPE destroyComponentInstance(OMX_COMPONENTTYPE *component);
    virtual OMX_ERRORTYPE enumerateComponents(OMX_STRING name, size_t size, OMX_U32 index);
    virtual OMX_ERRORTYPE getRolesOfComponent(const char *name, Vector<String8> *roles);

private:
    void *mCore;
    InitFunc mInit;
    DeinitFunc mDeinit;
    ComponentNameEnumFunc mComponentNameEnum;
    GetHandleFunc mGetHandle;
    FreeHandleFunc mFreeHandle;
    GetRolesOfComponentFunc mGetRolesOfComponent;
};

extern "C" OMXPluginBase *createOMXPlugin() { return new QcomOMXPlugin; }

QcomOMXPlugin::QcomOMXPlugin()
    : mCore(NULL), mInit(NULL), mDeinit(NULL), mComponentNameEnum(NULL), mGetHandle(NULL),
      mFreeHandle(NULL), mGetRolesOfComponent(NULL)
{
    hyb_set_root("/system/a11");
    mCore = hyb_dlopen("libOmxCore.so", 0);
    if (!mCore) {
        ALOGE("Android 11 libOmxCore: %s", hyb_dlerror());
        return;
    }
    mInit = (InitFunc)hyb_dlsym(mCore, "OMX_Init");
    mDeinit = (DeinitFunc)hyb_dlsym(mCore, "OMX_Deinit");
    mComponentNameEnum = (ComponentNameEnumFunc)hyb_dlsym(mCore, "OMX_ComponentNameEnum");
    mGetHandle = (GetHandleFunc)hyb_dlsym(mCore, "OMX_GetHandle");
    mFreeHandle = (FreeHandleFunc)hyb_dlsym(mCore, "OMX_FreeHandle");
    mGetRolesOfComponent = (GetRolesOfComponentFunc)hyb_dlsym(mCore, "OMX_GetRolesOfComponent");
    if (mInit)
        (*mInit)();
    ALOGI("Android 11 Qualcomm OMX core ready");
}

QcomOMXPlugin::~QcomOMXPlugin()
{
    if (mDeinit)
        (*mDeinit)();
}

OMX_ERRORTYPE QcomOMXPlugin::makeComponentInstance(const char *name, const OMX_CALLBACKTYPE *callbacks,
                                                   OMX_PTR appData, OMX_COMPONENTTYPE **component)
{
    if (!mGetHandle)
        return OMX_ErrorUndefined;
    OMX_ERRORTYPE err = (*mGetHandle)(reinterpret_cast<OMX_HANDLETYPE *>(component),
                                      const_cast<char *>(name), appData, callbacks);
    if (err != OMX_ErrorNone)
        ALOGE("%s: OMX_GetHandle = %#x", name, err);
    return err;
}

OMX_ERRORTYPE QcomOMXPlugin::destroyComponentInstance(OMX_COMPONENTTYPE *component)
{
    if (!mFreeHandle)
        return OMX_ErrorUndefined;
    return (*mFreeHandle)(reinterpret_cast<OMX_HANDLETYPE *>(component));
}

OMX_ERRORTYPE QcomOMXPlugin::enumerateComponents(OMX_STRING name, size_t size, OMX_U32 index)
{
    if (!mComponentNameEnum)
        return OMX_ErrorUndefined;
    return (*mComponentNameEnum)(name, size, index);
}

OMX_ERRORTYPE QcomOMXPlugin::getRolesOfComponent(const char *name, Vector<String8> *roles)
{
    roles->clear();
    if (!mGetRolesOfComponent)
        return OMX_ErrorUndefined;
    OMX_U32 numRoles;
    OMX_ERRORTYPE err = (*mGetRolesOfComponent)(const_cast<OMX_STRING>(name), &numRoles, NULL);
    if (err != OMX_ErrorNone || numRoles == 0)
        return err;
    OMX_U8 **array = new OMX_U8 *[numRoles];
    for (size_t i = 0; i < numRoles; ++i)
        array[i] = new OMX_U8[OMX_MAX_STRINGNAME_SIZE];
    OMX_U32 numRoles2 = numRoles;
    err = (*mGetRolesOfComponent)(const_cast<OMX_STRING>(name), &numRoles2, array);
    for (size_t i = 0; i < numRoles; ++i) {
        if (err == OMX_ErrorNone && i < numRoles2)
            roles->push(String8((const char *)array[i]));
        delete[] array[i];
    }
    delete[] array;
    return err;
}

}  // namespace android
