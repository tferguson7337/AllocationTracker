#pragma once

#include <Windows.h>
#include "AllocationTracker.h"

#include "EnumOps.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <unordered_map>
#include <set>
#include <span>
#include <stacktrace>
#include <thread>



namespace AllocationTracking
{
    extern thread_local std::int64_t gtl_IsInternalAllocationOrFree;
    struct [[nodiscard]] ScopedInternalAllocationOrFreeSetter
    {
        constexpr ScopedInternalAllocationOrFreeSetter() noexcept
        {
            ++gtl_IsInternalAllocationOrFree;
        }

        constexpr ~ScopedInternalAllocationOrFreeSetter() noexcept
        {
            --gtl_IsInternalAllocationOrFree;
        }
    };

    extern thread_local std::int64_t gtl_IsNoTrackAllocationOrFree;
    struct [[nodiscard]] ScopedNoTrackAllocationOrFreeSetter
    {
        constexpr ScopedNoTrackAllocationOrFreeSetter() noexcept
        {
            ++gtl_IsNoTrackAllocationOrFree;
        }

        constexpr ~ScopedNoTrackAllocationOrFreeSetter() noexcept
        {
            --gtl_IsNoTrackAllocationOrFree;
        }
    };

    extern std::atomic<std::int64_t> g_InternalAllocations;
    extern std::atomic<std::int64_t> g_InternalAllocationBytes;

    extern std::atomic<std::int64_t> g_ExternalAllocations;
    extern std::atomic<std::int64_t> g_ExternalAllocationBytes;

    extern std::atomic<bool> g_bTrackingEnabled;
}


namespace AllocationTracking
{
    template <typename ElemT>
    struct NonTrackingAllocator : std::allocator<ElemT>
    {
        [[nodiscard]] _Ret_notnull_ _Post_writable_size_(elems) constexpr ElemT* allocate(_In_ const std::size_t elems) const
        {
            ScopedNoTrackAllocationOrFreeSetter scopedNoTrackAllocation;
            return static_cast<ElemT*>(::operator new(elems * sizeof(ElemT)));
        }

        constexpr void deallocate(_In_opt_ _Post_ptr_invalid_ ElemT* ptr, [[maybe_unused]] _In_ const std::size_t elems) const
        {
            ScopedNoTrackAllocationOrFreeSetter scopedNoTrackDeallocation;
            return ::operator delete(static_cast<void*>(ptr));
        }

#if _HAS_CXX23
        [[nodiscard]] constexpr std::allocation_result<ElemT*, std::size_t>
            allocate_at_least(_In_ const std::size_t bytes)
        {
            return {allocate(bytes), bytes};
        }
#endif
    };

    template <typename ElemT>
    struct SelfAllocator : std::allocator<ElemT>
    {
        [[nodiscard]] _Ret_notnull_ _Post_writable_size_(elems) constexpr ElemT* allocate(_In_ const std::size_t elems) const
        {
            ScopedInternalAllocationOrFreeSetter scopedInternalAlloc;
            const auto bytes = (elems * sizeof(ElemT));
            ++g_InternalAllocations;
            g_InternalAllocationBytes += bytes;
            return static_cast<ElemT*>(::operator new(elems * sizeof(ElemT)));
        }

        constexpr void deallocate(_In_opt_ ElemT* ptr, _In_ const std::size_t elems) const
        {
            ScopedInternalAllocationOrFreeSetter scopedInternalFree;
            const auto bytes = (elems * sizeof(ElemT));
            --g_InternalAllocations;
            g_InternalAllocationBytes -= bytes;
            ::operator delete(static_cast<void*>(ptr));
        }

#if _HAS_CXX23
        [[nodiscard]] constexpr std::allocation_result<ElemT*, std::size_t>
            allocate_at_least(_In_ const std::size_t bytes)
        {
            return {allocate(bytes), bytes};
        }
#endif
    };
}


namespace AllocationTracking
{
    static constexpr std::size_t s_cMaxStackTraceFrames{256};

