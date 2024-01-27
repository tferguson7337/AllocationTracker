#include "AllocationTracker.h"

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
#include <ranges>
#include <set>
#include <sstream>
#include <stacktrace>
#include <thread>
#include <unordered_map>

#include "StringUtils.h"


// Formatter AllocationTracking::AllocFlag enum mask.
template <>
struct std::formatter<AllocationTracking::AllocFlag> : public std::formatter<std::string, char>
{
    using BaseT = std::formatter<std::string, char>;

    template <typename FormatContext>
    constexpr auto format(_In_ const AllocationTracking::AllocFlag flagMask, _Inout_ FormatContext& fmtCtx) const
    {
        return BaseT::format(AllocationTracking::FlagMaskToString(flagMask), fmtCtx);
    }
};

namespace AllocationTracking
{
    template <typename ElemT>
    struct SelfAllocator : std::allocator<ElemT>
    {
        [[nodiscard]] _Ret_notnull_ _Post_writable_size_(elems) constexpr ElemT* allocate(_In_ const std::size_t elems) const
        {
            return static_cast<ElemT*>(PerformAllocation(AllocFlag::SelfAlloc, elems * sizeof(ElemT)));
        }

        constexpr void deallocate(_In_opt_ ElemT* ptr, _In_ const std::size_t elems) const
        {
            return PerformDeallocation(AllocFlag::SelfAlloc, ptr, elems * sizeof(ElemT));
        }
    };

    //
    // Provides a large, static, thread-local buffer.
    //
    // Use case:
    //  `std::basic_stacktrace::current` will construct an internal vector with ~512KB of storage.
    //  Since we need to get the stacktrace for each allocation, this gets time consuming and is expensive.
    //  This allocator will instead provide a pre-allocated buffer to avoid repeated allocations.
    //
    // Note:
    //  Caller will need to synchronize uses of this using the allocator's mutex.
    //
    template <typename ElemT>
    struct ThreadLocalStaticSelfAllocator : std::allocator<ElemT>
    {
        static constexpr std::size_t s_cBufferSize = (512 << 10); // 512KB
        inline static constinit thread_local std::array<std::uint8_t, s_cBufferSize> s_Buffer;

        [[nodiscard]] _Ret_notnull_ _Post_writable_size_(elems) constexpr ElemT* allocate(
            [[maybe_unused]] _In_ const std::size_t elems) const
        {
            return reinterpret_cast<ElemT*>(s_Buffer.data());
        }

        constexpr void deallocate(
            [[maybe_unused]] _In_opt_ ElemT* ptr,
            [[maybe_unused]] _In_ const std::size_t elems) const
        {
            // no-op
        }
    };
}

namespace AllocationTracking
{
    // We need our own trimmed down `std::basic_stacktrace`, since with MSVC it rather annoyingly overallocates
    // its internal std::vector with 0xFFFF capacity when you call `current`, but doesn't shrink_to_fit.
    // If we don't do this, every tracked allocation would take ~512KB
    /**/
    struct [[nodiscard]] StackTraceEntryArray
    {
        const std::stacktrace_entry* m_pArr;
        std::uint16_t m_Len;

        constexpr StackTraceEntryArray() noexcept :
            m_pArr{nullptr},
            m_Len{0}
        { }

        template <typename StackTraceT>
        StackTraceEntryArray(_In_ const StackTraceT& allocStackTrace) :
            StackTraceEntryArray{}
        {
            *this = allocStackTrace;
        }

        StackTraceEntryArray(const StackTraceEntryArray&) = delete;

        StackTraceEntryArray(_Inout_ StackTraceEntryArray&& other) noexcept :
            StackTraceEntryArray{}
        {
            *this = std::move(other);
        }

        ~StackTraceEntryArray()
        {
            Reset();
        }

