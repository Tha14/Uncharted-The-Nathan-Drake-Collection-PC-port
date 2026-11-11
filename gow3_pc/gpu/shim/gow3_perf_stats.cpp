// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_perf_stats.h"

#include <atomic>
#include <cctype>  // std::tolower for the NVML device name match
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cwchar>  // wcsstr on the PDH instance names
#include <mutex>
#include <string>  // std::string / std::wstring members below
#include <thread>
#include <utility> // std::move
#include <vector>
#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_instance.h"

#ifdef _WIN32
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#endif

namespace Gow3PerfStats {

namespace {

std::mutex mutex;
Sample latest;
std::once_flag started;

// gow3 20 Oct 2026 (idle CPU): once started the thread sampled twice a second for good, the
// overlay hidden or not; its GPU counter query reads the GPU engines of every process (the most
// expensive part). It now samples once a second, and only while Start() was called in the last
// 2 s (each overlay frame showing a CPU, RAM, GPU or VRAM item calls it); otherwise it sleeps
// until the next call. GOW3_PERF_SAMPLER_PAUSE=0: every 500 ms, never paused.
bool PauseWhenHidden() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_PERF_SAMPLER_PAUSE");
        const bool pause = !(env && env[0] == '0');
        // Startup line naming the sampling cadence (temperatures included since 24 Oct 2026).
#define GOW3_SAMPLER_LINE "Overlay: CPU/RAM/GPU/VRAM and their temperatures sampled %s\n"
        std::printf(GOW3_SAMPLER_LINE,
                    pause ? "every second while shown (GOW3_PERF_SAMPLER_PAUSE=0: every 500 ms)"
                          : "every 500 ms (GOW3_PERF_SAMPLER_PAUSE=0)");
#undef GOW3_SAMPLER_LINE
        return pause;
    }();
    return on;
}
constexpr auto IdleAfter = std::chrono::seconds(2);
std::atomic<int64_t> last_demand_ns{0}; ///< steady_clock time of the last Start()
std::mutex demand_mutex;
std::condition_variable demand_cv;
std::atomic<bool> paused{false}; ///< the thread sleeps on demand_cv (changed under demand_mutex)

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::chrono::milliseconds SamplePeriod() {
    return std::chrono::milliseconds(PauseWhenHidden() ? 1000 : 500);
}

/// Sampling thread: sleeps while no overlay item needs the samples. True when it slept (the
/// CPU and GPU use need a new starting sample).
bool WaitWhileHidden() {
    const int64_t idle_ns = std::chrono::nanoseconds(IdleAfter).count();
    if (!PauseWhenHidden() || NowNs() - last_demand_ns.load() < idle_ns) {
        return false;
    }
    std::unique_lock lock{demand_mutex};
    paused = true;
    // Start() stores its time, then reads `paused` (both sequentially consistent): it either
    // sees true here and wakes this thread, or its time is seen by this check.
    if (NowNs() - last_demand_ns.load() < idle_ns) {
        paused = false;
        return false;
    }
    demand_cv.wait(lock, [] { return !paused.load(); });
    return true;
}

#ifdef _WIN32
uint64_t Ticks(const FILETIME& time) {
    return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

// This process's use of the GPU's 3D engines, as Task Manager shows it ("GPU Engine" counters,
// any vendor). Negative until the second sample or without the counters.
class GpuUsage {
public:
    GpuUsage() {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
            query = nullptr;
            return;
        }
        if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) !=
            ERROR_SUCCESS) {
            PdhCloseQuery(query);
            query = nullptr;
            return;
        }
        PdhCollectQueryData(query);
        pid = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    }

    float Percent() {
        if (!query || PdhCollectQueryData(query) != ERROR_SUCCESS) {
            return -1.0f;
        }
        DWORD size = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count,
                                         nullptr) != PDH_MORE_DATA) {
            return -1.0f;
        }
        buffer.resize(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count,
                                         items) != ERROR_SUCCESS) {
            return -1.0f;
        }
        double total = 0.0;
        for (DWORD i = 0; i < count; ++i) {
            const wchar_t* name = items[i].szName;
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA && wcsstr(name, pid.c_str()) &&
                wcsstr(name, L"engtype_3D")) {
                total += items[i].FmtValue.doubleValue;
            }
        }
        return std::clamp(float(total), 0.0f, 100.0f);
    }