    template <typename AllocT>
    using StdStackTraceWithAllocatorT = std::basic_stacktrace<AllocT>;

    //
    // Note:
    //  We need our own trimmed down `std::basic_stacktrace`, since with MSVC it rather annoyingly overallocates
    //  its internal std::vector with 0xFFFF capacity when you call `current`, but doesn't shrink_to_fit.
    //  Even if you call `current` with max_depth specified, it'll allocate max_depth elements and never shrink.
    //  If we don't do this, every tracked allocation would take ~512KB or (max_depth * sizeof(void*)).
    //  At the very least we can deal with the temporary alloc from `current` and copy just what we need to a smaller buffer.
    //
    // Note:
    //  Using this struct also lowers the memory footprint of `MemoryInfo` (16 vs 24 bytes)
    //
    struct [[nodiscard]] StackTraceEntryArray
    {
        using ElemT = std::stacktrace_entry;
        using AllocT = SelfAllocator<ElemT>;
        using StdStackTraceT = StdStackTraceWithAllocatorT<AllocT>;

        ElemT* m_pArr{nullptr};
        std::size_t m_Len{0};

        [[nodiscard]] constexpr auto ToSpan() const noexcept
        {
            return std::span{m_pArr, m_Len};
        }

        constexpr StackTraceEntryArray() noexcept = default;

        StackTraceEntryArray(_In_ const StdStackTraceT& allocStackTrace) :
            StackTraceEntryArray{}
        {
            *this = allocStackTrace;
        }

        constexpr StackTraceEntryArray(_In_ const StackTraceEntryArray& other) :
            StackTraceEntryArray{}
        {
            *this = other;
        }

        constexpr StackTraceEntryArray(_Inout_ StackTraceEntryArray&& other) noexcept :
            StackTraceEntryArray{}
        {
            *this = std::move(other);
        }

        constexpr ~StackTraceEntryArray() noexcept
        {
            Reset();
        }

        //
        // Annoyingly, we cannot make this constexpr since the begin/end
        // member functions of std::basic_stacktrace are not constexpr.
        //
        // Oh well.
        //
        StackTraceEntryArray& operator=(_In_ const StdStackTraceT& allocStackTrace)
        {
            const auto len{static_cast<std::size_t>(std::ranges::distance(allocStackTrace))};
            ElemT* const pNewArr{SelfAllocator<ElemT>{}.allocate(len)};

            ElemT* ptr = pNewArr;
            for (const auto& ste : allocStackTrace)
            {
                std::construct_at(ptr++, ste);
            }
            if (pNewArr != (ptr - len)) [[unlikely]]
            {
                __debugbreak();
            }

            Reset();
            m_pArr = pNewArr;
            m_Len = len;

            return *this;
        }

        constexpr StackTraceEntryArray& operator=(_In_ const StackTraceEntryArray& other)
        {
            if (this != &other)
            {
                if (!other)
                {
                    Reset();
                }
                else
                {
                    const auto len{other.m_Len};
                    ElemT* const pNewArr{SelfAllocator<ElemT>{}.allocate(len)};

                    ElemT* ptr = pNewArr;
                    for (const auto& ste : other.ToSpan())
                    {
                        std::construct_at(ptr++, ste);
                    }
                    if (pNewArr != (ptr - len)) [[unlikely]]
                    {
                        __debugbreak();
                    }

                    Reset();
                    m_pArr = ptr;
                    m_Len = len;
                }
            }

            return *this;
        }

        constexpr StackTraceEntryArray& operator=(_Inout_ StackTraceEntryArray&& other) noexcept
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

        constexpr void Reset() noexcept
        {
            if (!!m_pArr)
            {
                AllocT{}.deallocate(m_pArr, m_Len);
                m_pArr = nullptr;
                m_Len = 0;
            }
        }

        [[nodiscard]] constexpr operator bool() const noexcept
        {
            return !!m_pArr;
        }

        [[nodiscard]] constexpr bool operator==(_In_ const StackTraceEntryArray& other) const noexcept
        {
            return std::ranges::equal(ToSpan(), other.ToSpan());
        }

