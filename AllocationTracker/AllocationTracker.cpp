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
            merged.merge(toMerge, SortById);
        });

        return merged;
    }

    void GlobalTracker::WorkerThread::WorkerLoop()
    {
        while (WaitForWork())
        {
            auto scopedWorkInProgressSL{m_WorkerThreadInProgressLock.AcquireScoped()};
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


// GlobalTracker::AllocSummaryInfo
namespace AllocationTracking
{
    GlobalTracker::AllocSummaryInfo::AllocSummaryInfo() noexcept = default;

    GlobalTracker::AllocSummaryInfo::AllocSummaryInfo(_In_ const MemoryInfo& info) noexcept
    {
        m_TotalBytes += info.m_Bytes;
        ++m_TotalAllocations;
        m_OldestAllocation = (std::min)(m_OldestAllocation, info.m_Timestamp);
        m_NewestAllocation = (std::max)(m_NewestAllocation, info.m_Timestamp);
    }

    GlobalTracker::AllocSummaryInfo& GlobalTracker::AllocSummaryInfo::operator<<(
        _In_ const GlobalTracker::AllocSummaryInfo& other) noexcept
    {
        m_TotalBytes += other.m_TotalBytes;
        m_TotalAllocations += other.m_TotalAllocations;
        m_OldestAllocation = (std::min)(m_OldestAllocation, other.m_OldestAllocation);
        m_NewestAllocation = (std::max)(m_NewestAllocation, other.m_NewestAllocation);

        return *this;
    }
}


// GlobalTracker
namespace AllocationTracking
{
    [[nodiscard]] static std::string FormatAllocationSummaryInfo(_In_ const auto pairView)
    {
        using namespace StringUtils::Fmt;
        using FmtByteUpToMebibyte = Memory::AutoConverting::Byte<Memory::UnitTags::Mebibyte>;
        using FmtDec = Numeric::Dec<>;

        auto TimestampToLocalTime = [](const auto timestamp) -> decltype(std::chrono::current_zone()->to_local(timestamp))
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
        };

        static constexpr auto s_cSummaryInfoFormatStr = R"Fmt(

  Location
    Function[{0:}]
    File[{1:}@{2:}]
  Allocations[{3:} : {4:}]
  Oldest[{5:%m}/{5:%d}/{5:%Y} {5:%T}]
  Newest[{6:%m}/{6:%d}/{6:%Y} {6:%T}]
)Fmt";
        const auto& [stackTraceEntry, allocSummaryInfo] = *pairView;
        return std::format(
            s_cSummaryInfoFormatStr,
            stackTraceEntry.description(),
            stackTraceEntry.source_file(), FmtDec{}(stackTraceEntry.source_line()),
            FmtDec{}(allocSummaryInfo.m_TotalAllocations), FmtByteUpToMebibyte{}(allocSummaryInfo.m_TotalBytes),
            TimestampToLocalTime(allocSummaryInfo.m_OldestAllocation),
            TimestampToLocalTime(allocSummaryInfo.m_NewestAllocation));
    }

    // Logs each unique alloc-stacktrace, with total bytes each trace has allocated.
    // Also logs total allocations, total bytes overall, and tracking overhead bytes.
    void GlobalTracker::LogSummaryUnsafe(
        _In_ const LogCallback& logFn,
        _In_ const LogSummaryType type) const
    {
        using namespace std::literals::string_view_literals;

        using namespace StringUtils::Fmt;
        using FmtByteUpToMebibyte = Memory::AutoConverting::Byte<Memory::UnitTags::Mebibyte>;
        using FmtByte = Memory::Fixed::Byte<>;
        using FmtDec = Numeric::Dec<>;

        AllocSummaryInfo overallAllocSummaryInfo;
        auto CountMetrics = [&overallAllocSummaryInfo](const std::pair<const std::stacktrace_entry, MemoryInfoSet>& pair)
        {
            const AllocSummaryInfo newInfo = [&set = pair.second]()
            {
                AllocSummaryInfo asi;
                for (const auto& memInfo : set) { asi << memInfo; }
                return asi;
            }();
            overallAllocSummaryInfo << newInfo;
            return newInfo;
        };

        using LogLines = std::basic_string<char, std::char_traits<char>, NonTrackingAllocator<char>>;
        LogLines logLines;

        if (type != LogSummaryType::Limited)
        {
            using MapKey = std::stacktrace_entry;
            using MapValue = AllocSummaryInfo;
            using MapPair = std::pair<const MapKey, MapValue>;
            using Map = std::map<MapKey, MapValue, std::less<MapKey>, NonTrackingAllocator<MapPair>>;
            Map steToTotalAllocMap;

            auto CountMetricsAndBuildMap = [&CountMetrics, &steToTotalAllocMap, type, this](const auto& pair)
            {
                const auto& [steKey, allocPackageSet] {pair};
                if (pair.second.empty())
                {
                    return;
                }

                auto& summaryInfo = steToTotalAllocMap[pair.first];
                summaryInfo << CountMetrics(pair);
                if (type == LogSummaryType::FullStackTraces)
                {
                    for (const auto& allocPkg : allocPackageSet)
                    {
                        summaryInfo.m_FullStackTraces.insert(&allocPkg.m_StackTrace);
                    }
                }
            };
            std::ranges::for_each(m_StackTraceEntryToMemoryInfoSetMap, CountMetricsAndBuildMap);

            struct MapPairView
            {
                const MapPair* m_pView{nullptr};

                constexpr MapPairView() noexcept = default;

                constexpr MapPairView(_In_ const MapPair& mapPair) noexcept :
                    m_pView{std::addressof(mapPair)}
                { }

                [[nodiscard]] bool operator>(_In_ const MapPairView other) const noexcept
                {
                    return m_pView->second.m_TotalBytes > other.m_pView->second.m_TotalBytes;
                }

                const MapPair& operator*() const noexcept
                {
                    return *m_pView;
                }

                const MapPair* operator->() const noexcept
                {
                    return m_pView;
                }
            };

            const auto sortedSummaryAllocViews = [&steToTotalAllocMap]()
            {
                std::vector<MapPairView, NonTrackingAllocator<MapPairView>> vec{steToTotalAllocMap.size()};
                std::ranges::copy(steToTotalAllocMap, vec.begin());
                std::ranges::sort(vec, std::greater<>{});
                return vec;
            }();
            for (const MapPairView pairView : sortedSummaryAllocViews)
            {
                logLines += "\n\n--------------------------------------------------"sv;
                logLines += FormatAllocationSummaryInfo(pairView);
                if (type == LogSummaryType::FullStackTraces)
                {
                    for (const auto pStackTrace : pairView->second.m_FullStackTraces)
                    {
                        logLines += "\n~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~"sv;
                        logLines += std::format("\n\n{}", *pStackTrace);
                    }
                }
            }
            logLines += "\n\n=================================================="sv;
        }
        else
        {
            std::ranges::for_each(m_StackTraceEntryToMemoryInfoSetMap, CountMetrics);
        }

        auto BytesPerAllocationAverage = [](_In_ const auto bytes, _In_ const auto allocs)
        {
            if (allocs == 0) { return std::size_t{0}; }
            return static_cast<std::size_t>(std::ceil(static_cast<double>(bytes) / static_cast<double>(allocs)));
        };

        if (type == LogSummaryType::Limited)
        {
            logLines += "\n\n=================================================="sv;
        }

        const auto trackerAllocCount{g_InternalAllocations.load()};
        const auto trackerByteCount{g_InternalAllocationBytes.load()};
        logLines += std::format("\n\n  Total External[{} : {} ({})]\n  Total Tracker[{} : {} ({}) (~{}/ExtAlloc)]\n",
            FmtDec{}(overallAllocSummaryInfo.m_TotalAllocations),
            FmtByteUpToMebibyte{}(overallAllocSummaryInfo.m_TotalBytes), FmtByte{}(overallAllocSummaryInfo.m_TotalBytes),
            FmtDec{}(trackerAllocCount),
            FmtByteUpToMebibyte{}(trackerByteCount), FmtByte{}(trackerByteCount),
            FmtByte{}(BytesPerAllocationAverage(trackerByteCount, overallAllocSummaryInfo.m_TotalAllocations)));

        logLines += "\n\n==================================================\n\n"sv;

        logFn(logLines);
    }

    void GlobalTracker::AddExternalStackEntryMarkerUnsafe(_In_ const std::string_view markerSV)
    {
        m_ExternalUserStackTraceEntryMarkers.emplace_back(markerSV);
    }

    [[nodiscard]] GlobalTracker::StackTraceEntryToMemoryInfoSetMap::iterator
        GlobalTracker::FindSTEHashMapElement(_In_ const MemoryInfo& info)
    {
        for (const auto& entry : info.m_StackTrace.ToSpan())
        {
            const auto itr{m_StackTraceEntryToMemoryInfoSetMap.find(entry)};
            if (itr != m_StackTraceEntryToMemoryInfoSetMap.end())
            {
                return itr;
            }
        }

        return m_StackTraceEntryToMemoryInfoSetMap.end();
    }

    /**/
    [[nodiscard]] bool GlobalTracker::IsExternalStackTraceEntryByDescriptionUnsafe(
            _In_ const std::stacktrace_entry entry) const
    {
        // We need to allocate for the STE description, so we'll disable tracking
        // for the temporary alloc to avoid re-entry issues.
        ScopedNoTrackAllocationOrFreeSetter scopedNoTrack;

        using namespace std::literals::string_view_literals;
        static constexpr std::array s_cExternalStackTraceEntryMarkers
        {
            "!std::"sv,
            "!`std::"sv,
            "!__std"sv,
            "!AllocationTracking::"sv,
            "!`AllocationTracking::"sv,
            "!operator new"sv,
            "!operator delete"sv,
            "!_malloc_base"sv,
            "!_realloc_base"sv,
            "!_calloc_base"sv,
            "!`anonymous namespace'::"sv,
        };

        const std::string desc{entry.description()};
        const std::string_view descSV{desc};

        if (std::ranges::any_of(
            s_cExternalStackTraceEntryMarkers,
            [descSV](_In_ const std::string_view marker) { return descSV.contains(marker); }))
        {
            return true;
        }

        if (std::ranges::any_of(
            m_ExternalUserStackTraceEntryMarkers,
            [descSV](_In_ const std::string_view marker) { return descSV.contains(marker); }))
        {
            return true;
        }

        if (!!m_TargetModuleNamePrefix.empty() ||
            !descSV.starts_with(m_TargetModuleNamePrefix))
        {
            return true;
        }

        return false;
    }

    [[nodiscard]] std::stacktrace_entry
        GlobalTracker::FindFirstNonExternalStackTraceEntryByDescriptionUnsafe(
            _In_ const MemoryInfo& info) const
    {
        const auto stSpan{info.m_StackTrace.ToSpan()};
        const auto itr{std::ranges::find_if_not(stSpan,
            [this](const auto entry) { return IsExternalStackTraceEntryByDescriptionUnsafe(entry); })};
        return (itr == stSpan.cend()) ? std::stacktrace_entry{} : *itr;
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
        _In_ const LogSummaryType type,
        _In_ const bool waitForWorkerThreadLull) const
    {
        if (waitForWorkerThreadLull)
        {
            // Temporary lock grab to wait for WorkerThread to finish in-progress batch of work.
            auto waitSpinScoped{m_WorkerThread.m_WorkerThreadInProgressLock.AcquireScoped()};
        }
        auto scopedLock{m_TrackerLock.AcquireScoped()};
        ScopedNoTrackAllocationOrFreeSetter scopedNoTrack;
        LogSummaryUnsafe(logFn, type);
    }
}


namespace AllocationTracking
{
    void LogAllocations(
        _In_ const LogCallback& logFn,
        _In_ const LogSummaryType type,
        _In_ const bool bWaitForWorkerThreadLull /* = false */)
    {
        const auto pTracker = GlobalTracker::Instance();
        if (!!pTracker)
        {
            pTracker->LogSummary(logFn, type, bWaitForWorkerThreadLull);
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
