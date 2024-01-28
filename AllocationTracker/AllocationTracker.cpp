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
    //  Since we need to get the stacktrace for each allocation, this can be very time consuming.
    //  This allocator will instead provide a pre-allocated buffer to avoid repeated allocations.
    //
    // Note:
    //  Caller will need to synchronize uses of this using the allocator's mutex.
    //
    template <typename ElemT>
    struct ThreadLocalStackAllocator : std::allocator<ElemT>
    {
        static constexpr std::size_t s_cMaxElementsSupported = 256;
        static constexpr std::size_t s_cBufferSize = (sizeof(ElemT) * s_cMaxElementsSupported);

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


/*
    std::basic_stacktrace::current's implementation is extremely problematic for our use case.
    The crux of the issue is that it is pre-allocating a std::vector<std::stacktrace_entry, AllocT> with 0xFFFF
    elements, like so:

    >> static constexpr size_t _Max_frames = 0xFFFF;
    ...
    >> basic_stacktrace _Result{_Internal_t{}, _Max_frames, _Al};
    >> const unsigned short _Actual_size = __std_stacktrace_capture(...);
    >> _Result._Frames.resize(_Actual_size);
    >> return _Result;

    It's worth noting that the .resize call doesn't shrink the vector, it effectively just pop_back's all the unneeded elements.

    The work needed to effectively zero-initialize ~512KB of memory is a bit slow when we're doing this for each allocation.

    Due to this, I'm going to effectively copy-paste MSVC's implementation, but trim down the max amount of frames
    we expect to hit.  65,535 frames seems excessive, while ~255 (0xFF) seems a bit more reasonable.  Even 1,024
    frames (which still seems like quite a lot, at least for my use-case) would heaps better.
*/
namespace AllocationTracking
{
    template <typename _Alloc = ThreadLocalStackAllocator<std::stacktrace_entry>, std::size_t MaxFrames = ThreadLocalStackAllocator<std::stacktrace_entry>::s_cMaxElementsSupported>
    class AllocStackTrace
    {
    private:
        using _Frames_t = std::vector<std::stacktrace_entry, _Alloc>;

    public:
        using value_type = std::stacktrace_entry;
        using const_reference = const value_type&;
        using reference = value_type&;
        using const_iterator = _Frames_t::const_iterator;
        using iterator = const_iterator;
        using reverse_iterator = _STD reverse_iterator<iterator>;
        using const_reverse_iterator = _STD reverse_iterator<const_iterator>;
        using difference_type = _Frames_t::difference_type;
        using size_type = _Frames_t::size_type;
        using allocator_type = _Alloc;

        // __declspec(noinline) to make the same behavior for debug and release.
        // We force the current function to be always noinline and add its frame to skipped.

        _NODISCARD __declspec(noinline) static AllocStackTrace
            current(const allocator_type& _Al = allocator_type()) noexcept {
            _TRY_BEGIN
                AllocStackTrace _Result{_Internal_t{}, _Max_frames, _Al};
            const unsigned short _Actual_size = __std_stacktrace_capture(
                1, static_cast<unsigned long>(_Max_frames), _Result._To_voidptr_array(), &_Result._Hash);
            _Result._Frames.resize(_Actual_size);
            return _Result;
            _CATCH_ALL
                return AllocStackTrace{_Al};
            _CATCH_END
        }

        _NODISCARD __declspec(noinline) static AllocStackTrace
            current(const size_type _Skip, const allocator_type& _Al = allocator_type()) noexcept {
            _TRY_BEGIN
                AllocStackTrace _Result{_Internal_t{}, _Max_frames, _Al};
            const unsigned short _Actual_size = __std_stacktrace_capture(
                _Adjust_skip(_Skip), static_cast<unsigned long>(_Max_frames), _Result._To_voidptr_array(), &_Result._Hash);
            _Result._Frames.resize(_Actual_size);
            return _Result;
            _CATCH_ALL
                return AllocStackTrace{_Al};
            _CATCH_END
        }

        _NODISCARD __declspec(noinline) static AllocStackTrace
            current(const size_type _Skip, size_type _Max_depth, const allocator_type& _Al = allocator_type()) noexcept {
            _TRY_BEGIN
                if (_Max_depth > _Max_frames) {
                    _Max_depth = _Max_frames;
                }

            AllocStackTrace _Result{_Internal_t{}, _Max_depth, _Al};

            const unsigned short _Actual_size = __std_stacktrace_capture(
                _Adjust_skip(_Skip), static_cast<unsigned long>(_Max_depth), _Result._To_voidptr_array(), &_Result._Hash);
            _Result._Frames.resize(_Actual_size);
            return _Result;
            _CATCH_ALL
                return AllocStackTrace{_Al};
            _CATCH_END
        }

        AllocStackTrace() noexcept(std::is_nothrow_default_constructible_v<allocator_type>) = default;
        explicit AllocStackTrace(const allocator_type& _Al) noexcept : _Frames(_Al) {}

        AllocStackTrace(const AllocStackTrace&) = default;
        AllocStackTrace(AllocStackTrace&&) noexcept = default;
        AllocStackTrace(const AllocStackTrace& _Other, const allocator_type& _Al)
            : _Frames(_Other._Frames, _Al), _Hash(_Other._Hash) {}

        AllocStackTrace(AllocStackTrace&& _Other, const allocator_type& _Al)
            : _Frames(_STD move(_Other._Frames), _Al), _Hash(_Other._Hash) {}

        AllocStackTrace& operator=(const AllocStackTrace&) = default;
        AllocStackTrace& operator=(AllocStackTrace&&) noexcept(_Noex_move) = default;

        ~AllocStackTrace() = default;

        _NODISCARD allocator_type get_allocator() const noexcept {
            return _Frames.get_allocator();
        }

        _NODISCARD const_iterator begin() const noexcept {
            return _Frames.cbegin();
        }

        _NODISCARD const_iterator end() const noexcept {
            return _Frames.cend();
        }

        _NODISCARD const_reverse_iterator rbegin() const noexcept {
            return _Frames.crbegin();
        }

        _NODISCARD const_reverse_iterator rend() const noexcept {
            return _Frames.crend();
        }

        _NODISCARD const_iterator cbegin() const noexcept {
            return _Frames.cbegin();
        }

        _NODISCARD const_iterator cend() const noexcept {
            return _Frames.cend();
        }

        _NODISCARD const_reverse_iterator crbegin() const noexcept {
            return _Frames.crbegin();
        }

        _NODISCARD const_reverse_iterator crend() const noexcept {
            return _Frames.crend();
        }

        _NODISCARD_EMPTY_STACKTRACE_MEMBER bool empty() const noexcept {
            return _Frames.empty();
        }

        _NODISCARD size_type size() const noexcept {
            return _Frames.size();
        }

        _NODISCARD size_type max_size() const noexcept {
            return _Frames.max_size();
        }

        _NODISCARD const_reference operator[](const size_type _Sx) const noexcept /* strengthened */ {
            return _Frames[_Sx];
        }

        _NODISCARD const_reference at(const size_type _Sx) const {
            return _Frames.at(_Sx);
        }

        template <class _Al2>
        _NODISCARD_FRIEND bool operator==(const AllocStackTrace& _Lhs, const AllocStackTrace<_Al2>& _Rhs) noexcept {
            return _Lhs._Hash == _Rhs._Hash && _STD equal(_Lhs.begin(), _Lhs.end(), _Rhs.begin(), _Rhs.end());
        }

        template <class _Al2>
        _NODISCARD_FRIEND std::strong_ordering operator<=>(
            const AllocStackTrace& _Lhs, const AllocStackTrace<_Al2>& _Rhs) noexcept {
            const auto _Result = _Lhs._Frames.size() <=> _Rhs._Frames.size();
            if (_Result != std::strong_ordering::equal) {
                return _Result;
            }

#ifdef __cpp_lib_concepts
            return _STD lexicographical_compare_three_way(_Lhs.begin(), _Lhs.end(), _Rhs.begin(), _Rhs.end());
#else // ^^^ defined(__cpp_lib_concepts) / !defined(__cpp_lib_concepts) vvv
            for (size_t _Ix = 0, _Mx = _Lhs._Frames.size(); _Ix != _Mx; ++_Ix) {
                if (_Lhs._Frames[_Ix] != _Rhs._Frames[_Ix]) {
                    return _Lhs._Frames[_Ix] <=> _Rhs._Frames[_Ix];
                }
            }

            return strong_ordering::equal;
#endif // ^^^ !defined(__cpp_lib_concepts) ^^^
        }

        void swap(AllocStackTrace& _Other) noexcept(std::allocator_traits<_Alloc>::propagate_on_container_swap::value
            || std::allocator_traits<_Alloc>::is_always_equal::value) {
            _Frames.swap(_Other._Frames);
            _STD swap(_Hash, _Other._Hash);
        }

        _NODISCARD unsigned long _Get_hash() const noexcept {
            return _Hash;
        }

        _STL_INTERNAL_STATIC_ASSERT(sizeof(void*) == sizeof(std::stacktrace_entry));
        _STL_INTERNAL_STATIC_ASSERT(alignof(void*) == alignof(std::stacktrace_entry));

        _NODISCARD const void* const* _To_voidptr_array() const noexcept {
            return reinterpret_cast<const void* const*>(_Frames.data());
        }

        _NODISCARD void** _To_voidptr_array() noexcept {
            return reinterpret_cast<void**>(_Frames.data());
        }

    private:
        static constexpr size_t _Max_frames = MaxFrames;

        static constexpr bool _Noex_move = std::allocator_traits<_Alloc>::propagate_on_container_move_assignment::value
            || std::allocator_traits<_Alloc>::is_always_equal::value;

        struct _Internal_t {
            explicit _Internal_t() noexcept = default;
        };

        AllocStackTrace(_Internal_t, size_type _Max_depth, const allocator_type& _Al) : _Frames(_Max_depth, _Al) {}

        _NODISCARD static unsigned long _Adjust_skip(size_t _Skip) noexcept {
            return _Skip < ULONG_MAX - 1 ? static_cast<unsigned long>(_Skip + 1) : ULONG_MAX;
        }

        _Frames_t _Frames;
        unsigned long _Hash = 0;
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

    static std::atomic<std::uint64_t> g_MemoryInfoId{0};

    struct [[nodiscard]] MemoryInfo
    {
        using Clock = std::chrono::system_clock;
        using TimePoint = decltype(Clock::now());

        std::uint64_t m_Id{g_MemoryInfoId++};
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

    struct [[nodiscard]] AtomicFlagSpinLock
    {
    private:

        std::atomic_flag m_bOwned{};

        template <bool bAllowThreadYield>
        void Acquire()
        {
            while (m_bOwned.test_and_set(std::memory_order::acquire))
            {
                if constexpr (/*bAllowThreadYield*/ false)
                {
                    // If this is the worker thread, yield to give cycles to the allocating threads.
                    std::this_thread::yield();
                }
            }
        }

        void Release()
        {
            m_bOwned.clear(std::memory_order::release);
        }

    public:

        template <bool bAllowThreadYield>
        struct [[nodiscard]] ScopedAcquirer
        {
            AtomicFlagSpinLock* m_pAFSL;

            ScopedAcquirer(_In_ AtomicFlagSpinLock* pAFSL) noexcept :
                m_pAFSL{pAFSL}
            {
                m_pAFSL->Acquire<bAllowThreadYield>();
            }

            ~ScopedAcquirer() noexcept
            {
                m_pAFSL->Release();
            }
        };

        template <bool bAllowThreadYield>
        ScopedAcquirer<bAllowThreadYield> AcquireScoped() noexcept
        {
            return ScopedAcquirer<bAllowThreadYield>{this};
        }
    };

    thread_local struct ThreadTracker
    {
        using Queue = std::deque<MemoryInfo, SelfAllocator<MemoryInfo>>;
        Queue m_Queue;
        AtomicFlagSpinLock m_bQueueBusy;
        std::thread::id m_Tid{std::this_thread::get_id()};
        bool m_bRegistered{false};

        ~ThreadTracker()
        {
            if (m_bRegistered)
            {
                DeregisterWithGlobalTracker();
            }
        }

        bool RegisterWithGlobalTracker();

        void DeregisterWithGlobalTracker();

        void AddToQueue(_Inout_ MemoryInfo&& info)
        {
            if (!RegisterWithGlobalTracker())
            {
                // If we haven't successfully registered, don't flood the queue.
                return;
            }

            auto scopedSpinLock{m_bQueueBusy.AcquireScoped<false>()};
            // std::lock_guard lock(m_QueueMutex);
            m_Queue.push_back(std::move(info));
        }
    } gtl_ThreadTracker;

    class GlobalTracker
    {
    private:

        mutable AtomicFlagSpinLock m_TrackerSpinLock;
        // mutable std::mutex m_TrackerMutex;

        mutable class WorkerThread
        {
            friend class GlobalTracker;

        private:

            using ThreadTrackerRegistrationInfo = ThreadTracker*;

            // Populated by threads that have deregistered from GlobalTracker.
            ThreadTracker::Queue m_BacklogQueue;
            AtomicFlagSpinLock m_BacklogSpinLock;
            // std::mutex m_BacklogQueueMutex;

            std::deque<ThreadTrackerRegistrationInfo, SelfAllocator<ThreadTrackerRegistrationInfo>> m_ThreadTrackerRegistry;
            // std::condition_variable m_ThreadTrackerRegistryCV;
            AtomicFlagSpinLock m_ThreadTrackerRegistrySpinLock;
            // std::mutex m_ThreadTrackerRegistryMutex;

            std::atomic<bool> m_bContinueWork{true};

            // Used for when a LogAllocation request comes in and wishes to wait for current worker loop to complete.
            AtomicFlagSpinLock m_WorkerThreadInProgress;

            // Note: It's important that this is last
            std::jthread m_Thread;

            bool WaitForWork()
            {
                static constexpr auto s_cWaitTimeMs{std::chrono::milliseconds(50)};
                std::this_thread::sleep_for(s_cWaitTimeMs);
                return m_bContinueWork;
            }

            ThreadTracker::Queue GetAllQueued()
            {
                ThreadTracker::Queue ret;
                {
                    auto scopedRegistrySpinLock{m_ThreadTrackerRegistrySpinLock.AcquireScoped<true>()};
                    for (const auto pThreadTracker : m_ThreadTrackerRegistry)
                    {
                        auto scopedTTQueueSpinLock{pThreadTracker->m_bQueueBusy.AcquireScoped<true>()};
                        std::ranges::move(pThreadTracker->m_Queue, std::back_inserter(ret));
                        pThreadTracker->m_Queue.clear();
                    }
                }
                {
                    auto scopedBacklogSpinLock{m_BacklogSpinLock.AcquireScoped<true>()};
                    std::ranges::move(m_BacklogQueue, std::back_inserter(ret));
                    m_BacklogQueue.clear();
                }

                //
                // Now that we're splicing together a list of allocs/frees spanning multiple threads,
                // we need to ensure that the content is in order. This is to cover cases where one
                // thread allocates memory, then passes ownership of that memory to another thread.
                // Without this, we run the risk of processing the free before the allocation, which
                // means we'll never stop tracking the since freed alloc.
                //
                // The simplest way to do this at the moment is using the timestamp.
                // One concern is that it is currently std::chrono::system_clock, which is really just
                // either GetSystemTimeAsFileTime (older, less precise) or GetSystemTimePreciseAsFileTime
                // (more precise, <1us), but it's not clear if that's always available (e.g., older versions of Win).
                //
                // If this is insufficient, we could add an additional field to MemoryInfo that gets assigned an ID
                // from a global atomic counter for each alloc/free, then sort off of that to establish a confirmed order.
                // This would add yet another 8 byte overhead to allocation however - not ideal.
                //
                std::ranges::sort(ret, [](_In_ const MemoryInfo& lhs, _In_ const MemoryInfo& rhs) { return lhs.m_Id < rhs.m_Id; });

                return ret;
            }

            void WorkerLoop()
            {
                while (WaitForWork())
                {
                    auto scopedWorkInProgressSL{m_WorkerThreadInProgress.AcquireScoped<false>()};
                    auto pTracker = GlobalTracker::InstanceIfTrackingEnabled();
                    if (!pTracker)
                    {
                        continue;
                    }

                    auto queue{GetAllQueued()};
                    if (queue.empty())
                    {
                        continue;
                    }

                    auto scopedSpinLock{pTracker->m_TrackerSpinLock.AcquireScoped<true>()};
                    for (auto& info : queue)
                    {
                        if (!m_bContinueWork)
                        {
                            return;
                        }

                        pTracker->ProcessMemoryInfoUnsafe(std::move(info));
                    }
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
                if (bWait && m_Thread.joinable())
                {
                    m_Thread.join();
                }
            }

            void RegisterThreadTracker(_In_ ThreadTracker* pThreadTracker)
            {
                const auto scopedSpinLock{m_ThreadTrackerRegistrySpinLock.AcquireScoped<false>()};
                // std::lock_guard lock{m_ThreadTrackerRegistryMutex};
                m_ThreadTrackerRegistry.emplace_back(pThreadTracker);
            }

            void DeregisterThreadTracker(_In_ ThreadTracker* pThreadTracker)
            {
                {
                    const auto scopedSpinLock{m_ThreadTrackerRegistrySpinLock.AcquireScoped<false>()};
                    const auto registrationItr = std::ranges::find(m_ThreadTrackerRegistry, pThreadTracker);
                    if (registrationItr != m_ThreadTrackerRegistry.end())
                    {
                        std::swap(*registrationItr, m_ThreadTrackerRegistry.back());
                        m_ThreadTrackerRegistry.pop_back();
                    }
                }

                // Move any queued alloc/dealloc's to the backlog queue.
                auto tmpQueue = [pThreadTracker]()
                {
                    const auto scopedSpinLock{pThreadTracker->m_bQueueBusy.AcquireScoped<false>()};
                    ThreadTracker::Queue tmp{std::move(pThreadTracker->m_Queue)};
                    pThreadTracker->m_Queue.clear();
                    return tmp;
                }();

                const auto scopedSpinLock{m_BacklogSpinLock.AcquireScoped<false>()};
                std::ranges::move(tmpQueue, std::back_inserter(m_BacklogQueue));
            }
        } m_WorkerThread;

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
            logLines += std::format("\n\n  Total External[{} : {} ({})]\n  Total Tracker[{} : {} ({}) (~{}/ExtAlloc)]\n",
                FmtDec{}(overallAllocSummaryInfo.m_TotalAllocations),
                FmtByteUpToMebibyte{}(overallAllocSummaryInfo.m_TotalBytes), FmtByte{}(overallAllocSummaryInfo.m_TotalBytes),
                FmtDec{}(trackerAllocCount),
                FmtByteUpToMebibyte{}(trackerByteCount), FmtByte{}(trackerByteCount),
                FmtByte{}(BytesPerAllocationAverage(trackerByteCount, overallAllocSummaryInfo.m_TotalAllocations)));

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
                const auto cachedInfoItr = m_AllocationAddrToCachedInfoMap.find(info.m_pMem);
                if (cachedInfoItr != m_AllocationAddrToCachedInfoMap.end())
                {
                    const auto [hashMapKey, infoSetItr] = cachedInfoItr->second;
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
                    m_AllocationAddrToCachedInfoMap.erase(cachedInfoItr);
                }
                else
                {
                    // A few possibilities:
                    //  1) Allocation occurred while tracking was disabled.
                    //  2) Someone allocated with `new`, but deallocated with `free`.
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

        static std::shared_ptr<GlobalTracker> s_pTracker;

    public:

        static void Init()
        {
            s_pTracker = std::make_shared<GlobalTracker>();
        }

        static void DeInit()
        {
            s_pTracker.reset();
        }

        [[nodiscard]] static std::shared_ptr<GlobalTracker> Instance() noexcept
        {
            return s_pTracker;
        }

        [[nodiscard]] static std::shared_ptr<GlobalTracker> InstanceIfTrackingEnabled(_In_ const AllocFlag flags = {}) noexcept
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

        void AddExternalStackEntryMarker(_In_ const std::string_view markerSV)
        {
            auto scopedSpinLock{m_TrackerSpinLock.AcquireScoped<false>()};
            // std::unique_lock lock{m_TrackerMutex};
            AddExternalStackEntryMarkerUnsafe(std::string{markerSV});
        }

        void AddExternalStackEntryMarkers(_In_ const std::vector<std::string_view>& markers)
        {
            auto scopedSpinLock{m_TrackerSpinLock.AcquireScoped<false>()};
            // std::unique_lock lock{m_TrackerMutex};
            std::ranges::for_each(markers, [this](const auto markerSV) { AddExternalStackEntryMarkerUnsafe(std::string{markerSV}); });
        }

        void CollectFullStackTraces(_In_ const bool bCollect)
        {
            m_bCollectFullStackTraces = bCollect;
        }

        void RegisterThreadTracker(_In_ ThreadTracker* pTT)
        {
            m_WorkerThread.RegisterThreadTracker(pTT);
        }

        void DeregisterThreadTracker(_In_ ThreadTracker* pTT)
        {
            m_WorkerThread.DeregisterThreadTracker(pTT);
        }

        void Track(_Inout_ MemoryInfo&& info)
        {
            const bool bIsFree = !!(info.m_FlagMask & AllocFlag::Free);

            if (!!(info.m_FlagMask & AllocFlag::NoTracking))
            {
                return;
            }
            if (!!(info.m_FlagMask & AllocFlag::SelfAlloc))
            {
                // We don't queue these up, just update internal self-alloc counters.
                if (bIsFree)
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

            if (!bIsFree)
            {
                info.m_StackTrace = AllocStackTrace<>::current(1);
            }

            gtl_ThreadTracker.AddToQueue(std::move(info));
        }

        void LogSummary(
            _In_ const LogCallback& logFn,
            _In_ const LogSummaryType type,
            _In_ const bool waitForWorkerThreadLull) const
        {
            if (waitForWorkerThreadLull)
            {
                auto waitSpinScoped{m_WorkerThread.m_WorkerThreadInProgress.AcquireScoped<true>()};
            }
            auto scopedSpinLock{m_TrackerSpinLock.AcquireScoped<false>()};
            // std::unique_lock lock{m_TrackerMutex};
            ScopedThreadLocalTrackingDisabler stltd;
            LogSummaryUnsafe(logFn, type);
        }
    };


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

    ScopedTrackerInit::ScopedTrackerInit()
    {
        GlobalTracker::Init();
    }

    ScopedTrackerInit::~ScopedTrackerInit()
    {
        GlobalTracker::DeInit();
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

    void CollectFullStackTraces(_In_ const bool bCollect)
    {
        auto pTracker = GlobalTracker::Instance();
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
        // 3. Ensure alignment doesn't overflow.
        const bool bCanThrow = !(flags & AllocFlag::NoThrow);
        if ((byteCount == 0) ||
            !IsPowerOfTwo(alignment) ||
            (static_cast<std::size_t>(alignment) > (std::numeric_limits<decltype(MemoryInfo::m_Alignment)>::max)()))
        {
            if (bCanThrow)
            {
                throw std::bad_alloc{};
            }

            return nullptr;
        }

        static constexpr auto s_cMaxU8{(std::numeric_limits<std::uint8_t>::max)()};
        const auto alignmentU8 = static_cast<std::uint8_t>(
            (std::min)(
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

        auto pGlobalTracker = GlobalTracker::InstanceIfTrackingEnabled(flags);
        if (!!pGlobalTracker)
        {
            pGlobalTracker->Track(
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

        auto pGlobalTracker = GlobalTracker::InstanceIfTrackingEnabled(flags);
        if (!!pGlobalTracker)
        {
            pGlobalTracker->Track(
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
        _In_ const bool bWaitForWorkerThreadLull /* = false */)
    {
        const auto pTracker = GlobalTracker::Instance();
        if (!!pTracker)
        {
            pTracker->LogSummary(logFn, type, bWaitForWorkerThreadLull);
        }
    }
}

std::shared_ptr<AllocationTracking::GlobalTracker> AllocationTracking::GlobalTracker::s_pTracker{nullptr};