        [[nodiscard]] constexpr auto operator<=>(_In_ const StackTraceEntryArray& other) const noexcept
        {
            const auto lengthCompare3Way{m_Len <=> other.m_Len};
            if (lengthCompare3Way != std::strong_ordering::equal)
            {
                return lengthCompare3Way;
            }
            const auto lhsSpan{ToSpan()};
            const auto rhsSpan{other.ToSpan()};
            return std::lexicographical_compare_three_way(
                lhsSpan.cbegin(), lhsSpan.cend(),
                rhsSpan.cbegin(), rhsSpan.cend());
        }
    };
    /**/

    using StackTraceT = StackTraceEntryArray;
}


namespace AllocationTracking
{
    enum class OpFlag : std::uint32_t
    {
        None = 0x0,

        Alloc = 0x1,
        Realloc = 0x2,
        Free = 0x4
    };

    struct [[nodiscard]] MemoryInfo
    {
        inline static std::atomic<uint64_t> s_IdCounter{0};

        using Clock = std::chrono::system_clock;
        using TimePoint = decltype(Clock::now());

        std::uint64_t m_Id{s_IdCounter++};
        TimePoint m_Timestamp{Clock::now()};
        void* m_pMem{nullptr};
        std::size_t m_Bytes{0};
        StackTraceT m_StackTrace{};
        OpFlag m_OpFlagMask{OpFlag::None};
    };

    struct [[nodiscard]] ReallocMemoryInfo
    {
        void* m_pOriginalMem{nullptr};
        std::size_t m_OriginalBytes{0};

        void* m_pMem{nullptr};
        std::size_t m_Bytes{0};

        //
        // For the Realloc case, we'll generate a synthetic "Free" MemoryInfo
        // for the original address and bytes, as well as the new "Alloc" for
        // the (maybe) new address and bytes.
        //
        // This simplifies Tracker impl for Realloc cases, as well as trims
        // down MemoryInfo size for the more common Alloc and Free cases.
        //
        std::pair<MemoryInfo, MemoryInfo> Synthesize() const noexcept
        {
            const auto timestamp{MemoryInfo::Clock::now()};
            return
            {
                MemoryInfo{
                    .m_Id = MemoryInfo::s_IdCounter++,
                    .m_Timestamp = timestamp,
                    .m_pMem = m_pOriginalMem,
                    .m_Bytes = m_OriginalBytes,
                    .m_OpFlagMask = OpFlag::Free},
                MemoryInfo{
                    .m_Id = MemoryInfo::s_IdCounter++,
                    .m_Timestamp = timestamp,
                    .m_pMem = m_pMem,
                    .m_Bytes = m_Bytes,
                    .m_OpFlagMask = OpFlag::Alloc}
            };
        }
    };



    template <typename KeyT>
    concept ValidMemoryInfoKey =
        std::same_as<KeyT, MemoryInfo> ||
        std::same_as<KeyT, void*>;

    struct [[nodiscard]] MemoryInfoLess
    {
        using is_transparent = int;

        template <ValidMemoryInfoKey KeyLhsT, ValidMemoryInfoKey KeyRhsT>
        [[nodiscard]] constexpr bool operator()(
            _In_ const KeyLhsT& lhs,
            _In_ const KeyRhsT& rhs) const noexcept
        {
            if constexpr (std::same_as<KeyLhsT, MemoryInfo>)
            {
                if constexpr (std::same_as<KeyRhsT, MemoryInfo>)
                {
                    return lhs.m_pMem < rhs.m_pMem;
                }
                else
                {
                    static_assert(std::same_as<KeyRhsT, void*>);
                    return lhs.m_pMem < rhs;
                }
            }
            else
            {
                static_assert(std::same_as<KeyLhsT, void*>);
                if constexpr (std::same_as<KeyRhsT, MemoryInfo>)
                {
                    return lhs < rhs.m_pMem;
                }
                else
                {
                    static_assert(std::same_as<KeyRhsT, void*>);
                    return lhs < rhs;
                }
            }
        }
    };

