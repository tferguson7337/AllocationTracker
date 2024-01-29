#include <AllocationTracker.h>
#include <SimpleLoggingLibrary.h>

#include <memory>
#include <stacktrace>


// C++ new/new[] operator overloads //

#pragma warning(push)
#pragma warning(disable: 28213) // The _Use_decl_annotations_ annotation must be used to reference, without modification, a prior declaration. No prior declaration found.

[[nodiscard]] _Use_decl_annotations_
void* operator new(
    const std::size_t byteCount)
{
    using Flag = AllocationTracking::AllocFlag;
    return AllocationTracking::PerformAllocation(Flag::None, byteCount);
}

[[nodiscard]] _Use_decl_annotations_
void* operator new(
    const std::size_t byteCount,
    const std::align_val_t alignment)
{
    using Flag = AllocationTracking::AllocFlag;
    return AllocationTracking::PerformAllocation(Flag::CustomAlignment, byteCount, alignment);
}

[[nodiscard]] _Use_decl_annotations_
void* operator new(
    const std::size_t byteCount,
    const std::nothrow_t&) noexcept
{
    using Flag = AllocationTracking::AllocFlag;
    return AllocationTracking::PerformAllocation(Flag::NoThrow, byteCount);
}

[[nodiscard]] _Use_decl_annotations_
void* operator new(
    const std::size_t byteCount,
    const std::align_val_t alignment,
    const std::nothrow_t&) noexcept
{
    using Flag = AllocationTracking::AllocFlag;
    return AllocationTracking::PerformAllocation(Flag::NoThrow | Flag::CustomAlignment, byteCount, alignment);
}

[[nodiscard]] _Use_decl_annotations_
void* operator new[](
    const std::size_t byteCount)
{
    using Flag = AllocationTracking::AllocFlag;
    return AllocationTracking::PerformAllocation(Flag::Array, byteCount);
}

[[nodiscard]] _Use_decl_annotations_
void* operator new[](
    const std::size_t byteCount,
    const std::align_val_t alignment)
{
    using Flag = AllocationTracking::AllocFlag;
    return AllocationTracking::PerformAllocation(Flag::Array | Flag::CustomAlignment, byteCount, alignment);
}

[[nodiscard]] _Use_decl_annotations_
void* operator new[](
    const std::size_t byteCount,
    const std::nothrow_t&) noexcept
{
    using Flag = AllocationTracking::AllocFlag;
    return AllocationTracking::PerformAllocation(Flag::Array | Flag::NoThrow, byteCount);
}

[[nodiscard]] _Use_decl_annotations_
void* operator new[](
    const std::size_t byteCount,
    const std::align_val_t alignment,
    const std::nothrow_t&) noexcept
{
    using Flag = AllocationTracking::AllocFlag;
    return AllocationTracking::PerformAllocation(Flag::Array | Flag::NoThrow | Flag::CustomAlignment, byteCount, alignment);
}


// C++ delete/delete[] operator overloads //

void operator delete(
    void* pMem) noexcept
{
    using Flag = AllocationTracking::AllocFlag;
    return AllocationTracking::PerformDeallocation(Flag::NoThrow, pMem);
}

void operator delete(
    void* pMem,
    [[maybe_unused]] const std::align_val_t alignment) noexcept
{
    using Flag = AllocationTracking::AllocFlag;
    return AllocationTracking::PerformDeallocation(Flag::NoThrow | Flag::CustomAlignment, pMem);
}

void operator delete[](
    void* pMem) noexcept
{
    using Flag = AllocationTracking::AllocFlag;
    return AllocationTracking::PerformDeallocation(Flag::Array | Flag::NoThrow, pMem);
}

void operator delete[](
    void* pMem,
    [[maybe_unused]] const std::align_val_t alignment) noexcept
{
    using Flag = AllocationTracking::AllocFlag;
    return AllocationTracking::PerformDeallocation(Flag::Array | Flag::NoThrow | Flag::CustomAlignment, pMem);
}

#pragma warning(pop)

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


auto g_pStdOutLogger{SLL::Factory<SLL::SyncLogger<SLL::StdOutLogger>>{}()};

