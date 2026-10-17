// HIDL services in the chroot. There is no hwservicemanager: libhidl would wait forever
// (hwservicemanager.ready) when the NXP libraries look for the secure element (INxpEse) while
// the NFC HAL opens. The executable exports (-rdynamic) its own getRawServiceInternal, which
// takes precedence over libhidlbase's: the service is missing, right away.
// sp<IBase> has a non-trivial destructor: returned through the address passed in x8, as here.
struct SpNull {
    void *ptr;
    ~SpNull() {}
};

SpNull hidl_no_service(const void *descriptor, const void *instance, bool retry, bool getStub) __asm__(
        "_ZN7android8hardware7details21getRawServiceInternalERKNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_bb");

__attribute__((visibility("default"))) SpNull hidl_no_service(const void *, const void *, bool, bool)
{
    SpNull r;
    r.ptr = nullptr;
    return r;
}