        template <typename StackTraceT>
        StackTraceEntryArray& operator=(_In_ const StackTraceT& allocStackTrace) noexcept
        {
            Reset();

            //
            // Note:
            //
            //  For full stack traces on Windows, the two bottom frames are usually OS thread-start calls:
            //  E.g., for a stack trace contain N frames
            //      N-2> KERNEL32!BaseThreadInitThunk+0x1D
            //      N-1> ntdll!RtlUserThreadStart+0x28
            //
            //  Skip these frames to save some overhead.
            //

            using AllocType = std::stacktrace_entry;

            const auto fullLen{std::distance(allocStackTrace.cbegin(), allocStackTrace.cend())};
            const auto copyLen = (fullLen <= 2) ? fullLen : (fullLen - 2);
            std::unique_ptr<AllocType[]> p{static_cast<AllocType*>(PerformAllocation(AllocFlag::SelfAlloc, sizeof(AllocType) * copyLen))};
            std::uninitialized_copy_n(allocStackTrace.cbegin(), copyLen, p.get());

            m_pArr = p.release();
            m_Len = static_cast<std::uint16_t>(copyLen);

            return *this;
        }

        StackTraceEntryArray& operator=(const StackTraceEntryArray&) = delete;

        StackTraceEntryArray& operator=(_Inout_ StackTraceEntryArray&& other) noexcept
        {
            if (this != &other)
            {
                Reset();

                m_pArr = other.m_pArr;
                m_Len = other.m_Len;

                other.m_pArr = nullptr;
                other.m_Len = 0;
            }

            return *this;
        }

        void Reset() noexcept
        {
            if (!!m_pArr)
            {
                std::destroy(m_pArr, m_pArr + m_Len);
                PerformDeallocation(AllocFlag::SelfAlloc, (void*)m_pArr, sizeof(std::stacktrace_entry) * m_Len);
                m_pArr = nullptr;
                m_Len = 0;
            }
        }

        constexpr auto ToSpan() const noexcept
        {
            return std::span{m_pArr, m_Len};
        }
    };
}

