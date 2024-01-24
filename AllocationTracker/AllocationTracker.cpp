#include "AllocationTracker.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <ranges>
#include <set>
#include <sstream>
#include <stacktrace>
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
        return BaseT::format(AllocationTracking::Metadata::FlagMaskToString(flagMask), fmtCtx);
    }
};

namespace AllocationTracking
{
    // We need our own trimmed down `std::basic_stacktrace`, since with MSVC it rather annoyingly overallocates
    // its internal std::vector with 0xFFFF capacity when you call `current`, but doesn't shrink_to_fit.
    // If we don't do this, every tracked allocation would take ~512KB
    struct [[nodiscard]] StackTraceEntryArray
    {
        const std::stacktrace_entry* m_pArr;
        std::size_t m_Len;

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

            m_Len = std::distance(allocStackTrace.cbegin(), allocStackTrace.cend());
            auto p = static_cast<std::stacktrace_entry*>(PerformAllocation(AllocFlag::SelfAlloc, sizeof(std::stacktrace_entry) * m_Len));
            std::uninitialized_copy(allocStackTrace.cbegin(), allocStackTrace.cend(), p);
            m_pArr = p;

            return *this;
        }

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
    std::atomic<std::int64_t> g_TrackingDisabledCount{0};

    ScopedTrackingDisabler::ScopedTrackingDisabler()
    {
        ++g_TrackingDisabledCount;
    }

    ScopedTrackingDisabler::~ScopedTrackingDisabler()
    {
        --g_TrackingDisabledCount;
    }

    struct AllocPackage : public Metadata
    {
        void* m_pMem{nullptr};
        std::size_t m_ByteCount{0};
        std::align_val_t m_Alignment{__STDCPP_DEFAULT_NEW_ALIGNMENT__};
        StackTraceEntryArray m_StackTrace{};

        [[nodiscard]] constexpr bool operator<(_In_ const AllocPackage& other) const noexcept
        {
            return m_pMem < other.m_pMem;
        }
    };

    struct DeallocPackage : public Metadata
    {
        void* m_pMem{nullptr};
        std::size_t m_ByteCount{0};

        friend bool operator<(_In_ const AllocPackage& allocPkg, _In_ const DeallocPackage& deallocPkg) noexcept
        {
            return allocPkg.m_pMem < deallocPkg.m_pMem;
        }

        friend bool operator<(_In_ const DeallocPackage& deallocPkg, _In_ const AllocPackage& allocPkg) noexcept
        {
            return deallocPkg.m_pMem < allocPkg.m_pMem;
        }
    };

    class Tracker
    {
    private:

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
        using AllocStackTrace = std::basic_stacktrace<SelfAllocator<std::stacktrace_entry>>;
#endif

    private:

