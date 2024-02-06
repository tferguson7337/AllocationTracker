#include <AllocationTracker.h>
#include <SimpleLoggingLibrary.h>

#include <Windows.h>

#include <map>
#include <memory>
#include <stacktrace>


class [[nodiscard]] ScopedLibraryHandle
{
private:

    HMODULE m_hLib{nullptr};

public:

    constexpr ScopedLibraryHandle(_In_opt_ HMODULE hLib = nullptr) noexcept :
        m_hLib{hLib}
    { }

    ScopedLibraryHandle(_In_ const ScopedLibraryHandle&) = delete;
    ScopedLibraryHandle(_Inout_ ScopedLibraryHandle&& other) noexcept :
        m_hLib{other.m_hLib}
    {
        other.m_hLib = nullptr;
    }

    ~ScopedLibraryHandle()
    {
        Reset();
    }

    ScopedLibraryHandle& operator=(_In_ const ScopedLibraryHandle&) = delete;
    ScopedLibraryHandle& operator=(_Inout_ ScopedLibraryHandle&& other) noexcept
    {
        if (this != &other)
        {
            Reset();

            m_hLib = other.m_hLib;
            other.m_hLib = nullptr;
        }

        return *this;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return !!m_hLib;
    }

    [[nodiscard]] constexpr operator bool() const noexcept
    {
        return IsValid();
    }

    [[nodiscard]] constexpr HMODULE Get() const noexcept
    {
        return m_hLib;
    }

    [[nodiscard]] constexpr operator HMODULE() const noexcept
    {
        return Get();
    }

    void Reset()
    {
        if (IsValid())
        {
            FreeLibrary(m_hLib);
            m_hLib = nullptr;
        }
    }
};

ScopedLibraryHandle g_hAllocationTrackerLib;


std::filesystem::path BuildLoggerPath(_In_opt_z_ const char* pFilePath)
{
    const auto p{std::filesystem::path{pFilePath ? pFilePath : R"(C:\users\tferg\desktop\)"} / "ScratchPad_AllocationTrackingLog.txt"};
    std::filesystem::remove(p);
    return p;
}

std::vector<std::uint8_t> CreateDynamicBuffer(_In_ const std::size_t reserveBytes)
{
    /**/
    if ((reserveBytes % 100) == 0)
    {
        std::this_thread::yield();
    }
    /**/

    return std::vector<std::uint8_t>((reserveBytes % 1024) + 1);
}


auto g_pStdOutLogger{SLL::UniqueFactory<SLL::SyncLogger<SLL::StdOutLogger>>{}()};
decltype(SLL::Factory<SLL::DispatchLogger>{}()) g_pDispatchLogger;

void GenerateLoggers()
{
    if (!!g_pDispatchLogger.Get())
    {
        return;
    }

    using namespace std::literals::string_view_literals;
    static constexpr auto s_cFileLoggerPath{LR"(.\AllocationTrackingLog.txt)"sv};
    std::filesystem::remove(s_cFileLoggerPath);

    g_pDispatchLogger = SLL::Factory<SLL::DispatchLogger>{}(
        g_pStdOutLogger,
        SLL::Factory<SLL::FileLogger>{}(s_cFileLoggerPath));
}

static constexpr std::size_t s_cTestBuffersArrayLength{64};
using TestBuffers = std::vector<std::vector<std::uint8_t>>;
using TestBuffersArray = std::array<TestBuffers, s_cTestBuffersArrayLength>;
std::unique_ptr<TestBuffersArray> g_pTestBuffers;