private:
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    std::wstring pid;
    std::vector<unsigned char> buffer;
};

// What the two temperature readers below return when they have nothing.
constexpr float kNoReading = -1.0f;

// Graphics card temperature (gow3, 24 Oct 2026), read through NVML. Every NVIDIA driver installs
// nvml.dll; it is opened at run time and its entry points are looked up by name, which keeps the
// NVML SDK (headers, import library) out of the build. Where it is missing (AMD, Intel) the
// counter just leaves the temperature out. With more than one NVIDIA device, the one whose name
// matches the Vulkan device is read, so a hybrid laptop never reports its integrated chip.
class NvmlTemperature {
    // NVML entry points (every one returns nvmlReturn_t, 0 = success).
    using InitProc = int (*)();
    using DeviceCountProc = int (*)(unsigned*);
    using DeviceAtProc = int (*)(unsigned, void**);
    using DeviceNameProc = int (*)(void*, char*, unsigned);
    using TemperatureProc = int (*)(void*, int, unsigned*);

    std::string vk_device;
    HMODULE dll = nullptr;
    void* device = nullptr; ///< nvmlDevice_t of the card being read
    int64_t attempt_ms = 0; ///< steady clock of the last Connect()
    InitProc nvml_init = nullptr;
    DeviceCountProc device_count = nullptr;
    DeviceAtProc device_at = nullptr;
    DeviceNameProc device_name_of = nullptr;
    TemperatureProc temperature_of = nullptr;

    static int64_t SteadyMs() {
        return NowNs() / 1'000'000;
    }

    /// Lower case, one byte at a time (std::tolower).
    static std::string Folded(const std::string& text) {
        std::string folded;
        folded.reserve(text.size());
        for (const unsigned char byte : text) {
            folded.push_back(static_cast<char>(std::tolower(byte)));
        }
        return folded;
    }

    /// Case-blind: the two names are equal, or one of them contains the other.
    static bool NamesMatch(const char* nvml_device, const std::string& vulkan_device) {
        const std::string driver_side = Folded(nvml_device);
        const std::string vulkan_side = Folded(vulkan_device);
        return !driver_side.empty() && !vulkan_side.empty() &&
               (driver_side.find(vulkan_side) != std::string::npos ||
                vulkan_side.find(driver_side) != std::string::npos);
    }

    /// Looks `primary` up, then `fallback` when the newer export is absent.
    template <typename Proc>
    Proc Lookup(const char* primary, const char* fallback = nullptr) const {
        FARPROC address = GetProcAddress(dll, primary);
        if (!address && fallback) {
            address = GetProcAddress(dll, fallback);
        }
        return reinterpret_cast<Proc>(address);
    }

    /// Opens nvml.dll and initialises NVML; false (the DLL released again) when that fails.
    bool OpenLibrary() {
        // From System32 only (the driver puts it there): a file of that name next to the
        // game is never loaded (24 Oct 2026).
        dll = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (dll == nullptr) {
            return false;
        }
        nvml_init = Lookup<InitProc>("nvmlInit_v2", "nvmlInit");
        device_count = Lookup<DeviceCountProc>("nvmlDeviceGetCount_v2", "nvmlDeviceGetCount");
        device_at = Lookup<DeviceAtProc>("nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex");
        device_name_of = Lookup<DeviceNameProc>("nvmlDeviceGetName");
        temperature_of = Lookup<TemperatureProc>("nvmlDeviceGetTemperature");
        const bool usable = nvml_init && device_count && device_at && temperature_of;
        if (usable && nvml_init() == 0) {
            return true;
        }
        FreeLibrary(dll);
        dll = nullptr;
        return false;
    }