        [[nodiscard]] bool IsExternalStackTraceEntry(_In_ const std::stacktrace_entry& entry) const
        {
            // We need to allocate for the STE description, so we'll disable tracking
            // for the temporary alloc to avoid crashing due to re-entry.
            ScopedTrackingDisabler disabler;

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

        [[nodiscard]] std::stacktrace_entry FindFirstNonExternalStackTraceEntry(_In_ const AllocPackage& pkg) const
        {
            const auto traceView{pkg.m_StackTrace.ToSpan()};
            const auto itr = std::ranges::find_if_not(traceView, [this](const auto& entry) { return IsExternalStackTraceEntry(entry); });
            return (itr == traceView.cend()) ? *traceView.cbegin() : *itr;
        }

        using AllocPackageSet = std::set<AllocPackage, std::less<>, SelfAllocator<AllocPackage>>;
        using StackTraceToAllocPackageSetMap = std::unordered_map<std::stacktrace_entry, AllocPackageSet, std::hash<std::stacktrace_entry>, std::equal_to<std::stacktrace_entry>, SelfAllocator<std::pair<const std::stacktrace_entry, AllocPackageSet>>>;
        StackTraceToAllocPackageSetMap m_StackTraceToAllocPackageSetMap;
        mutable std::recursive_mutex m_TrackerMutex;

        using AddrContainerCacheInfo = std::pair<std::stacktrace_entry, AllocPackageSet::const_iterator>;
        using AllocationAddressToContainerCacheInfoMap = std::map<void*, AddrContainerCacheInfo, std::less<>, SelfAllocator<std::pair<void* const, AddrContainerCacheInfo>>>;
        AllocationAddressToContainerCacheInfoMap m_AllocAddrToContainerCacheInfoMap;

        using ExternalUserStackTraceEntryMarkers = std::set<std::string, std::less<>, SelfAllocator<std::string>>;
        ExternalUserStackTraceEntryMarkers m_ExternalUserStackTraceEntryMarkers;

        std::atomic<std::size_t> m_InternalAllocationByteCount{0};
        std::atomic<std::size_t> m_InternalAllocationCount{0};

        void AddExternalStackEntryMarkerUnsafe(_In_ const std::string_view marker)
        {
            m_ExternalUserStackTraceEntryMarkers.emplace(marker);
        }

        [[nodiscard]] static constexpr std::size_t CalculateTotalBytesFromAllocPackageSet(_In_ const AllocPackageSet& set) noexcept
        {
            auto Accum = [](_In_ const std::size_t accum, const AllocPackage& pkg)
            {
                return accum + pkg.m_ByteCount;
            };

            return std::accumulate(set.cbegin(), set.cend(), static_cast<std::size_t>(0), Accum);
        }

        struct AllocSummaryInfo
        {
            std::size_t m_TotalBytes{0};
            std::size_t m_TotalAllocations{0};

            AllocFlag m_FlagMask{AllocFlag::None};
            Metadata::TimestampMs m_OldestAllocation{(Metadata::TimestampMs::max)()};
            Metadata::TimestampMs m_NewestAllocation{(Metadata::TimestampMs::min)()};

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

            AllocSummaryInfo(_In_ const AllocPackage& allocPackage) noexcept :
                m_TotalBytes{allocPackage.m_ByteCount},
                m_TotalAllocations{1},
                m_FlagMask{allocPackage.m_FlagMask},
                m_OldestAllocation{allocPackage.m_TimestampMs},
                m_NewestAllocation{allocPackage.m_TimestampMs}
            { }

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
            using FmtDec = Numeric::Dec<>;

            AllocSummaryInfo overallAllocSummaryInfo;
            auto CountMetrics = [&overallAllocSummaryInfo](const std::pair<const std::stacktrace_entry, AllocPackageSet>& pair)
            {
                const AllocSummaryInfo newInfo = [&set = pair.second]()
                {
                    AllocSummaryInfo asi;
                    for (const auto& allocPackage : set) { asi << allocPackage; }
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
                std::ranges::for_each(m_StackTraceToAllocPackageSetMap, CountMetricsAndBuildMap);

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
                        for (const auto pStackTrace : (*pairView).second.m_FullStackTraces)
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
                std::ranges::for_each(m_StackTraceToAllocPackageSetMap, CountMetrics);
            }

            logLines += std::format("\n\n  Total Allocations[{} : {}]\n  Total Internal[{} : {}]",
                FmtDec{}(overallAllocSummaryInfo.m_TotalAllocations), FmtByteUpToMebibyte{}(overallAllocSummaryInfo.m_TotalBytes),
                FmtDec{}(m_InternalAllocationCount.load()), FmtByteUpToMebibyte{}(m_InternalAllocationByteCount.load()));

            logLines += "\n\n==================================================\n\n"sv;

            logFn(logLines);
        }

        void RemoveTrackedAllocUnsafe(_In_ const DeallocPackage& pkg)
        {
            const auto addrToCacheInfoItr = m_AllocAddrToContainerCacheInfoMap.find(pkg.m_pMem);
            if (addrToCacheInfoItr == m_AllocAddrToContainerCacheInfoMap.end())
            {
                // We aren't tracking this memory.
                return;
            }

            const auto [cachedSTEKey, cachedAllocPackageSetItr] = addrToCacheInfoItr->second;
            m_AllocAddrToContainerCacheInfoMap.erase(addrToCacheInfoItr);

            const auto steToAllocPkgSetItr = m_StackTraceToAllocPackageSetMap.find(cachedSTEKey);
            if (steToAllocPkgSetItr != m_StackTraceToAllocPackageSetMap.end())
            {
                // Cache hit successful, remove corresponding entry in AllocPackageSet.
                AllocPackageSet& allocPkgSet = steToAllocPkgSetItr->second;
                if (allocPkgSet.size() == 1)
                {
                    // TODO?:
                    //  Consider not erasing this hash-map entry to avoid needing to
                    //  recreate it for hot code paths that have frequent short-lived allocs.
                    m_StackTraceToAllocPackageSetMap.erase(steToAllocPkgSetItr);
                }
                else
                {
                    allocPkgSet.erase(cachedAllocPackageSetItr);
                }
            }
            else
            {
                //
                // We have the address for the allocation cached, but couldn't find the AllocPackage via the cached STE.
                //
                // This is unexpected - fall back to linear search through the hash-map.
                //
                static bool bDebugBreakFlag = false;
                if (!bDebugBreakFlag)
                {
                    bDebugBreakFlag = true;
                    __debugbreak();
                }

                for (auto& [mapSTEKey, allocPackageSet] : m_StackTraceToAllocPackageSetMap)
                {
                    const auto setItr = allocPackageSet.find(pkg);
                    if (setItr != allocPackageSet.end())
                    {
                        allocPackageSet.erase(setItr);
                        return;
                    }
                }

                //
                // We failed to find this address at all, despite having it cached.
                // Extremely unexpected.
                //
                const std::string exceptionMsg =
                    std::format("Had cached address, but failed to find it in hash-map.\nAddr[{:p}]\nCached STE:\n{}",
                        pkg.m_pMem, cachedSTEKey);
                throw std::runtime_error(exceptionMsg);
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

        static std::shared_ptr<Tracker> Instance()
        {
            return s_pTracker;
        }

        static std::shared_ptr<Tracker> InstanceIfTrackingEnabled(_In_ const AllocFlag flags = {})
        {
            if (!!(flags & AllocFlag::SelfAlloc))
            {
                // Always track self-allocations.
                return s_pTracker.load();
            }

            if (!!(flags & AllocFlag::NoTracking))
            {
                // We're not tracking this allocation.
                return nullptr;
            }

            if (g_TrackingDisabledCount > 0)
            {
                // Tracking has been globally disabled.
                return nullptr;
            }

            // Track in all other cases.
            return s_pTracker.load();
        }

        __declspec(noinline) void Track(_In_ AllocPackage pkg)
        {
            if (pkg.IsSelfAlloc())
            {
                if (pkg.m_ByteCount == 0)
                {
                    throw std::logic_error("Internal AllocPackage is missing byte count");
                }

                m_InternalAllocationByteCount += pkg.m_ByteCount;
                ++m_InternalAllocationCount;
                return;
            }

            pkg.SetTimestamp();
            pkg.m_StackTrace = AllocStackTrace::current();
            const auto key{FindFirstNonExternalStackTraceEntry(pkg)};

            std::unique_lock lock{m_TrackerMutex};
            const auto[allocSetItr, bEmplaced] = m_StackTraceToAllocPackageSetMap[key].emplace(std::move(pkg));
            m_AllocAddrToContainerCacheInfoMap.emplace(pkg.m_pMem, AddrContainerCacheInfo{key, allocSetItr});
        }

        void Track(_In_ const DeallocPackage pkg)
        {
            if (pkg.IsSelfAlloc())
            {
                if (pkg.m_ByteCount == 0)
                {
                    throw std::logic_error("Internal DeallocPackage is missing byte count");
                }

                m_InternalAllocationByteCount -= pkg.m_ByteCount;
                --m_InternalAllocationCount;
                return;
            }

            std::unique_lock lock{m_TrackerMutex};
            RemoveTrackedAllocUnsafe(pkg);
        }

        void AddExternalStackEntryMarker(_In_ const std::string_view markerSV)
        {
            std::unique_lock lock{m_TrackerMutex};
            AddExternalStackEntryMarkerUnsafe(markerSV);
        }

        void AddExternalStackEntryMarkers(_In_ const std::vector<std::string_view>& markers)
        {
            std::unique_lock lock{m_TrackerMutex};
            std::ranges::for_each(markers, [this](const auto markerSV) { AddExternalStackEntryMarkerUnsafe(markerSV); });
        }

        void LogSummary(
            _In_ const LogCallback& logFn,
            _In_ const LogSummaryType type) const
        {
            std::unique_lock lock{m_TrackerMutex};
            ScopedTrackingDisabler scopedTrackingDisabler;
            LogSummaryUnsafe(logFn, type);
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
        auto pTracker = Tracker::InstanceIfTrackingEnabled();
        if (!!pTracker)
        {
            pTracker->AddExternalStackEntryMarker(markerSV);
        }
    }

    void RegisterExternalStackEntryMarkers(_In_ const std::vector<std::string_view>& markers)
    {
        auto pTracker = Tracker::InstanceIfTrackingEnabled();
        if (!!pTracker)
        {
            pTracker->AddExternalStackEntryMarkers(markers);
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
        _In_ const std::align_val_t alignment /* = std::align_val_t{__STDCPP_DEFAULT_NEW_ALIGNMENT__} */)
    {
        if (byteCount == 0)
        {
            throw std::bad_alloc{};
        }
        if (!IsPowerOfTwo(alignment))
        {
            // _aligned_malloc requires alignment to be power of 2
            throw std::bad_alloc{};
        }

        const Metadata metadata{flags};
        void* const pMem = metadata.IsCustomAlignment()
            ? _aligned_malloc(byteCount, static_cast<std::size_t>(alignment))
            : std::malloc(byteCount);
        if (!pMem)
        {
            if (!metadata.IsNoThrow())
            {
                throw std::bad_alloc{};
            }

            return nullptr;
        }

        const auto pTracker = Tracker::InstanceIfTrackingEnabled(flags);
        if (!!pTracker)
        {
            pTracker->Track(AllocPackage{metadata, pMem, byteCount, std::align_val_t{alignment}});
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

        const Metadata metadata{flags};
        const auto pTracker = Tracker::InstanceIfTrackingEnabled(flags);
        if (!!pTracker)
        {
            pTracker->Track(DeallocPackage{metadata, pMem, byteCount});
        }

        metadata.IsCustomAlignment() ? _aligned_free(pMem) : free(pMem);
    }

    void LogAllocations(
        _In_ const LogCallback& logFn,
        _In_ const LogSummaryType type)
    {
        const auto pTracker = Tracker::InstanceIfTrackingEnabled();
        if (!!pTracker)
        {
            pTracker->LogSummary(logFn, type);
        }
    }
}

std::atomic<std::shared_ptr<AllocationTracking::Tracker>> AllocationTracking::Tracker::s_pTracker{nullptr};