    using MemoryInfoSet = std::set<MemoryInfo, MemoryInfoLess, SelfAllocator<MemoryInfo>>;
}


namespace AllocationTracking
{
    struct [[nodiscard]] DeregisteredMemoryFreeInfo
    {
        std::uint64_t m_Id{MemoryInfo::s_IdCounter++};
        void* m_pMem{nullptr};
        std::size_t m_Bytes{0};

        [[nodiscard]] MemoryInfo ConvertToMemoryInfo() const noexcept
        {
            return MemoryInfo{
                .m_Id = m_Id,
                .m_pMem = m_pMem,
                .m_Bytes = m_Bytes};
        }
    };

    template <typename ContainerT>
    concept ValidDeregisteredMemoryFreeInfoQueueTransferContainer = requires (ContainerT cont)
    {
        { std::back_inserter(cont) };
    };

    struct [[nodiscard]] DeregisteredMemoryFreeInfoQueue
    {
    public:

        using ElemT = DeregisteredMemoryFreeInfo;
        using AllocT = SelfAllocator<ElemT>;

        static constexpr std::size_t Capacity{(512 << 10) / sizeof(DeregisteredMemoryFreeInfo)};

        ElemT* m_pQueue{nullptr};
        std::size_t m_Len{0};

        constexpr DeregisteredMemoryFreeInfoQueue() :
            m_pQueue{AllocT{}.allocate(Capacity)},
            m_Len{0}
        { }

        // No copy
        DeregisteredMemoryFreeInfoQueue(const DeregisteredMemoryFreeInfoQueue&) = delete;

        constexpr DeregisteredMemoryFreeInfoQueue(_Inout_ DeregisteredMemoryFreeInfoQueue&& other) noexcept :
            m_pQueue{other.m_pQueue},
            m_Len{other.m_Len}
        {
            other.m_pQueue = nullptr;
            other.m_Len = 0;
        }

        constexpr ~DeregisteredMemoryFreeInfoQueue() noexcept
        {
            if (!!m_pQueue)
            {
                AllocT{}.deallocate(m_pQueue, Capacity);
            }
        }

        // No copy
        DeregisteredMemoryFreeInfoQueue& operator=(const DeregisteredMemoryFreeInfoQueue&) = delete;

        constexpr DeregisteredMemoryFreeInfoQueue& operator=(_Inout_ DeregisteredMemoryFreeInfoQueue&& other) noexcept
        {
            if (this != &other)
            {
                Reset();

                m_pQueue = other.m_pQueue;
                m_Len = other.m_Len;

                other.m_pQueue = nullptr;
                other.m_Len = 0;
            }

            return *this;
        }

        constexpr void Reset() noexcept
        {
            if (!!m_pQueue)
            {
                AllocT{}.deallocate(m_pQueue, Capacity);
                m_pQueue = nullptr;
                m_Len = 0;
            }
        }

        constexpr void AddToQueue(_Inout_ ElemT&& elem) noexcept
        {
            if (!m_pQueue || (m_Len == Capacity))
            {
                __debugbreak();
                return;
            }

            std::construct_at(&m_pQueue[m_Len], std::move(elem));
            ++m_Len;
        }

        template <ValidDeregisteredMemoryFreeInfoQueueTransferContainer ContainerT>
        constexpr void TransferTo(_Inout_ ContainerT& cont)
        {
            if (!m_pQueue)
            {
                __debugbreak();
                return;
            }

            std::for_each(m_pQueue, m_pQueue + m_Len,
                [&cont](_In_ const ElemT& freeInfo)
            {
                cont.push_back(freeInfo.ConvertToMemoryInfo());
            });
        }

        constexpr auto ToSpan() const noexcept
        {
            return std::span(m_pQueue, m_Len);
        }
    };
}


namespace AllocationTracking
{
    template <typename LockT>
    concept ValidLockType = requires (LockT lock)
    {
        { lock.Acquire() } -> std::same_as<void>;
        { lock.Release() } -> std::same_as<void>;
    };