    /// Index of the NVML device named like the Vulkan one; 0 when none matches.
    unsigned PickDevice(unsigned devices) const {
        for (unsigned index = 0; index < devices; ++index) {
            void* probe = nullptr;
            char label[96] = {};
            const bool named = device_at(index, &probe) == 0 &&
                               device_name_of(probe, label, sizeof(label)) == 0;
            if (named && NamesMatch(label, vk_device)) {
                return index;
            }
        }
        return 0;
    }

    /// (Re)loads NVML when needed and picks the device to read.
    void Connect() {
        attempt_ms = SteadyMs();
        unsigned devices = 0;
        if ((dll || OpenLibrary()) && device_count(&devices) == 0 && devices > 0) {
            const unsigned index = (devices > 1 && device_name_of) ? PickDevice(devices) : 0u;
            void* found = nullptr;
            device = (device_at(index, &found) == 0) ? found : nullptr;
        }
    }

public:
    explicit NvmlTemperature(std::string vulkan_device) : vk_device(std::move(vulkan_device)) {
        Connect();
    }

    /// Degrees Celsius, or kNoReading.
    float Read() {
        // Early in a session the driver may not respond yet; while unconnected, retry every 30 s.
        const bool connected = dll && device;
        if (!connected && SteadyMs() - attempt_ms > 30'000) {
            Connect();
        }
        unsigned reading = 0;
        constexpr int kGpuSensor = 0; // NVML_TEMPERATURE_GPU
        const bool ok = dll && device && temperature_of &&
                        temperature_of(device, kGpuSensor, &reading) == 0;
        const float degrees = static_cast<float>(reading);
        return (ok && degrees >= -20.0f && degrees <= 150.0f) ? degrees : kNoReading;
    }
};

// Processor temperature (gow3, 24 Oct 2026) from the ACPI thermal zones that Windows publishes as
// the "Thermal Zone Information" performance counters: Kelvin, readable without administrator
// rights, available on most desktops and laptops. The zone tracks the processor's load but sits a
// few degrees off the on-die sensor, which Windows only exposes to kernel drivers. A machine
// without any zone gets no CPU temperature.
class ThermalZoneTemperature {
    PDH_HQUERY pdh = nullptr;
    PDH_HCOUNTER zones = nullptr;
    std::vector<unsigned char> storage;

    /// Some firmware counts in tenths of a kelvin: anything over 1000 is scaled down.
    static double AsKelvin(double raw) {
        return raw > 1000.0 ? raw / 10.0 : raw;
    }
    /// 200..400 K (about -73..127 C); a value outside is not a real temperature.
    static bool InRange(double kelvin) {
        return kelvin >= 200.0 && kelvin <= 400.0;
    }

public:
    ThermalZoneTemperature() {
        constexpr const wchar_t* kZones = L"\\Thermal Zone Information(*)\\Temperature";
        PDH_HQUERY opened = nullptr;
        if (PdhOpenQueryW(nullptr, 0, &opened) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(opened, kZones, 0, &zones) == ERROR_SUCCESS) {
                pdh = opened;
                PdhCollectQueryData(pdh); // first sample; values come from the next collection
            } else {
                PdhCloseQuery(opened);
            }
        }
    }

    /// The warmest plausible zone in degrees Celsius, or kNoReading.
    float Read() {
        if (pdh == nullptr || PdhCollectQueryData(pdh) != ERROR_SUCCESS) {
            return kNoReading;
        }
        DWORD bytes = 0, entries = 0;
        const PDH_STATUS sized =
            PdhGetFormattedCounterArrayW(zones, PDH_FMT_DOUBLE, &bytes, &entries, nullptr);
        if (sized != PDH_MORE_DATA) {
            return kNoReading;
        }
        storage.resize(bytes);
        auto* rows = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
        const PDH_STATUS filled =
            PdhGetFormattedCounterArrayW(zones, PDH_FMT_DOUBLE, &bytes, &entries, rows);
        if (filled != ERROR_SUCCESS) {
            return kNoReading;
        }
        double warmest = 0.0;
        for (DWORD row = 0; row < entries; ++row) {
            const PDH_FMT_COUNTERVALUE& value = rows[row].FmtValue;
            const double kelvin = AsKelvin(value.doubleValue);
            if (value.CStatus == PDH_CSTATUS_VALID_DATA && InRange(kelvin) && kelvin > warmest) {
                warmest = kelvin;
            }
        }
        return warmest > 0.0 ? static_cast<float>(warmest - 273.15) : kNoReading;
    }
};

