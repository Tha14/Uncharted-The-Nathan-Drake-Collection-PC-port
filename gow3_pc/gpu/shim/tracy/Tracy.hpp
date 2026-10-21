// gow3: profiler disabled; Tracy macros compile to nothing.
#pragma once
#include <cstddef>
#include <cstdint>
#define TracyFile __FILE__
#define TracyLine __LINE__
#define TracyFunction __func__
#define ZoneScoped
#define ZoneScopedN(name)
#define ZoneScopedC(color)
#define ZoneScopedNC(name, color)
// gow3: the arguments are not evaluated (a formatted string would still be built otherwise).
#define ZoneText(text, size)
#define ZoneName(text, size)
#define FrameMark
#define FrameMarkNamed(name)
#define TracyMessageC(text, size, color)
#define TracyMessageL(text)
#define TracyAllocN(ptr, size, name)
#define TracyFreeN(ptr, name)
#define TracyFiberEnter(name)
#define TracyFiberLeave
#define TracyPlot(name, value)
namespace tracy {
struct SourceLocationData { const char* name; const char* function; const char* file; uint32_t line; uint32_t color; };
struct Color { enum : uint32_t { Red = 0xff0000, HotPink = 0xff69b4, DarkOrange = 0xff8c00 }; };
struct LockableCtx { explicit LockableCtx(const SourceLocationData*) {} void BeforeLock() {} void AfterLock() {} void AfterUnlock() {} void AfterTryLock(bool) {} void Mark(const SourceLocationData*) {} void CustomName(const char*, size_t) {} };
struct Profiler { bool IsConnected() const { return false; } };
inline Profiler& GetProfiler() { static Profiler p; return p; }
} // namespace tracy
