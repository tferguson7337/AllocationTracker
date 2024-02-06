#include "AllocationTrackerInternal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <print>
#include <ranges>
#include <set>
#include <sstream>
#include <stacktrace>
#include <thread>
#include <unordered_map>
#include <vector>

#include "StringUtils.h"


// Global state trackers
namespace AllocationTracking
{
    thread_local std::int64_t gtl_IsInternalAllocationOrFree{0};
    thread_local std::int64_t gtl_IsNoTrackAllocationOrFree{0};

    constinit std::atomic<std::int64_t> g_InternalAllocations{0};
    constinit std::atomic<std::int64_t> g_InternalAllocationBytes{0};

    constinit std::atomic<std::int64_t> g_ExternalAllocations{0};
    constinit std::atomic<std::int64_t> g_ExternalAllocationBytes{0};

    constinit std::atomic<bool> g_bTrackingEnabled{false};
}

template <>
struct std::formatter<AllocationTracking::StackTraceEntryArray> : public std::formatter<std::basic_string<char>, char>
{
    template <typename FormatContext>
    constexpr auto format(_In_ const AllocationTracking::StackTraceEntryArray& stackTrace, _Inout_ FormatContext& fmtCtx) const
    {
        std::stringstream ss;

        std::size_t frameNum{0};
        for (const std::stacktrace_entry& ste : stackTrace.ToSpan())
        {
            ss << (frameNum++) << "> ";

            const auto file{ste.source_file()};
            if (!file.empty())
            {
                ss << file << '(' << ste.source_line() << "): ";
            }

            ss << ste.description();
            ss << '\n';
        }

        return std::formatter<std::basic_string<char>, char>::format(ss.str(), fmtCtx);
    }
};
/**/

namespace AllocationTracking
{
    ThreadTracker::~ThreadTracker()
    {
        if (m_bRegistered)
        {
            DeregisterWithGlobalTracker();
        }
    }

    bool ThreadTracker::RegisterWithGlobalTracker()
    {
        if (m_bRegistered)
        {
            return true;
        }
        auto pGlobalTracker = GlobalTracker::Instance();
        if (!!pGlobalTracker)
        {
            pGlobalTracker->RegisterThreadTracker(this);
            m_bRegistered = true;
        }

        return m_bRegistered;
    }

    void ThreadTracker::DeregisterWithGlobalTracker()
    {
        auto pGlobalTracker = GlobalTracker::Instance();
        if (!!pGlobalTracker)
        {
            pGlobalTracker->DeregisterThreadTracker(this);
            m_bRegistered = false;
        }
    }

    void ThreadTracker::AddToQueue(_Inout_ MemoryInfo&& info)
    {
        if (!m_bRegistered)
        {
            // If we haven't successfully registered, don't flood the queue.
            return;
        }
        if (!m_bTrackingReady)
        {
            // BaseThreadInitThunk has not been called yet, don't track early
            // DLL related allocations.
            return;
        }

        auto scopedLock{m_QueueBusyLock.AcquireScoped()};
        m_Queue.push_back(std::move(info));
    }

    void ThreadTracker::EnableTracking(_In_ const bool bEnable)
    {
        m_bTrackingReady = bEnable;
    }

    thread_local ThreadTracker gtl_ThreadTracker;
}


// GlobalTracker::WorkerThread
namespace AllocationTracking
{
    bool GlobalTracker::WorkerThread::WaitForWork()
    {
        static constexpr auto s_cWaitTimeMs{std::chrono::milliseconds(500)};
        std::this_thread::sleep_for(s_cWaitTimeMs);
        return m_bContinueWork;
    }