void AllocateForTestBuffer(_In_ const std::size_t idx)
{
    // AllocationTracking::ScopedThreadLocalTrackingDisabler at_stltd;

    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    static constexpr auto s_cOuterLoopAmount{1'000u};
    static constexpr auto s_cInnerLoopAmount{100u};

    Duration minCreationDur{(std::numeric_limits<Duration::rep>::max)()};
    Duration maxCreationDur{(std::numeric_limits<Duration::rep>::min)()};

    const auto totalT0{Clock::now()};
    TestBuffers& buffers = (*g_pTestBuffers)[idx];
    for (auto i = 0u; i < s_cOuterLoopAmount; ++i)
    {
        buffers = TestBuffers(s_cInnerLoopAmount);
        std::ranges::generate(buffers, [&minCreationDur, &maxCreationDur, i = std::size_t{0}]() mutable
        {
            const auto t0{Clock::now()};
            const auto ret{CreateDynamicBuffer(i++)};
            const auto dur{Clock::now() - t0};

            minCreationDur = (std::min)(minCreationDur, dur);
            maxCreationDur = (std::max)(maxCreationDur, dur);

            return ret;
        });
    }
    Duration totalCreationDur{Clock::now() - totalT0};

    buffers.clear();

    /**
    g_pStdOutLogger->Log("Thread[{}]\n  TotalCreations[{}]\n  Min Creation Dur[{}]\n  Max Creation Dur[{}]\n  TotalCreationDur[{}]\n  Avg Creation Dur[{}]\n\n",
        std::this_thread::get_id(),
        s_cOuterLoopAmount * s_cInnerLoopAmount,
        std::chrono::duration_cast<std::chrono::microseconds>(minCreationDur),
        std::chrono::duration_cast<std::chrono::microseconds>(maxCreationDur),
        std::chrono::duration_cast<std::chrono::milliseconds>(totalCreationDur),
        std::chrono::duration_cast<std::chrono::microseconds>(totalCreationDur / (s_cOuterLoopAmount * s_cInnerLoopAmount)));
    /**/
}

void LogAllocs()
{
    if (!g_pDispatchLogger.Get())
    {
        return;
    }

    auto LogCallback = [](_In_ const std::string_view logMsgSV) mutable
    {
        g_pDispatchLogger->Log("{}", logMsgSV);
    };

    static constexpr auto s_cLogType{AllocationTracking::LogSummaryType::FullStackTraces};
    const auto pfLogAllocaitons = reinterpret_cast<AllocationTracking::LogAllocationsFn>(
        GetProcAddress(g_hAllocationTrackerLib, "AllocationTracker_LogAllocations"));
    if (!!pfLogAllocaitons)
    {
        pfLogAllocaitons(LogCallback, s_cLogType);
    }
}


void RunTrackerTests()
{
    // LogAllocs();
    {
        g_pTestBuffers = std::make_unique<TestBuffersArray>();
        // LogAllocs();

        {
            const auto allocT0{std::chrono::steady_clock::now()};
            std::ranges::generate(std::array<std::jthread, s_cTestBuffersArrayLength>{},
                [idx = std::size_t{0}]() mutable { return std::jthread{AllocateForTestBuffer, idx++}; });
            const auto dur{std::chrono::steady_clock::now() - allocT0};
            g_pStdOutLogger->Log("Test Buffer Alloc Duration[{}]", std::chrono::duration_cast<std::chrono::milliseconds>(dur));
        }

        // LogAllocs();

        {
            const auto freeT0{std::chrono::steady_clock::now()};
            g_pTestBuffers.reset();
            const auto freeT1{std::chrono::steady_clock::now()};
            g_pStdOutLogger->Log("Test Buffer Free Duration[{}]", std::chrono::duration_cast<std::chrono::milliseconds>(freeT1 - freeT0));
        }
    }
    // LogAllocs();
    {
        static constexpr std::size_t s_cElems = (1 << 30);
        auto ptr = std::make_unique<std::uint32_t[]>(s_cElems);
        if (!!ptr) { std::fill(&ptr[0], &ptr[s_cElems], 0); } // Touch the memory so it gets committed.
        // LogAllocs();
    }

    for (auto i = 0u; i < 10u; ++i)
    {
        // LogAllocs();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    LogAllocs();
}


static ScopedLibraryHandle EnableTracking()
{
    using namespace std::string_view_literals;
    ScopedLibraryHandle hAllocationTracker{LoadLibraryA(".\\AllocationTracker.dll")};
    if (!hAllocationTracker)
    {
        SLL::StdErrLogger{}.Log("Failed to load AllocationTracker DLL - WinErr[{}]", GetLastError());
        return {};
    }

    const auto pfSetTargetModuleNamePrefix = reinterpret_cast<AllocationTracking::SetTargetModuleNamePrefix>(
        GetProcAddress(hAllocationTracker, "AllocationTracker_SetTargetModuleNamePrefix"));
    if (!pfSetTargetModuleNamePrefix)
    {
        SLL::StdErrLogger{}.Log("Failed to find AllocationTracker_SetTargetModuleNamePrefix function - WinErr[{}]", GetLastError());
        return {};
    }
    pfSetTargetModuleNamePrefix("Testing!");

    const auto pfRegisterExternalStackEntryMarkers = reinterpret_cast<AllocationTracking::RegisterExternalStackEntryMarkers>(
        GetProcAddress(hAllocationTracker, "AllocationTracker_RegisterExternalStackEntryMarkers"));
    if (!pfRegisterExternalStackEntryMarkers)
    {
        SLL::StdErrLogger{}.Log("Failed to find AllocationTracker_RegisterExternalStackEntryMarkers function - WinErr[{}]", GetLastError());
        return {};
    }
    pfRegisterExternalStackEntryMarkers({"!SLL::Factory"sv, "!GenerateLogger"sv});

    const auto pfSetCollectFullStackTraces = reinterpret_cast<AllocationTracking::SetCollectFullStackTraces>(
        GetProcAddress(hAllocationTracker, "AllocationTracker_SetCollectFullStackTraces"));
    if (!pfSetCollectFullStackTraces)
    {
        SLL::StdErrLogger{}.Log("Failed to find AllocationTracker_SetCollectFullStackTraces function - WinErr[{}]", GetLastError());
        return {};
    }
    pfSetCollectFullStackTraces(true);

    const auto pfEnableTracking = reinterpret_cast<AllocationTracking::EnableTracking>(
        GetProcAddress(hAllocationTracker, "AllocationTracker_EnableTracking"));
    if (!pfSetCollectFullStackTraces)
    {
        SLL::StdErrLogger{}.Log("Failed to find AllocationTracker_EnableTracking function - WinErr[{}]", GetLastError());
        return {};
    }
    pfEnableTracking(true);

    return hAllocationTracker;
}

static volatile bool g_bReadyForExit{true};

int main(
    [[maybe_unused]] _In_ const int argc,
    [[maybe_unused]] _In_count_(argc) const char* argv[])
{
    g_hAllocationTrackerLib = EnableTracking();
    if (!g_hAllocationTrackerLib)
    {
        return EXIT_FAILURE;
    }

    GenerateLoggers();
    g_pStdOutLogger->Log("Testing AllocationTracking");

    do
    {
        RunTrackerTests();
    } while (!g_bReadyForExit);

    g_bReadyForExit = true;
    do
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } while (!g_bReadyForExit);
}