auto GenerateLogger()
{
    using namespace std::literals::string_view_literals;
    static constexpr auto s_cFileLoggerPath{LR"(C:\users\tferg\desktop\AllocationTrackingLog.txt)"sv};
    std::filesystem::remove(s_cFileLoggerPath);

    auto pDispatchLogger = SLL::Factory<SLL::DispatchLogger>{}();

    const bool bRegistrationSuccessful =
        pDispatchLogger->RegisterLoggers(
            g_pStdOutLogger,
            SLL::Factory<SLL::FileLogger>{}(s_cFileLoggerPath));
    if (!bRegistrationSuccessful)
    {
        SLL::StdErrLogger{}("Failed to register loggers");
    }

    return pDispatchLogger;
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

    g_pStdOutLogger->Log("Thread[{}]\n  TotalCreations[{}]\n  Min Creation Dur[{}]\n  Max Creation Dur[{}]\n  TotalCreationDur[{}]\n  Avg Creation Dur[{}]\n\n",
        std::this_thread::get_id(),
        s_cOuterLoopAmount * s_cInnerLoopAmount,
        std::chrono::duration_cast<std::chrono::microseconds>(minCreationDur),
        std::chrono::duration_cast<std::chrono::microseconds>(maxCreationDur),
        std::chrono::duration_cast<std::chrono::milliseconds>(totalCreationDur),
        std::chrono::duration_cast<std::chrono::microseconds>(totalCreationDur / (s_cOuterLoopAmount * s_cInnerLoopAmount)));
}

void LogAllocs(_Inout_ SLL::ValidLoggerType auto& logger)
{
    auto LogCallback = [&logger](_In_ const std::string_view logMsgSV) mutable
    {
        logger.Log("{}", logMsgSV);
    };

    AllocationTracking::LogAllocations(LogCallback, AllocationTracking::LogSummaryType::FullStackTraces, true);
}

int main(
    [[maybe_unused]] _In_ const int argc,
    [[maybe_unused]] _In_count_(argc) const char* argv[])
{
    AllocationTracking::ScopedTrackerInit at_sti_;
    AllocationTracking::CollectFullStackTraces(true);
    AllocationTracking::RegisterExternalStackEntryMarker("!SLL::Factory");
    AllocationTracking::RegisterExternalStackEntryMarker("!GenerateLogger");

    g_pStdOutLogger->Log("Testing AllocationTracking");
    {
        AllocationTracking::ScopedThreadLocalTrackingDisabler at_stltd_;
        g_pStdOutLogger->Log("Reference StackTrace:\n{}", std::stacktrace::current());
    }

    auto pLogger = GenerateLogger();

    LogAllocs(*pLogger);
    {
        g_pTestBuffers = std::make_unique<TestBuffersArray>();
        LogAllocs(*pLogger);

        {
            const auto allocT0{std::chrono::steady_clock::now()};
            std::ranges::generate(std::array<std::jthread, s_cTestBuffersArrayLength>{},
                [idx = std::size_t{0}]() mutable { return std::jthread{AllocateForTestBuffer, idx++}; });
            const auto allocT1{std::chrono::steady_clock::now()};
            g_pStdOutLogger->Log("Test Buffer Alloc Duration[{}]",
                std::chrono::duration_cast<std::chrono::milliseconds>(allocT1 - allocT0));
        }

        LogAllocs(*pLogger);

        {
            const auto freeT0{std::chrono::steady_clock::now()};
            g_pTestBuffers.reset();
            const auto freeT1{std::chrono::steady_clock::now()};
            g_pStdOutLogger->Log("Test Buffer Free Duration[{}]",
                std::chrono::duration_cast<std::chrono::milliseconds>(freeT1 - freeT0));
        }
    }
    LogAllocs(*pLogger);
    {
        static constexpr std::size_t s_cElems = (1 << 30);
        auto ptr = std::make_unique<std::uint32_t[]>(s_cElems);
        if (!!ptr) { std::fill(&ptr[0], &ptr[s_cElems], 0); }
        LogAllocs(*pLogger);
    }

    for (auto i = 0u; i < 10u; ++i)
    {
        LogAllocs(*pLogger);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    LogAllocs(*pLogger);
}