void Run(const Vulkan::Instance* instance) {
    GpuUsage gpu;
    NvmlTemperature card_heat{std::string(instance->GetModelName())};
    ThermalZoneTemperature cpu_heat;
    FILETIME created, ended, kernel, user, now;
    uint64_t last_process = 0, last_wall = 0;
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    constexpr double GiB = 1024.0 * 1024.0 * 1024.0;
    while (true) {
        if (WaitWhileHidden()) {
            // Shown again: CPU and GPU use are differences between two samples. New starting
            // sample now, the first values 250 ms later (not the average over the pause).
            GetSystemTimeAsFileTime(&now);
            if (GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user)) {
                last_process = Ticks(kernel) + Ticks(user);
                last_wall = Ticks(now);
            }
            gpu.Percent();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        Sample s;
        s.gpu_name = std::string(instance->GetModelName());
        GetSystemTimeAsFileTime(&now);
        if (GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user)) {
            const uint64_t process = Ticks(kernel) + Ticks(user), wall = Ticks(now);
            if (last_wall) {
                s.cpu_percent = CpuPercent(process - last_process, wall - last_wall, cores);
            }
            last_process = process;
            last_wall = wall;
        }
        // The game's own memory (its working set), not the whole system's (24 Oct 2026).
        PROCESS_MEMORY_COUNTERS memory{};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
            s.ram_used_gb = double(memory.WorkingSetSize) / GiB;
        }
        MEMORYSTATUSEX system{.dwLength = sizeof(MEMORYSTATUSEX)};
        if (GlobalMemoryStatusEx(&system)) {
            s.ram_total_gb = double(system.ullTotalPhys) / GiB;
        }
        if (instance->CanReportMemoryUsage()) {
            s.vram_used_gb = double(instance->GetDeviceMemoryUsage()) / GiB;
            s.vram_total_gb = double(instance->GetDeviceMemoryBudgetNow()) / GiB;
        }
        s.gpu_percent = gpu.Percent();
        s.gpu_temp_c = card_heat.Read();
        s.cpu_temp_c = cpu_heat.Read();
        {
            std::scoped_lock lock{mutex};
            latest = std::move(s);
        }
        std::this_thread::sleep_for(SamplePeriod());
    }
}
#else
void Run(const Vulkan::Instance* instance) {
    while (true) {
        WaitWhileHidden();
        Sample s;
        s.gpu_name = std::string(instance->GetModelName());
        {
            std::scoped_lock lock{mutex};
            latest = std::move(s);
        }
        std::this_thread::sleep_for(SamplePeriod());
    }
}
#endif

} // namespace

void Start(const Vulkan::Instance& instance) {
    last_demand_ns.store(NowNs());
    // ponytail: never joined; a thread that ended while the game ran crashed in ntdll
    // (vk_pipeline_serialization.cpp, StopWarmUp). It sleeps between samples, and while the
    // overlay shows no sampled item (gow3 20 Oct 2026, see PauseWhenHidden).
    std::call_once(started, [&] {
        std::thread([&instance] {
            Common::SetCurrentThreadName("gow3:PerfStats");
            Run(&instance);
        }).detach();
    });
    if (paused.load()) {
        {
            std::scoped_lock lock{demand_mutex};
            paused = false;
        }
        demand_cv.notify_one();
    }
}

Sample Latest() {
    std::scoped_lock lock{mutex};
    return latest;
}

} // namespace Gow3PerfStats
