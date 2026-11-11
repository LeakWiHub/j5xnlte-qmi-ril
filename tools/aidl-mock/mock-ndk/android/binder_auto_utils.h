// Minimal type-checking stand-in for NDK <android/binder_auto_utils.h>. NOT for the device build.
#pragma once
#include <cstdint>
#include <memory>
struct AIBinder;
namespace ndk {
class ScopedAStatus {
  public:
    static ScopedAStatus ok() { return {}; }
    static ScopedAStatus fromServiceSpecificError(int32_t) { return {}; }
    static ScopedAStatus fromExceptionCode(int32_t) { return {}; }
    bool isOk() const { return true; }
};
class SpAIBinder {
  public:
    AIBinder* get() const { return nullptr; }
};
class ICInterface {
  public:
    virtual ~ICInterface() = default;
    virtual SpAIBinder asBinder() = 0;
};
class SharedRefBase : public std::enable_shared_from_this<SharedRefBase> {
  public:
    virtual ~SharedRefBase() = default;
    template <typename T, typename... A>
    static std::shared_ptr<T> make(A&&... a) { return std::make_shared<T>(std::forward<A>(a)...); }
};
template <typename I>
class BnCInterface : public SharedRefBase, public I {
  public:
    SpAIBinder asBinder() override { return {}; }
};
}  // namespace ndk