    MemoryInfoQueue GlobalTracker::WorkerThread::GetAllQueued()
    {
        using MemoryInfoQueueVector = std::vector<MemoryInfoQueue, SelfAllocator<MemoryInfoQueue>>;
        MemoryInfoQueueVector queuesToMerge = [this]()
        {
            MemoryInfoQueueVector tmp;

            // Handle registered ThreadTracker queues.
            {
                auto scopedRegistryLock{m_ThreadTrackerRegistryLock.AcquireScoped()};
                tmp.reserve(m_ThreadTrackerRegistry.size() + 2); // + 2 for the backlog and deregistered free queues
                for (const auto pThreadTracker : m_ThreadTrackerRegistry)
                {
                    auto scopedTTQueueLock{pThreadTracker->m_QueueBusyLock.AcquireScoped()};
                    tmp.push_back(std::move(pThreadTracker->m_Queue));
                    pThreadTracker->m_Queue = {};
                }
            }

            // Handle m_BacklogQueue
            {
                auto scopedBacklogLock{m_BacklogLock.AcquireScoped()};
                tmp.push_back(std::move(m_BacklogQueue));
                m_BacklogQueue = {};
            }

            // Handle m_DeregisteredFreeQueue
            {
                DeregisteredMemoryFreeInfoQueue dmfiq;
                {
                    auto scopedDeregisteredFreeQueueLock{m_DeregisteredFreeQueueLock.AcquireScoped()};
                    std::swap(dmfiq, m_DeregisteredFreeQueue);
                }
                MemoryInfoQueue miq;
                dmfiq.TransferTo(miq);
                tmp.push_back(std::move(miq));
            }

            return tmp;
        }();

        MemoryInfoQueue merged;
        std::ranges::for_each(queuesToMerge, [&merged](_In_ MemoryInfoQueue& toMerge)
        {
            auto SortById = [](_In_ const MemoryInfo& lhs, _In_ const MemoryInfo& rhs)
            {
                return lhs.m_Id < rhs.m_Id;
            };
            toMerge.sort(SortById); // !?!?!?
            merged.merge(toMerge, SortById);
        });

        return merged;
    }

    void GlobalTracker::WorkerThread::WorkerLoop()
    {
        while (WaitForWork())
        {
            auto pTracker{GlobalTracker::InstanceIfTrackingEnabled()};
            if (!pTracker)
            {
                continue;
            }

            while (true)
            {
                if (!m_bContinueWork)
                {
                    return;
                }

                auto queue{GetAllQueued()};
                if (queue.empty())
                {
                    // No work, go ahead and sleep for a bit.
                    break;
                }

                auto scopedLock{pTracker->m_TrackerLock.AcquireScoped()};
                for (auto& info : queue)
                {
                    if (!m_bContinueWork)
                    {
                        return;
                    }

                    if (!(info.m_OpFlagMask & OpFlag::Free))
                    {
                        pTracker->ProcessAllocationUnsafe(std::move(info));
                    }
                    else
                    {
                        pTracker->ProcessDeallocationUnsafe(std::move(info));
                    }
                }
            }
        }
    }


    GlobalTracker::WorkerThread::WorkerThread() :
        m_Thread{[this]() { WorkerLoop(); }}
    { }

    GlobalTracker::WorkerThread::~WorkerThread()
    {
        SignalStop(false);
    }

    void GlobalTracker::WorkerThread::Start()
    {
        SignalStop(true);
        m_bContinueWork = true;
        m_Thread = std::jthread{[this]() { WorkerLoop(); }};
    }

    void GlobalTracker::WorkerThread::SignalStop(_In_ const bool bWait)
    {
        m_bContinueWork = false;
        if (bWait && m_Thread.joinable())
        {
            m_Thread.join();
        }
    }

    void GlobalTracker::WorkerThread::RegisterThreadTracker(_In_ ThreadTracker* pThreadTracker)
    {
        const auto scopedLock{m_ThreadTrackerRegistryLock.AcquireScoped()};
        m_ThreadTrackerRegistry.emplace_back(pThreadTracker);
    }

    void GlobalTracker::WorkerThread::DeregisterThreadTracker(_In_ ThreadTracker* pThreadTracker)
    {
        {
            auto scopedLock{m_ThreadTrackerRegistryLock.AcquireScoped()};
            auto registrationItr{std::ranges::find(m_ThreadTrackerRegistry, pThreadTracker)};
            if (registrationItr != m_ThreadTrackerRegistry.end())
            {
                std::swap(*registrationItr, m_ThreadTrackerRegistry.back());
                m_ThreadTrackerRegistry.pop_back();
            }
        }

        // Move any queued alloc/dealloc's to the backlog queue.
        auto scopedBacklogLock{m_BacklogLock.AcquireScoped()};
        auto scopedThreadTrackerQueueLock{pThreadTracker->m_QueueBusyLock.AcquireScoped()};
        m_BacklogQueue.splice(m_BacklogQueue.end(), pThreadTracker->m_Queue);
    }