    template <ValidLockType LockT>
    class [[nodiscard]] ScopedLock
    {
    private:

        LockT* m_pLock;

    public:

        ScopedLock(_In_ LockT* pLock) :
            m_pLock{pLock}
        {
            m_pLock->Acquire();
        }

        ~ScopedLock()
        {
            m_pLock->Release();
        }
    };

    class [[nodiscard]] AtomicFlagSpinLock
    {
    protected:

        std::atomic_flag m_bOwned{};

    public:

        void Acquire()
        {
            // Loop forever until we get it.
            while (m_bOwned.test_and_set(std::memory_order::acquire)) { }
        }

        void Release()
        {
            m_bOwned.clear(std::memory_order::release);
        }

        auto AcquireScoped() noexcept
        {
            return ScopedLock{this};
        }
    };

    class [[nodiscard]] RecursiveAtomicFlagSpinLock : public AtomicFlagSpinLock
    {
    protected:

        using BaseT = AtomicFlagSpinLock;

        std::atomic<std::thread::id> m_OwnerTID{};
        std::uint32_t m_OwnershipCount{0};

    public:

        void Acquire()
        {
            if (m_OwnerTID == std::this_thread::get_id())
            {
                ++m_OwnershipCount;
                return;
            }

            BaseT::Acquire();
            m_OwnershipCount = 1;
            m_OwnerTID = std::this_thread::get_id();
        }

        void Release()
        {
            if (m_OwnerTID != std::this_thread::get_id())
            {
                __debugbreak();
                return;
            }

            if (--m_OwnershipCount == 0)
            {
                m_OwnerTID = std::thread::id{};
                m_bOwned.clear(std::memory_order::release);
            }
        }

        auto AcquireScoped() noexcept
        {
            return ScopedLock{this};
        }
    };

    template <typename MutexT>
    class [[nodiscard]] DebugLock
    {
    private:

        MutexT m_Mutex;
        std::atomic<std::thread::id> m_OwnerTID{};
        std::uint32_t m_OwnershipCount{0};

    public:

        __declspec(noinline)
        void Acquire()
        {
            if constexpr (
                std::is_same_v<MutexT, std::recursive_mutex> ||
                std::is_same_v<MutexT, std::recursive_timed_mutex>)
            {
                if (m_OwnerTID == std::this_thread::get_id())
                {
                    ++m_OwnershipCount;
                    return;
                }
            }

            if constexpr (
                std::is_same_v<MutexT, std::timed_mutex> ||
                std::is_same_v<MutexT, std::recursive_timed_mutex>)
            {
                while (!m_Mutex.try_lock_for(std::chrono::milliseconds(25)))
                {
                    __debugbreak();
                }
            }
            else
            {
                std::uint32_t tryCounter{0};
                while (!m_Mutex.try_lock())
                {
                    if (++tryCounter > 20)
                    {
                        __debugbreak();
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            }

            m_OwnershipCount = 1;
            m_OwnerTID = std::this_thread::get_id();
        }

        __declspec(noinline)
        void Release()
        {
            if (m_OwnerTID != std::this_thread::get_id())
            {
                __debugbreak();
                return;
            }

            if (--m_OwnershipCount == 0)
            {
                m_OwnerTID = std::thread::id{};
                m_Mutex.unlock();
            }
        }

        __declspec(noinline)
        auto AcquireScoped()
        {
            return ScopedLock{this};
        }
    };

    template <typename MutexT>
    struct [[nodiscard]] NonDebugLock
    {
    private:

        MutexT m_Mutex;

    public:

        void Acquire()
        {
            m_Mutex.lock();
        }

        void Release()
        {
            m_Mutex.unlock();
        }

        auto AcquireScoped()
        {
            return ScopedLock{this};
        }
    };

    /**/
    using PreferredLock = NonDebugLock<std::mutex>;
    using PreferredRecursiveLock = NonDebugLock<std::recursive_mutex>;
    /**
    using PreferredLock = AtomicFlagSpinLock;
    using PreferredRecursiveLock = RecursiveAtomicFlagSpinLock;
    /**/
}

namespace AllocationTracking
{
    using MemoryInfoQueue = std::list<MemoryInfo, SelfAllocator<MemoryInfo>>;
    inline void TransferMemoryInfoQueue(_Inout_ MemoryInfoQueue& recipient, _Inout_ MemoryInfoQueue& donor)
    {
        static_assert(std::same_as<MemoryInfoQueue, std::list<MemoryInfo, SelfAllocator<MemoryInfo>>>);
        recipient.splice(recipient.end(), std::move(donor));
    }