template <>
struct std::formatter<AllocationTracking::StackTraceEntryArray> : public std::formatter<std::basic_string<char>, char>
{
    template <typename FormatContext>
    constexpr auto format(_In_ const AllocationTracking::StackTraceEntryArray& stackTrace, _Inout_ FormatContext& fmtCtx) const
    {
        std::stringstream ss;

        std::size_t frameNum = 0;
        for (const std::stacktrace_entry ste : stackTrace.ToSpan())
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


namespace AllocationTracking
{
    thread_local std::int64_t gtl_TrackingDisabledCount{0};

    ScopedThreadLocalTrackingDisabler::ScopedThreadLocalTrackingDisabler()
    {
        ++gtl_TrackingDisabledCount;
    }

    ScopedThreadLocalTrackingDisabler::~ScopedThreadLocalTrackingDisabler()
    {
        --gtl_TrackingDisabledCount;
    }

    std::atomic<std::int64_t> g_TrackingDisabledCount{0};

    ScopedGlobalTrackingDisabler::ScopedGlobalTrackingDisabler()
    {
        ++g_TrackingDisabledCount;
    }

    ScopedGlobalTrackingDisabler::~ScopedGlobalTrackingDisabler()
    {
        --g_TrackingDisabledCount;
    }

#pragma pack(push, 1)
    struct [[nodiscard]] MemoryInfo
    {
        using Clock = std::chrono::system_clock;
        using TimePoint = decltype(Clock::now());

        TimePoint m_Timestamp{Clock::now()};
        void* m_pMem{nullptr};
        std::size_t m_ByteCount{0};
        StackTraceEntryArray m_StackTrace{};
        std::uint8_t m_Alignment{__STDCPP_DEFAULT_NEW_ALIGNMENT__};
        AllocFlag m_FlagMask{AllocFlag::None};

        [[nodiscard]] constexpr bool operator<(_In_ const MemoryInfo& other) const noexcept
        {
            return m_pMem < other.m_pMem;
        }
    };
#pragma pack(pop)

    class Tracker
    {
    private:

        mutable std::mutex m_TrackerMutex;

        class WorkerThread
        {
            friend class Tracker;

        private:

            using Queue = std::deque<MemoryInfo, SelfAllocator<MemoryInfo>>;

            Queue m_Queue{};
            std::mutex m_QueueMutex{};
            std::condition_variable m_QueueCV{};
            std::atomic<std::size_t> m_QueueLength{0};
            std::atomic<bool> m_bContinueWork{true};

            // Note: It's important that this is last
            std::jthread m_Thread;

            Queue WaitForWork()
            {
                std::unique_lock lock{m_QueueMutex};
                m_QueueCV.notify_all();
                m_QueueCV.wait_for(lock, std::chrono::seconds(1));
                Queue tmp;
                std::swap(tmp, m_Queue);
                return tmp;
            }

            void WorkerLoop()
            {
                while (m_bContinueWork.load())
                {
                    auto work{WaitForWork()};
                    if (work.empty())
                    {
                        continue;
                    }

                    const auto pTracker{Tracker::Instance()};
                    if (!pTracker)
                    {
                        continue;
                    }

                    std::unique_lock lock{pTracker->m_TrackerMutex};
                    for (auto& info : work)
                    {
                        if (!m_bContinueWork.load())
                        {
                            return;
                        }

                        pTracker->ProcessMemoryInfoUnsafe(std::move(info));
                        --m_QueueLength;
                    }
                }
            }

            void EnqueueAndNotifyUnsafe(_Inout_ MemoryInfo&& item)
            {
                using Clock = std::chrono::steady_clock;
                static constinit std::uint64_t s_BytesInFlightSinceLastNotify = 0;
                static constinit std::uint64_t s_ItemsInFlightSinceLastNotify = 0;
                static auto s_LastNotifyTime = Clock::now();

                const auto now{Clock::now()};

                s_BytesInFlightSinceLastNotify += item.m_ByteCount;
                ++s_ItemsInFlightSinceLastNotify;
                m_Queue.push_back(std::move(item));
                ++m_QueueLength;

                if ((s_BytesInFlightSinceLastNotify >= (1 << 20)) ||
                    (s_ItemsInFlightSinceLastNotify > 1000) ||
                    ((s_LastNotifyTime - now) > std::chrono::seconds(1)))
                {
                    s_BytesInFlightSinceLastNotify = 0;
                    s_ItemsInFlightSinceLastNotify = 0;
                    s_LastNotifyTime = now;
                    m_QueueCV.notify_one();
                }
            }

        public:

            WorkerThread() :
                m_Thread{[this]() { WorkerLoop(); }}
            { }

            ~WorkerThread()
            {
                SignalStop(false);
            }

            void Start()
            {
                SignalStop(true);
                m_bContinueWork = true;
                m_Thread = std::jthread{[this]() { WorkerLoop(); }};
            }

            void SignalStop(_In_ const bool bWait)
            {
                m_bContinueWork = false;
                m_QueueCV.notify_one();
                if (bWait && m_Thread.joinable())
                {
                    m_Thread.join();
                }
            }

            void Enqueue(_Inout_ MemoryInfo&& item)
            {
                std::lock_guard lock{m_QueueMutex};
                EnqueueAndNotifyUnsafe(std::move(item));
            }
        } m_WorkerThread;

    public:

#if defined _DEBUG
        //
        // This is needed for _DEBUG, since MSVC's std::stacktrace appears to perform debug-related
        // allocations during default construction.  W/o this, we crash due to infinite recursion on
        // the next allocation once tracking is enabled.
        //
        // Rather inconvenient.
        //
        using AllocStackTrace = std::basic_stacktrace<NonTrackingAllocator<std::stacktrace_entry>>;
#else
        using AllocStackTrace = std::basic_stacktrace<ThreadLocalStaticSelfAllocator<std::stacktrace_entry>>;
#endif

    private:

        using MemoryInfoSet = std::set<MemoryInfo, std::less<>, SelfAllocator<MemoryInfo>>;
        using StackTraceEntryToMemoryInfoSetMap = std::unordered_map<std::stacktrace_entry, MemoryInfoSet, std::hash<std::stacktrace_entry>, std::equal_to<std::stacktrace_entry>, SelfAllocator<std::pair<const std::stacktrace_entry, MemoryInfoSet>>>;
        StackTraceEntryToMemoryInfoSetMap m_StackTraceEntryToMemoryInfoSetMap;

        using CachedInfo = std::pair<std::stacktrace_entry, MemoryInfoSet::const_iterator>;
        using AllocationAddrToCachedInfoMap = std::map<void*, CachedInfo, std::less<>, SelfAllocator<std::pair<void* const, CachedInfo>>>;
        AllocationAddrToCachedInfoMap m_AllocationAddrToCachedInfoMap;

        using ExternalUserStackTraceEntryMarkers = std::vector<std::string, SelfAllocator<std::string>>;
        ExternalUserStackTraceEntryMarkers m_ExternalUserStackTraceEntryMarkers;

        std::atomic<std::size_t> m_InternalAllocationByteCount{0};
        std::atomic<std::size_t> m_InternalAllocationCount{0};

        std::atomic<bool> m_bCollectFullStackTraces{true};

        struct AllocSummaryInfo
        {
            std::size_t m_TotalBytes{0};
            std::size_t m_TotalAllocations{0};

            AllocFlag m_FlagMask{AllocFlag::None};
            MemoryInfo::TimePoint m_OldestAllocation{(MemoryInfo::TimePoint::max)()};
            MemoryInfo::TimePoint m_NewestAllocation{(MemoryInfo::TimePoint::min)()};

            struct FullStackTracePtrLess
            {
                [[nodiscard]] bool operator()(
                    _In_ const StackTraceEntryArray* pLhs,
                    _In_ const StackTraceEntryArray* pRhs) const noexcept
                {
                    const auto lhs = pLhs->ToSpan();
                    const auto rhs = pRhs->ToSpan();
                    if (lhs.size() < rhs.size())
                    {
                        return true;
                    }
                    if (lhs.size() > rhs.size())
                    {
                        return false;
                    }

                    for (std::size_t idx = 0; idx < lhs.size(); ++idx)
                    {
                        if (!(lhs[idx] < rhs[idx]))
                        {
                            return false;
                        }
                    }

                    return true;
                }
            };

            using FullStackTracePtrSet = std::set<const StackTraceEntryArray*, FullStackTracePtrLess, SelfAllocator<const StackTraceEntryArray*>>;
            FullStackTracePtrSet m_FullStackTraces;

            AllocSummaryInfo() noexcept = default;

            AllocSummaryInfo(_In_ const MemoryInfo& info) noexcept
            {
                m_TotalBytes += info.m_ByteCount;
                ++m_TotalAllocations;
                m_FlagMask |= info.m_FlagMask;
                m_OldestAllocation = (std::min)(m_OldestAllocation, info.m_Timestamp);
                m_NewestAllocation = (std::max)(m_NewestAllocation, info.m_Timestamp);
            }

            AllocSummaryInfo& operator<<(_In_ const AllocSummaryInfo& other) noexcept
            {
                m_TotalBytes += other.m_TotalBytes;
                m_TotalAllocations += other.m_TotalAllocations;
                m_FlagMask |= other.m_FlagMask;
                m_OldestAllocation = (std::min)(m_OldestAllocation, other.m_OldestAllocation);
                m_NewestAllocation = (std::max)(m_NewestAllocation, other.m_NewestAllocation);

                return *this;
            }
        };

        [[nodiscard]] static std::string FormatAllocationSummaryInfo(_In_ const auto pairView)
        {
            using namespace StringUtils::Fmt;
            using FmtByteUpToMebibyte = Memory::AutoConverting::Byte<Memory::UnitTags::Mebibyte>;
            using FmtDec = Numeric::Dec<>;

            auto TimestampToLocalTime = [](const auto timestamp) -> decltype(std::chrono::current_zone()->to_local(timestamp))
            {
                const auto pCurrentZone = std::chrono::current_zone();
                if (!pCurrentZone)
                {
                    return {};
                }

                try
                {
                    return std::chrono::current_zone()->to_local(timestamp);
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
  Metadata
    Flags[{5:}]
    Oldest[{6:%m}/{6:%d}/{6:%Y} {6:%T}]
    Newest[{7:%m}/{7:%d}/{7:%Y} {7:%T}]
)Fmt";
            const auto& [stackTraceEntry, allocSummaryInfo] = *pairView;
            return std::format(
                s_cSummaryInfoFormatStr,
                stackTraceEntry.description(),
                stackTraceEntry.source_file(), FmtDec{}(stackTraceEntry.source_line()),
                FmtDec{}(allocSummaryInfo.m_TotalAllocations), FmtByteUpToMebibyte{}(allocSummaryInfo.m_TotalBytes),
                allocSummaryInfo.m_FlagMask,
                TimestampToLocalTime(allocSummaryInfo.m_OldestAllocation),
                TimestampToLocalTime(allocSummaryInfo.m_NewestAllocation));
        }

        // Logs each unique alloc-stacktrace, with total bytes each trace has allocated.
        // Also logs total allocations, total bytes overall, and tracking overhead bytes.
        void LogSummaryUnsafe(
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
                    const auto& [steKey, allocPackageSet] = pair;
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

            const auto trackerAllocCount = m_InternalAllocationCount.load();
            const auto trackerByteCount = m_InternalAllocationByteCount.load();
            logLines += std::format("\n\n  Total External[{} : {} ({})]\n  Total Tracker[{} : {} ({}) (~{}/ExtAlloc)]\n  Tracker Queue[{}]",
                FmtDec{}(overallAllocSummaryInfo.m_TotalAllocations),
                FmtByteUpToMebibyte{}(overallAllocSummaryInfo.m_TotalBytes), FmtByte{}(overallAllocSummaryInfo.m_TotalBytes),
                FmtDec{}(trackerAllocCount),
                FmtByteUpToMebibyte{}(trackerByteCount), FmtByte{}(trackerByteCount),
                FmtByte{}(BytesPerAllocationAverage(trackerByteCount, overallAllocSummaryInfo.m_TotalAllocations)),
                m_WorkerThread.m_QueueLength.load());

            logLines += "\n\n==================================================\n\n"sv;

            logFn(logLines);
        }

        void AddExternalStackEntryMarkerUnsafe(_In_ std::string marker)
        {
            m_ExternalUserStackTraceEntryMarkers.push_back(std::move(marker));
        }

        [[nodiscard]] StackTraceEntryToMemoryInfoSetMap::iterator FindSTEHashMapElement(_In_ const MemoryInfo& info)
        {
            for (const auto entry : info.m_StackTrace.ToSpan())
            {
                const auto itr{m_StackTraceEntryToMemoryInfoSetMap.find(entry)};
                if (itr != m_StackTraceEntryToMemoryInfoSetMap.end())
                {
                    return itr;
                }
            }

            return m_StackTraceEntryToMemoryInfoSetMap.end();
        }

        [[nodiscard]] bool IsExternalStackTraceEntryByDescriptionUnsafe(_In_ const std::stacktrace_entry entry) const
        {
            // We need to allocate for the STE description, so we'll disable tracking
            // for the temporary alloc to avoid crashing due to re-entry.
            ScopedThreadLocalTrackingDisabler disabler;

            using namespace std::literals::string_view_literals;
            static constexpr std::array s_cExternalStackTraceEntryMarkers
            {
                "!std::"sv,
                "!`std::"sv,
                "!__std"sv,
                "!AllocationTracking::"sv,
                "!operator new"sv,
                "!operator delete"sv
            };

            const std::string desc = entry.description();
            const std::string_view descSV{desc};

            if (std::ranges::any_of(
                s_cExternalStackTraceEntryMarkers,
                [descSV](_In_ const std::string_view marker) { return descSV.contains(marker); }))
            {
                return true;
            }

            return std::ranges::any_of(
                m_ExternalUserStackTraceEntryMarkers,
                [descSV](_In_ const std::string_view marker) { return descSV.contains(marker); });
        }

        [[nodiscard]] std::stacktrace_entry FindFirstNonExternalStackTraceEntryByDescriptionUnsafe(_In_ const MemoryInfo& info) const
        {
            const auto traceSpan{info.m_StackTrace.ToSpan()};
            const auto itr = std::ranges::find_if_not(traceSpan,
                [this](const auto entry) { return IsExternalStackTraceEntryByDescriptionUnsafe(entry); });
            return (itr == traceSpan.cend()) ? *traceSpan.cbegin() : *itr;
        }

        void ProcessMemoryInfoUnsafe(_In_ MemoryInfo&& info)
        {
            if (!!(info.m_FlagMask & AllocFlag::Free))
            {
                //
                // Note:
                //
                //  If `free` performance becomes an issue, we could speed this up by caching
                //  the STE (and potentially the MemoryInfoSet iterator) that each alloc corresponds to.
                //
                //  This would allow logarithmic/constant in `free` case, but require quite a bit extra memory overhead.
                //  For std::map that's 48/56 bytes per allocation.
                //
                const auto cachedInfoItr = m_AllocationAddrToCachedInfoMap.find(info.m_pMem);
                if (cachedInfoItr != m_AllocationAddrToCachedInfoMap.end())
                {
                    const auto [hashMapKey, infoSetItr] = cachedInfoItr->second;
                    m_StackTraceEntryToMemoryInfoSetMap[hashMapKey].erase(infoSetItr);
                    m_AllocationAddrToCachedInfoMap.erase(cachedInfoItr);
                }
                else
                {
                    // We didn't have the cached info.
                    // Not expected - exhaustively search the hash map.
                    __debugbreak();
                    for (auto& [steKey, allocPackageSet] : m_StackTraceEntryToMemoryInfoSetMap)
                    {
                        if (allocPackageSet.empty())
                        {
                            continue;
                        }

                        const auto setItr = allocPackageSet.find(info);
                        if (setItr == allocPackageSet.end())
                        {
                            continue;
                        }

                        allocPackageSet.erase(setItr);
                        return;
                    }
                }
            }
            else
            {
                const auto hashMapItr{FindSTEHashMapElement(info)};
                if (hashMapItr != m_StackTraceEntryToMemoryInfoSetMap.end())
                {
                    if (!m_bCollectFullStackTraces) { info.m_StackTrace.Reset(); }
                    void* const ptr = info.m_pMem;
                    const auto[infoSetItr, bEmplaceSuccess] = hashMapItr->second.emplace(std::move(info));
                    m_AllocationAddrToCachedInfoMap.emplace(ptr, CachedInfo{hashMapItr->first, infoSetItr});
                }
                else
                {
                    const auto key{FindFirstNonExternalStackTraceEntryByDescriptionUnsafe(info)};
                    if (!m_bCollectFullStackTraces) { info.m_StackTrace.Reset(); }
                    void* const ptr = info.m_pMem;
                    const auto[infoSetItr, bEmplaceSuccess] = m_StackTraceEntryToMemoryInfoSetMap[key].emplace(std::move(info));
                    m_AllocationAddrToCachedInfoMap.emplace(ptr, CachedInfo{key, infoSetItr});
                }
            }
        }

        static std::atomic<std::shared_ptr<Tracker>> s_pTracker;

    public:

        static void Init()
        {
            if (!s_pTracker.load())
            {
                std::shared_ptr<Tracker> expected{nullptr};
                s_pTracker.compare_exchange_strong(expected, std::make_shared<Tracker>());
            }
        }

        static void DeInit()
        {
            s_pTracker.store(nullptr);
        }

        [[nodiscard]] static std::shared_ptr<Tracker> Instance() noexcept
        {
            return s_pTracker.load();
        }

        [[nodiscard]] static std::shared_ptr<Tracker> InstanceIfTrackingEnabled(_In_ const AllocFlag flags = {}) noexcept
        {
            if (!!(flags & AllocFlag::SelfAlloc))
            {
                // Always track self-allocations.
                return Instance();
            }

            if (!!(flags & AllocFlag::NoTracking))
            {
                // We're not tracking this allocation.
                return nullptr;
            }

            if (gtl_TrackingDisabledCount > 0)
            {
                // Tracking has been disabled for this thread.
                return nullptr;
            }

            if (g_TrackingDisabledCount > 0)
            {
                // Tracking has been globally disabled for all threads.
                return nullptr;
            }

            // Track in all other cases.
            return Instance();
        }

        __declspec(noinline) void Track(_In_ MemoryInfo info)
        {
            if (!!(info.m_FlagMask & AllocFlag::SelfAlloc))
            {
                if (info.m_ByteCount == 0)
                {
                    throw std::logic_error("Internal AllocPackage is missing byte count");
                }

                if (!!(info.m_FlagMask & AllocFlag::Free))
                {
                    m_InternalAllocationByteCount -= info.m_ByteCount;
                    --m_InternalAllocationCount;
                }
                else
                {
                    m_InternalAllocationByteCount += info.m_ByteCount;
                    ++m_InternalAllocationCount;
                }

                return;
            }

            if (!(info.m_FlagMask & AllocFlag::Free))
            {
                info.m_StackTrace = AllocStackTrace::current(1);
            }

            m_WorkerThread.Enqueue(std::move(info));
        }

        void AddExternalStackEntryMarker(_In_ const std::string_view markerSV)
        {
            std::unique_lock lock{m_TrackerMutex};
            AddExternalStackEntryMarkerUnsafe(std::string{markerSV});
        }

        void AddExternalStackEntryMarkers(_In_ const std::vector<std::string_view>& markers)
        {
            std::unique_lock lock{m_TrackerMutex};
            std::ranges::for_each(markers, [this](const auto markerSV) { AddExternalStackEntryMarkerUnsafe(std::string{markerSV}); });
        }

        void CollectFullStackTraces(_In_ const bool bCollect)
        {
            m_bCollectFullStackTraces = bCollect;
        }

        void LogSummary(
            _In_ const LogCallback& logFn,
            _In_ const LogSummaryType type) const
        {
            std::unique_lock lock{m_TrackerMutex};
            ScopedThreadLocalTrackingDisabler stltd;
            LogSummaryUnsafe(logFn, type);
        }

        void WaitForQueueEmpty()
        {
            std::unique_lock lock{m_WorkerThread.m_QueueMutex};
            m_WorkerThread.m_QueueCV.wait(lock, [this]() { return m_WorkerThread.m_QueueLength == 0; });
        }
    };

    ScopedTrackerInit::ScopedTrackerInit()
    {
        Tracker::Init();
    }

    ScopedTrackerInit::~ScopedTrackerInit()
    {
        Tracker::DeInit();
    }

    void RegisterExternalStackEntryMarker(_In_ const std::string_view markerSV)
    {
        auto pTracker = Tracker::Instance();
        if (!!pTracker)
        {
            pTracker->AddExternalStackEntryMarker(markerSV);
        }
    }

    void RegisterExternalStackEntryMarkers(_In_ const std::vector<std::string_view>& markers)
    {
        auto pTracker = Tracker::Instance();
        if (!!pTracker)
        {
            pTracker->AddExternalStackEntryMarkers(markers);
        }
    }

    void CollectFullStackTraces(_In_ const bool bCollect)
    {
        auto pTracker = Tracker::Instance();
        if (!!pTracker)
        {
            pTracker->CollectFullStackTraces(bCollect);
        }
    }

    template <typename ArgT>
    concept ValidIsPowerOfTwoArg = std::integral<ArgT> || std::is_enum_v<ArgT>;

    template <ValidIsPowerOfTwoArg ArgT>
    [[nodiscard]] constexpr bool IsPowerOfTwo(_In_ const ArgT val)
    {
        if constexpr (std::is_enum_v<ArgT>)
        {
            return IsPowerOfTwo(static_cast<std::underlying_type_t<ArgT>>(val));
        }
        else if constexpr (std::signed_integral<ArgT>)
        {
            return IsPowerOfTwo(static_cast<std::make_unsigned_t<ArgT>>(val));
        }
        else
        {
            return ((val) & (val - 1)) == 0;
        }
    }

    [[nodiscard]] void* PerformAllocation(
        _In_ const AllocFlag flags,
        _In_ const std::size_t byteCount,
        _In_ std::align_val_t alignment /* = std::align_val_t{__STDCPP_DEFAULT_NEW_ALIGNMENT__} */)
    {
        // 1. Don't allow zero-size alloc
        // 2. _aligned_malloc requires alignment to be power of 2
        // 3. We store alignment as uint16_t, ensure provided arg meets constraint.
        const bool bCanThrow = !(flags & AllocFlag::NoThrow);
        if ((byteCount == 0) ||
            !IsPowerOfTwo(alignment) ||
            (static_cast<std::size_t>(alignment) > (std::numeric_limits<std::uint16_t>::max)()))
        {
            if (bCanThrow)
            {
                throw std::bad_alloc{};
            }

            return nullptr;
        }

        static constexpr auto s_cMaxU8{(std::numeric_limits<std::uint8_t>::max)()};
        const auto alignmentU8 = static_cast<std::uint8_t>(
            (std::max)(
                static_cast<std::size_t>(alignment),
                static_cast<std::size_t>(s_cMaxU8)));

        void* const pMem = !!(flags & AllocFlag::CustomAlignment)
            ? _aligned_malloc(byteCount, alignmentU8)
            : std::malloc(byteCount);
        if (!pMem)
        {
            if (bCanThrow)
            {
                throw std::bad_alloc{};
            }

            return nullptr;
        }

        const auto pTracker = Tracker::InstanceIfTrackingEnabled(flags);
        if (!!pTracker)
        {
            pTracker->Track(
                MemoryInfo{
                    .m_pMem = pMem,
                    .m_ByteCount = byteCount,
                    .m_Alignment = alignmentU8,
                    .m_FlagMask = flags});
        }

        return pMem;
    }

    void PerformDeallocation(
        _In_ const AllocFlag flags,
        _In_opt_ void* pMem,
        _In_ const std::size_t byteCount /* = 0 */) noexcept
    {
        if (!pMem)
        {
            return;
        }

        const auto pTracker = Tracker::InstanceIfTrackingEnabled(flags);
        if (!!pTracker)
        {
            pTracker->Track(
                MemoryInfo{
                    .m_pMem = pMem,
                    .m_ByteCount = byteCount,
                    .m_FlagMask = (flags | AllocFlag::Free)});
        }

        !!(flags & AllocFlag::CustomAlignment) ? _aligned_free(pMem) : free(pMem);
    }

    void LogAllocations(
        _In_ const LogCallback& logFn,
        _In_ const LogSummaryType type,
        _In_ const bool bWaitForQueueEmpty /* = false */)
    {
        const auto pTracker = Tracker::Instance();
        if (!pTracker)
        {
            return;
        }

        if (bWaitForQueueEmpty)
        {
            pTracker->WaitForQueueEmpty();
        }
        pTracker->LogSummary(logFn, type);
    }
}

std::atomic<std::shared_ptr<AllocationTracking::Tracker>> AllocationTracking::Tracker::s_pTracker{nullptr};