    void GlobalTracker::WorkerThread::AddToDeregisteredFreeQueue(_Inout_ DeregisteredMemoryFreeInfo&& info)
    {
        auto scopedDeregisteredFreeQueueLock{m_DeregisteredFreeQueueLock.AcquireScoped()};
        m_DeregisteredFreeQueue.AddToQueue(std::move(info));
    }
}

// GlobalTracker
namespace AllocationTracking
{
    [[nodiscard]] auto SystemClockTimestampToLocalTime(_In_ const std::chrono::system_clock::time_point timestamp)
        -> decltype(std::chrono::current_zone()->to_local(timestamp))
    {
        try
        {
            const auto pCurrentZone{std::chrono::current_zone()};
            if (!!pCurrentZone)
            {
                return std::chrono::current_zone()->to_local(timestamp);
            }
        }
        catch (...)
        {
            // Nothing to do...
        }

        return {};
    }

    [[nodiscard]] static std::string FormatSTEAllocationSummaryInfoPair(_In_ const auto steAllocSummaryInfoPair)
    {
        using namespace StringUtils::Fmt;
        using FmtByte = Memory::Fixed::Byte<>;
        using FmtByteUpToMebibyte = Memory::AutoConverting::Byte<Memory::UnitTags::Mebibyte>;
        using FmtDec = Numeric::Dec<>;

        static constexpr auto s_cSTESummaryInfoFormatStr = R"Fmt(

  Location
    Function[{0:}]
    File[{1:}@{2:}]
  Allocations[{3:} : {4:} ({5:})]
  Oldest[{6:%m}/{6:%d}/{6:%Y} {6:%T}]
  Newest[{7:%m}/{7:%d}/{7:%Y} {7:%T}]
)Fmt";
        const auto& [pStackTraceEntry, pAllocSummaryInfo] = steAllocSummaryInfoPair;
        return std::format(
            s_cSTESummaryInfoFormatStr,
            pStackTraceEntry->description(),
            pStackTraceEntry->source_file(), FmtDec{}(pStackTraceEntry->source_line()),
            FmtDec{}(pAllocSummaryInfo->m_TotalAllocations), FmtByteUpToMebibyte{}(pAllocSummaryInfo->m_TotalBytes), FmtByte{}(pAllocSummaryInfo->m_TotalBytes),
            SystemClockTimestampToLocalTime(pAllocSummaryInfo->m_OldestAllocation),
            SystemClockTimestampToLocalTime(pAllocSummaryInfo->m_NewestAllocation));
    }

    [[nodiscard]] static std::string FormatFullStackTraceAllocationSummaryInfoPair(_In_ const auto fullStackTraceAllocSummaryInfoPair)
    {
        using namespace StringUtils::Fmt;
        using FmtByte = Memory::Fixed::Byte<>;
        using FmtByteUpToMebibyte = Memory::AutoConverting::Byte<Memory::UnitTags::Mebibyte>;
        using FmtDec = Numeric::Dec<>;

        static constexpr auto s_cFullStackSummaryInfoFormatStr = R"Fmt(

  Stack
{0:}

  Allocations[{1:} : {2:} ({3:})]
  Oldest[{4:%m}/{4:%d}/{4:%Y} {4:%T}]
  Newest[{5:%m}/{5:%d}/{5:%Y} {5:%T}]
)Fmt";
        const auto& [pFullStackTrace, pAllocSummaryInfo] = fullStackTraceAllocSummaryInfoPair;
        return std::format(
            s_cFullStackSummaryInfoFormatStr,
            *pFullStackTrace,
            FmtDec{}(pAllocSummaryInfo->m_TotalAllocations), FmtByteUpToMebibyte{}(pAllocSummaryInfo->m_TotalBytes), FmtByte{}(pAllocSummaryInfo->m_TotalBytes),
            SystemClockTimestampToLocalTime(pAllocSummaryInfo->m_OldestAllocation),
            SystemClockTimestampToLocalTime(pAllocSummaryInfo->m_NewestAllocation));
    }

    //
    // Returns true if `entry` does not come from something "internal", e.g.:
    //  - `std` namespace
    //  - AllocationTracking namespace.
    //  - Contains a string that was added via `RegisterExternalStackEntryMarker`.
    //  - Allocation related (e.g,, operator new, malloc, etc.).
    //  - If TargetModuleNamePrefix has been set, must match beginning of STE description (e.g., "MyProgram.exe!").
    //      - Helps exclude calls internal to DLLs.
    //
    //  Note, this is not comprehensive.
    //
    [[nodiscard]] static bool IsUserStackTraceEntry(
        _In_ const LogInformationPackage& infoPkg,
        _In_ const std::stacktrace_entry& entry)
    {
        // TODO: Settle on a better naming schema than internal or external.
        //      Perhaps "NonBucketable"? ...ugh.
        using namespace std::literals::string_view_literals;

        static constexpr std::array s_cMSVCInternalSourceFileMarkers
        {
            // Filter out common MSVC related source file paths.
            "minkernel\\crts\\"sv,
            "\\Microsoft Visual Studio\\"sv,
            "\\src\\vctools\\crt\\"sv,
        };

        const std::string sourcePath{entry.source_file()};
        auto SourcePathContainsMarker =
            [sv = std::string_view{sourcePath}](_In_ const std::string_view marker) { return sv.contains(marker); };

        if (std::ranges::any_of(s_cMSVCInternalSourceFileMarkers, SourcePathContainsMarker))
        {
            return false;
        }

        static constexpr std::array s_cInternalStackTraceEntryMarkers
        {
            // Don't bucket functions that have description beginning with backtick or underscore.
            // These are typically lambdas, or MSVC internal functions.
            "!`"sv,
            "!_"sv,
            "!<"sv,

            "!std::"sv,
            "!AllocationTracking::"sv,
            "!operator new"sv,
            "!malloc"sv,
            "!calloc"sv,
            "!realloc"sv,
        };

        const std::string desc{entry.description()};
        auto DescriptionContainsMarker =
            [sv = std::string_view{desc}](_In_ const std::string_view marker) { return sv.contains(marker); };

        if (std::ranges::any_of(s_cInternalStackTraceEntryMarkers, DescriptionContainsMarker))
        {
            return false;
        }

        if (std::ranges::any_of(infoPkg.m_ExternalUserStackTraceEntryMarkers, DescriptionContainsMarker))
        {
            return false;
        }

        if (infoPkg.m_TargetModuleNamePrefix.empty())
        {
            // If we don't have a target module name, assume this is good.
            return true;
        }

        return desc.starts_with(infoPkg.m_TargetModuleNamePrefix);
    }

    [[nodiscard]] static std::stacktrace_entry FindFirstUserStackTraceEntryByDescription(
        _In_ const LogInformationPackage& infoPkg,
        _In_ const MemoryInfo& info)
    {
        const auto stSpan{info.m_StackTrace.ToSpan()};
        const auto itr{std::ranges::find_if(stSpan,
            [&infoPkg](const auto entry) { return IsUserStackTraceEntry(infoPkg, entry); })};
        return (itr == stSpan.cend()) ? stSpan.front() : *itr;
    }

    template <LogSummaryType Type>
    static auto GenerateStackTraceEntryBucketedMapOfMemoryInfoPtrSets(
        _In_ const LogInformationPackage& infoPkg)
    {
        using MemoryInfoPtr = const MemoryInfo*;
        struct MemoryInfoPtrGreater
        {
            [[nodiscard]] constexpr bool operator()(
                _In_ const MemoryInfoPtr pLhs,
                _In_ const MemoryInfoPtr pRhs) const noexcept
            {
                // Sort by '> bytes' to prioritize larger allocs.
                return pLhs->m_Bytes > pRhs->m_Bytes;
            }
        };

        struct AllocSummaryInfo
        {
            std::size_t m_TotalBytes{0};
            std::size_t m_TotalAllocations{0};

            MemoryInfo::TimePoint m_OldestAllocation{(MemoryInfo::TimePoint::max)()};
            MemoryInfo::TimePoint m_NewestAllocation{(MemoryInfo::TimePoint::min)()};

            AllocSummaryInfo() noexcept = default;
            AllocSummaryInfo(_In_ const MemoryInfo& info) noexcept
            {
                m_TotalBytes += info.m_Bytes;
                ++m_TotalAllocations;
                m_OldestAllocation = (std::min)(m_OldestAllocation, info.m_Timestamp);
                m_NewestAllocation = (std::max)(m_NewestAllocation, info.m_Timestamp);
            }

            AllocSummaryInfo& operator<<(_In_ const AllocSummaryInfo& other) noexcept
            {
                m_TotalBytes += other.m_TotalBytes;
                m_TotalAllocations += other.m_TotalAllocations;
                m_OldestAllocation = (std::min)(m_OldestAllocation, other.m_OldestAllocation);
                m_NewestAllocation = (std::max)(m_NewestAllocation, other.m_NewestAllocation);

                return *this;
            }
        };

        using FullStackTracePtr = const StackTraceEntryArray*;
        struct FullStackTracePtrLess
        {
            [[nodiscard]] constexpr bool operator()(
                _In_ const FullStackTracePtr pLhs,
                _In_ const FullStackTracePtr pRhs) const noexcept
            {
                return *pLhs < *pRhs;
            }
        };

        using FullStackTracePtrToAllocSummaryMapPair = std::pair<const FullStackTracePtr, AllocSummaryInfo>;
        using FullStackTracePtrToAllocSummaryMap = std::map<
            FullStackTracePtr,
            AllocSummaryInfo,
            FullStackTracePtrLess,
            SelfAllocator<FullStackTracePtrToAllocSummaryMapPair>>;

        using FullStackTracePtrToAllocSummaryMapPairVector = 
            std::vector<FullStackTracePtrToAllocSummaryMapPair, SelfAllocator<FullStackTracePtrToAllocSummaryMapPair>>;

        struct [[nodiscard]] STEBucketInfo
        {
            AllocSummaryInfo m_OverallSummaryInfo;
            FullStackTracePtrToAllocSummaryMap m_FullStackTracePtrToAllocSummaryMap;

            [[nodiscard]] auto GetFullStackTracesSortedByBytesDescending() const
            {
                using ElemT = std::pair<
                    std::remove_const_t<typename FullStackTracePtrToAllocSummaryMapPair::first_type>,
                    typename FullStackTracePtrToAllocSummaryMapPair::second_type>;
                std::vector<ElemT, SelfAllocator<ElemT>> ret{
                    m_FullStackTracePtrToAllocSummaryMap.cbegin(),
                    m_FullStackTracePtrToAllocSummaryMap.cend()};

                auto SortByAllocSummaryBytes = [](
                    const FullStackTracePtrToAllocSummaryMapPair& lhs,
                    const FullStackTracePtrToAllocSummaryMapPair& rhs)
                {
                    return lhs.second.m_TotalBytes > rhs.second.m_TotalBytes;
                };

                std::ranges::sort(ret, SortByAllocSummaryBytes);

                return ret;
            }
        };

        using MapKey = std::stacktrace_entry;
        using MapValue = STEBucketInfo;
        using StackTraceEntryToMemoryInfoSetMap = std::unordered_map<
            MapKey,
            MapValue,
            std::hash<MapKey>,
            std::equal_to<MapKey>,
            SelfAllocator<std::pair<const MapKey, MapValue>>>;

        StackTraceEntryToMemoryInfoSetMap buckets;
        for (const MemoryInfo& info : infoPkg.m_MemoryInfoSet)
        {
            auto& [overallSummaryInfo, fullStackToAllocSummaryMap] = buckets[FindFirstUserStackTraceEntryByDescription(infoPkg, info)];
            overallSummaryInfo << info;

            if constexpr (Type == LogSummaryType::FullStackTraces)
            {
                [[maybe_unused]] const auto [fullStackMapItr, bFSTASMEmplaced] {fullStackToAllocSummaryMap.emplace(&info.m_StackTrace, AllocSummaryInfo{})};
                fullStackMapItr->second << info;
            }
        }

        // Now that it's all bucketed, put each bucket in a vector and sort by overall bytes alloc'd, descending.
        using STEBucket = std::stacktrace_entry;
        using BucketValue = STEBucketInfo;
        using BucketPair = std::pair<STEBucket, BucketValue>;
        std::vector<BucketPair, SelfAllocator<BucketPair>> ret{buckets.begin(), buckets.end()};
        
        auto SortByBucketAllocBytesDescending = [](_In_ const BucketPair& lhs, _In_ const BucketPair& rhs)
        {
            return lhs.second.m_OverallSummaryInfo.m_TotalBytes > rhs.second.m_OverallSummaryInfo.m_TotalBytes;
        };
        std::ranges::sort(ret, SortByBucketAllocBytesDescending);

        return ret;
    }

    template <LogSummaryType Type>
    static void LogSummaryImpl(
        _In_ LogInformationPackage infoPkg,
        _In_ const LogCallback& logFn)
    {
        using namespace std::literals::string_view_literals;

        using namespace StringUtils::Fmt;
        using FmtByteUpToMebibyte = Memory::AutoConverting::Byte<Memory::UnitTags::Mebibyte>;
        using FmtByte = Memory::Fixed::Byte<>;
        using FmtDec = Numeric::Dec<>;

        using LogLines = std::basic_string<char, std::char_traits<char>, NonTrackingAllocator<char>>;
        LogLines logLines;

        if constexpr (Type != LogSummaryType::Limited)
        {
            const auto sortedAllocationInfoGroupByCommonStackTraceEntries{GenerateStackTraceEntryBucketedMapOfMemoryInfoPtrSets<Type>(infoPkg)};
            for (const auto& [steBucket, bucketInfo] : sortedAllocationInfoGroupByCommonStackTraceEntries)
            {
                const auto steBucketAllocSummaryInfoPair{std::make_pair(&steBucket, &bucketInfo.m_OverallSummaryInfo)};
                logLines += "\n\n--------------------------------------------------"sv;
                logLines += FormatSTEAllocationSummaryInfoPair(steBucketAllocSummaryInfoPair);
                if constexpr (Type == LogSummaryType::FullStackTraces)
                {
                    for (const auto& [pFullStackTrace, stackTraceAllocSummaryInfo] : bucketInfo.GetFullStackTracesSortedByBytesDescending())
                    {
                        const auto fullStackAllocSummaryInfoPair{std::make_pair(pFullStackTrace, &stackTraceAllocSummaryInfo)};
                        logLines += "\n~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~"sv;
                        logLines += FormatFullStackTraceAllocationSummaryInfoPair(fullStackAllocSummaryInfoPair);
                    }
                }
            }
        }

        logLines += "\n\n=================================================="sv;

        auto BytesPerAllocationAverage = [](_In_ const auto bytes, _In_ const auto allocs)
        {
            if (allocs == 0) { return std::size_t{0}; }
            return static_cast<std::size_t>(std::ceil(static_cast<double>(bytes) / static_cast<double>(allocs)));
        };

        const auto extAllocCount{g_ExternalAllocations.load()};
        const auto extAllocBytes{g_ExternalAllocationBytes.load()};
        const auto trackerAllocCount{g_InternalAllocations.load()};
        const auto trackerAllocBytes{g_InternalAllocationBytes.load()};
        logLines += std::format("\n\n  Total External[{} : {} ({})]\n  Total Tracker[{} : {} ({}) (~{}/ExtAlloc)]\n",
            FmtDec{}(extAllocCount), FmtByteUpToMebibyte{}(extAllocBytes), FmtByte{}(extAllocBytes),
            FmtDec{}(trackerAllocCount), FmtByteUpToMebibyte{}(trackerAllocBytes), FmtByte{}(trackerAllocBytes),
            FmtByte{}(BytesPerAllocationAverage(trackerAllocBytes, extAllocCount)));

        logLines += "\n\n==================================================\n\n"sv;

        logFn(logLines);
    }

    void GlobalTracker::AddExternalStackEntryMarkerUnsafe(_In_ const std::string_view markerSV)
    {
        m_ExternalUserStackTraceEntryMarkers.emplace_back(markerSV);
    }

    void GlobalTracker::ProcessAllocationUnsafe(_Inout_ MemoryInfo&& info)
    {
        //
        // Note:
        // 
        //  We may receive an allocation for an address we are currently tracking in some cases
        //  (e.g., missed a free from some DLL's THREAD_DETACH code for an allocation that was
        //  done during that DLL's THREAD_ATTACH).
        // 
        //  Reallocs may also end up here naturally, where an alloc was expanded/shrunk in-place.
        //
        //  Go ahead and attempt to remove the addr from our tracked set before inserting this most
        //  recent one.
        //
        ProcessDeallocationUnsafe(info);

        const auto [itr, bEmplaced] {m_MemoryInfoSet.insert(std::move(info))};
        if (!bEmplaced) [[unlikely]]
        {
            __debugbreak();
        }
    }

    void GlobalTracker::ProcessDeallocationUnsafe(_In_ const MemoryInfo& info)
    {
        /**
        const auto cachedInfoMapItr = m_AllocationAddrToCachedInfoMap.find(info.m_pMem);
        if (cachedInfoMapItr == m_AllocationAddrToCachedInfoMap.end())
        {
            // Must be a free for an allocation that happened prior
            // to tracking being enabled for the allocating thread.
            return;
        }

        const auto& [hashMapKey, infoSetItr] = cachedInfoMapItr->second;
        const auto hashMapItr = m_StackTraceEntryToMemoryInfoSetMap.find(hashMapKey);
        if (hashMapItr != m_StackTraceEntryToMemoryInfoSetMap.end())
        {
            if (hashMapItr->second.size() <= 1)
            {
                m_StackTraceEntryToMemoryInfoSetMap.erase(hashMapItr);
            }
            else
            {
                hashMapItr->second.erase(infoSetItr);
            }
        }
        m_AllocationAddrToCachedInfoMap.erase(cachedInfoMapItr);
        /**/

        m_MemoryInfoSet.erase(info);
    }

    void GlobalTracker::Init()
    {
        s_pTracker = std::make_shared<GlobalTracker>();
    }

    void GlobalTracker::DeInit()
    {
        s_pTracker.reset();
    }

    [[nodiscard]] std::shared_ptr<GlobalTracker> GlobalTracker::Instance() noexcept
    {
        return s_pTracker;
    }

    [[nodiscard]] std::shared_ptr<GlobalTracker> GlobalTracker::InstanceIfTrackingEnabled() noexcept
    {
        if (gtl_IsInternalAllocationOrFree > 0)
        {
            // Internal allocation stats are handled directly in the detour functions.
            return nullptr;
        }

        if (gtl_IsNoTrackAllocationOrFree > 0)
        {
            // We're not tracking this allocation.
            return nullptr;
        }

        if (!g_bTrackingEnabled)
        {
            return nullptr;
        }

        // Track in all other cases.
        return Instance();
    }

    void GlobalTracker::SetTargetModuleNamePrefix(_In_ const std::string_view prefixSV)
    {
        auto scopedLock{m_TrackerLock.AcquireScoped()};
        m_TargetModuleNamePrefix.assign(prefixSV.cbegin(), prefixSV.cend());
    }

    void GlobalTracker::AddExternalStackEntryMarker(_In_ const std::string_view markerSV)
    {
        auto scopedLock{m_TrackerLock.AcquireScoped()};
        AddExternalStackEntryMarkerUnsafe(std::string{markerSV});
    }

    void GlobalTracker::AddExternalStackEntryMarkers(_In_ const std::vector<std::string_view>& markers)
    {
        auto scopedLock{m_TrackerLock.AcquireScoped()};
        std::ranges::for_each(markers,
            [this](const auto markerSV) { AddExternalStackEntryMarkerUnsafe(std::string{markerSV}); });
    }

    void GlobalTracker::SetCollectFullStackTraces(_In_ const bool bCollect)
    {
        m_bCollectFullStackTraces = bCollect;
    }

    void GlobalTracker::RegisterThreadTracker(_In_ ThreadTracker* pTT)
    {
        m_WorkerThread.RegisterThreadTracker(pTT);
    }

    void GlobalTracker::DeregisterThreadTracker(_In_ ThreadTracker* pTT)
    {
        m_WorkerThread.DeregisterThreadTracker(pTT);
    }

    void GlobalTracker::TrackAllocation(_Inout_ MemoryInfo&& info)
    {
        if (!!(info.m_OpFlagMask & OpFlag::Free)) [[unlikely]]
        {
            __debugbreak();
        }

        g_ExternalAllocationBytes += info.m_Bytes;
        ++g_ExternalAllocations;

        using StdStackTraceT = StdStackTraceWithAllocatorT<SelfAllocator<std::stacktrace_entry>>;
        info.m_StackTrace = StdStackTraceT::current(1, s_cMaxStackTraceFrames);

        gtl_ThreadTracker.AddToQueue(std::move(info));
    }

    void GlobalTracker::TrackDeallocation(_Inout_ MemoryInfo&& info)
    {
        if (!(info.m_OpFlagMask & OpFlag::Free)) [[unlikely]]
        {
            __debugbreak();
        }

        g_ExternalAllocationBytes -= info.m_Bytes;
        --g_ExternalAllocations;

        gtl_ThreadTracker.AddToQueue(std::move(info));
    }

    void GlobalTracker::AddToDeregisteredFreeQueue(_Inout_ DeregisteredMemoryFreeInfo&& info)
    {
        m_WorkerThread.AddToDeregisteredFreeQueue(std::move(info));
    }

    void GlobalTracker::LogSummary(
        _In_ const LogCallback& logFn,
        _In_ const LogSummaryType type) const
    {
        using Type = LogSummaryType;

        ScopedNoTrackAllocationOrFreeSetter scopedNoTrack;
        switch (type)
        {
        case Type::Limited:
            LogSummaryImpl<Type::Limited>(CreateLogInformationPackage<true>(), logFn);
            break;

        case Type::Normal:
            LogSummaryImpl<Type::Normal>(CreateLogInformationPackage<false>(), logFn);
            break;

        case Type::FullStackTraces:
            LogSummaryImpl<Type::FullStackTraces>(CreateLogInformationPackage<false>(), logFn);
            break;

        default:
            __debugbreak();
        }
    }
}