    struct ThreadTracker
    {
        MemoryInfoQueue m_Queue;
        PreferredLock m_QueueBusyLock;
        std::thread::id m_ThreadId{std::this_thread::get_id()};
        bool m_bRegistered{false};
        bool m_bTrackingReady{false};

        ~ThreadTracker();

        bool RegisterWithGlobalTracker();

        void DeregisterWithGlobalTracker();

        void AddToQueue(_Inout_ MemoryInfo&& info);

        void EnableTracking(_In_ const bool bEnable);
    };

    extern thread_local ThreadTracker gtl_ThreadTracker;
}

namespace AllocationTracking
{
    class GlobalTracker
    {
    private:

        mutable PreferredLock m_TrackerLock;

        mutable class WorkerThread
        {
            friend class GlobalTracker;

        private:

            using ThreadTrackerRegistrationInfo = ThreadTracker*;

            // Populated by threads that have deregistered from GlobalTracker.
            MemoryInfoQueue m_BacklogQueue;
            PreferredLock m_BacklogLock;

            // Populated by threads that have not yet registered or have deregistered,
            // intended to cover scenarios where thread has reached a point during exit
            // where it deregisters from GlobalTracker, where TLS can no longer be accessed.
            DeregisteredMemoryFreeInfoQueue m_DeregisteredFreeQueue;
            PreferredLock m_DeregisteredFreeQueueLock;

            using ThreadRegistrationContainer =
                std::deque<ThreadTrackerRegistrationInfo, SelfAllocator<ThreadTrackerRegistrationInfo>>;
            ThreadRegistrationContainer m_ThreadTrackerRegistry;
            PreferredLock m_ThreadTrackerRegistryLock;

            std::atomic<bool> m_bContinueWork{true};

            // Used for when a LogAllocation request comes in and wishes to wait for current worker loop to complete.
            PreferredLock m_WorkerThreadInProgressLock;

            // Note: It's important that this is last
            std::jthread m_Thread;

            bool WaitForWork();

            MemoryInfoQueue GetAllQueued();

            void WorkerLoop();

        public:

            WorkerThread();

            ~WorkerThread();

            void Start();

            void SignalStop(_In_ const bool bWait);

            void RegisterThreadTracker(_In_ ThreadTracker* pThreadTracker);
            void DeregisterThreadTracker(_In_ ThreadTracker* pThreadTracker);

            void AddToDeregisteredFreeQueue(_Inout_ DeregisteredMemoryFreeInfo&& info);
        } m_WorkerThread;

    private:

        using StringT = std::basic_string<char, std::char_traits<char>, SelfAllocator<char>>;

        using StackTraceEntryToMemoryInfoSetMap = std::unordered_map<std::stacktrace_entry, MemoryInfoSet, std::hash<std::stacktrace_entry>, std::equal_to<std::stacktrace_entry>, SelfAllocator<std::pair<const std::stacktrace_entry, MemoryInfoSet>>>;
        StackTraceEntryToMemoryInfoSetMap m_StackTraceEntryToMemoryInfoSetMap;

        using CachedInfo = std::pair<std::stacktrace_entry, MemoryInfoSet::const_iterator>;
        using AllocationAddrToCachedInfoMap = std::map<void*, CachedInfo, std::less<>, SelfAllocator<std::pair<void* const, CachedInfo>>>;
        AllocationAddrToCachedInfoMap m_AllocationAddrToCachedInfoMap;