namespace AllocationTracking
{
    void LogAllocations(
        _In_ const LogCallback& logFn,
        _In_ const LogSummaryType type)
    {
        const auto pTracker = GlobalTracker::Instance();
        if (!!pTracker)
        {
            pTracker->LogSummary(logFn, type);
        }
    }
}


namespace AllocationTracking
{
    void EnableTracking(_In_ const bool bEnabled)
    {
        g_bTrackingEnabled = bEnabled;
    }

    void SetTargetModuleNamePrefix(_In_ const std::string_view prefixSV)
    {
        auto pTracker = GlobalTracker::Instance();
        if (!!pTracker)
        {
            pTracker->SetTargetModuleNamePrefix(prefixSV);
        }
    }

    void RegisterExternalStackEntryMarker(_In_ const std::string_view markerSV)
    {
        auto pTracker = GlobalTracker::Instance();
        if (!!pTracker)
        {
            pTracker->AddExternalStackEntryMarker(markerSV);
        }
    }

    void RegisterExternalStackEntryMarkers(_In_ const std::vector<std::string_view>& markers)
    {
        auto pTracker = GlobalTracker::Instance();
        if (!!pTracker)
        {
            pTracker->AddExternalStackEntryMarkers(markers);
        }
    }

    void SetCollectFullStackTraces(_In_ const bool bCollect)
    {
        auto pTracker = GlobalTracker::Instance();
        if (!!pTracker)
        {
            pTracker->SetCollectFullStackTraces(bCollect);
        }
    }
}

std::shared_ptr<AllocationTracking::GlobalTracker> AllocationTracking::GlobalTracker::s_pTracker{nullptr};