        using AllocationAddrToMemoryInfoMap = std::map<const void*, MemoryInfo, std::less<>, SelfAllocator<std::pair<const void* const, MemoryInfo>>>;
        MemoryInfoSet m_MemoryInfoSet;

        using ExternalUserStackTraceEntryMarkers = std::vector<StringT, SelfAllocator<StringT>>;
        ExternalUserStackTraceEntryMarkers m_ExternalUserStackTraceEntryMarkers;

        StringT m_TargetModuleNamePrefix;

        std::atomic<bool> m_bCollectFullStackTraces{true};

        struct AllocSummaryInfo
        {
            std::size_t m_TotalBytes{0};
            std::size_t m_TotalAllocations{0};

            MemoryInfo::TimePoint m_OldestAllocation{(MemoryInfo::TimePoint::max)()};
            MemoryInfo::TimePoint m_NewestAllocation{(MemoryInfo::TimePoint::min)()};

            struct FullStackTracePtrLess
            {
                [[nodiscard]] constexpr bool operator()(
                    _In_ const StackTraceT* pLhs,
                    _In_ const StackTraceT* pRhs) const noexcept
                {
                    return *pLhs < *pRhs;
                }
            };

            using FullStackTracePtrSet = std::set<const StackTraceT*, FullStackTracePtrLess, NonTrackingAllocator<const StackTraceT*>>;
            FullStackTracePtrSet m_FullStackTraces;

            AllocSummaryInfo() noexcept;
            AllocSummaryInfo(_In_ const MemoryInfo& info) noexcept;

            AllocSummaryInfo& operator<<(_In_ const AllocSummaryInfo& other) noexcept;
        };

        // Logs each unique alloc-stacktrace, with total bytes each trace has allocated.
        // Also logs total allocations, total bytes overall, and tracking overhead bytes.
        void LogSummaryUnsafe(
            _In_ const LogCallback& logFn,
            _In_ const LogSummaryType type) const;

        void AddExternalStackEntryMarkerUnsafe(_In_ const std::string_view markerSV);

        [[nodiscard]] StackTraceEntryToMemoryInfoSetMap::iterator FindSTEHashMapElement(_In_ const MemoryInfo& info);

        [[nodiscard]] bool IsExternalStackTraceEntryByDescriptionUnsafe(_In_ const std::stacktrace_entry entry) const;

        [[nodiscard]] std::stacktrace_entry FindFirstNonExternalStackTraceEntryByDescriptionUnsafe(_In_ const MemoryInfo& info) const;

        void ProcessAllocationUnsafe(_Inout_ MemoryInfo&& info);

        void ProcessDeallocationUnsafe(_In_ const MemoryInfo& info);

        static std::shared_ptr<GlobalTracker> s_pTracker;

    public:

        static void Init();

        static void DeInit();

        [[nodiscard]] static std::shared_ptr<GlobalTracker> Instance() noexcept;

        __declspec(noinline)
        [[nodiscard]] static std::shared_ptr<GlobalTracker> InstanceIfTrackingEnabled() noexcept;

        void SetTargetModuleNamePrefix(_In_ const std::string_view prefixSV);

        void AddExternalStackEntryMarker(_In_ const std::string_view markerSV);

        void AddExternalStackEntryMarkers(_In_ const std::vector<std::string_view>& markers);

        void SetCollectFullStackTraces(_In_ const bool bCollect);

        void RegisterThreadTracker(_In_ ThreadTracker* pTT);

        void DeregisterThreadTracker(_In_ ThreadTracker* pTT);

        void TrackAllocation(_Inout_ MemoryInfo&& info);
        void TrackDeallocation(_Inout_ MemoryInfo&& info);

        void AddToDeregisteredFreeQueue(_Inout_ DeregisteredMemoryFreeInfo&& info);

        void LogSummary(
            _In_ const LogCallback& logFn,
            _In_ const LogSummaryType type,
            _In_ const bool waitForWorkerThreadLull) const;
    };
}
