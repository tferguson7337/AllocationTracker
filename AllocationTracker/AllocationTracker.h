#pragma once

#include <sal.h>

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>


namespace AllocationTracking
{
    enum class AllocFlag : std::uint8_t
    {
        None = 0x00,
        Array = 0x01,
        NoThrow = 0x02,
        Placement = 0x04,
        CustomAlignment = 0x08,
        NoTracking = 0x10,
        SelfAlloc = 0x20,
        Free = 0x40,

        _LastPlusOne,
        _Begin = 0x1,
        _Last = _LastPlusOne - 1,
        _End = _Last << 1,
        _ValidMask = _End - 1
    };

    [[nodiscard]] constexpr AllocFlag operator&(
        _In_ const AllocFlag lhs,
        _In_ const AllocFlag rhs) noexcept
    {
        using T = AllocFlag;
        using UT = std::underlying_type_t<T>;
        return static_cast<T>(static_cast<UT>(lhs) & static_cast<UT>(rhs));
    }

    constexpr AllocFlag& operator&=(
        _Inout_ AllocFlag& lhs,
        _In_ const AllocFlag rhs) noexcept
    {
        lhs = lhs & rhs;
        return lhs;
    }

    [[nodiscard]] constexpr AllocFlag operator|(
        _In_ const AllocFlag lhs,
        _In_ const AllocFlag rhs) noexcept
    {
        using T = AllocFlag;
        using UT = std::underlying_type_t<T>;
        return static_cast<T>(static_cast<UT>(lhs) | static_cast<UT>(rhs));
    }

    constexpr AllocFlag& operator|=(
        _Inout_ AllocFlag& lhs,
        _In_ const AllocFlag rhs) noexcept
    {
        lhs = lhs | rhs;
        return lhs;
    }

    [[nodiscard]] constexpr AllocFlag operator<<(
        _In_ const AllocFlag lhs,
        _In_range_(0, (sizeof(AllocFlag) * 8) - 1) const int rhs) noexcept
    {
        using T = AllocFlag;
        using UT = std::underlying_type_t<T>;
        return static_cast<T>(static_cast<UT>(lhs) << rhs);
    }

    constexpr AllocFlag& operator<<=(
        _Inout_ AllocFlag& lhs,
        _In_range_(0, (sizeof(AllocFlag) * 8) - 1) const int rhs) noexcept
    {
        lhs = lhs << rhs;
        return lhs;
    }

    [[nodiscard]] constexpr bool operator!(_In_ const AllocFlag rhs) noexcept
    {
        using UT = std::underlying_type_t<AllocFlag>;
        return static_cast<UT>(rhs) == 0;
    }

    [[nodiscard]] constexpr std::string_view FlagToStringView(_In_ const AllocFlag flag) noexcept
    {
        using namespace std::string_view_literals;
        switch (flag)
        {
        case AllocFlag::None: return "None"sv;
        case AllocFlag::Array: return "Array"sv;
        case AllocFlag::NoThrow: return "NoThrow"sv;
        case AllocFlag::Placement: return "Placement"sv;
        case AllocFlag::CustomAlignment: return "CustomAlignment"sv;
        case AllocFlag::NoTracking: return "NoTracking"sv;
        case AllocFlag::SelfAlloc: return "SelfAlloc"sv;
        case AllocFlag::Free: return "Free"sv;
        }

        return "Unknown"sv;
    }

    [[nodiscard]] _CONSTEXPR20 std::string FlagMaskToString(_In_ const AllocFlag mask)
    {
        if (!mask)
        {
            using namespace std::string_literals;
            return "None"s;
        }

        std::string str;
        str.reserve(64);

        for (AllocFlag bit = AllocFlag::_Begin; bit != AllocFlag::_End; bit <<= 1)
        {
            if (!(bit & mask))
            {
                continue;
            }

            using namespace std::string_view_literals;
            if (!str.empty()) { str += ", "sv; }
            str += FlagToStringView(bit);
        }

        return str;
    }
}


namespace AllocationTracking
{
    [[nodiscard]] void* PerformAllocation(
        _In_ const AllocFlag flags,
        _In_ const std::size_t byteCount,
        _In_ const std::align_val_t alignment = std::align_val_t{__STDCPP_DEFAULT_NEW_ALIGNMENT__});

    void PerformDeallocation(
        _In_ const AllocFlag flags,
        _In_opt_ void* pMem,
        _In_ const std::size_t byteCount = 0) noexcept;

    using LogCallback = std::function<void(_In_ const std::string_view logMsgSV)>;

    enum class LogSummaryType
    {
        Limited,
        Normal,
        FullStackTraces
    };

    void LogAllocations(
        _In_ const LogCallback& logFn,
        _In_ const LogSummaryType type,
        _In_ const bool waitForWorkerThreadLull = false);
}


namespace AllocationTracking
{
    struct ScopedThreadLocalTrackingDisabler
    {
        ScopedThreadLocalTrackingDisabler();
        ~ScopedThreadLocalTrackingDisabler();
    };

    struct ScopedGlobalTrackingDisabler
    {
        ScopedGlobalTrackingDisabler();
        ~ScopedGlobalTrackingDisabler();
    };

    struct ScopedTrackerInit
    {
        ScopedTrackerInit();
        ~ScopedTrackerInit();
    };

    void RegisterExternalStackEntryMarker(_In_ const std::string_view markerSV);
    void RegisterExternalStackEntryMarkers(_In_ const std::vector<std::string_view>& markers);

    void CollectFullStackTraces(_In_ const bool bCollect);
}


namespace AllocationTracking
{
    template <typename ElemT>
    struct NonTrackingAllocator : std::allocator<ElemT>
    {
        [[nodiscard]] _Ret_notnull_ _Post_writable_size_(elems) constexpr ElemT* allocate(_In_ const std::size_t elems) const
        {
            return static_cast<ElemT*>(PerformAllocation(AllocFlag::NoTracking, elems * sizeof(ElemT)));
        }

        constexpr void deallocate(_In_opt_ _Post_ptr_invalid_ ElemT* ptr, _In_ const std::size_t elems) const
        {
            return PerformDeallocation(AllocFlag::NoTracking, ptr, elems * sizeof(ElemT));
        }
    };

    template <typename ElemT>
    struct TrackingAllocator : std::allocator<ElemT>
    {
        [[nodiscard]] _Ret_notnull_ _Post_writable_size_(elems) constexpr ElemT* allocate(_In_ const std::size_t elems) const
        {
            return static_cast<ElemT*>(PerformAllocation(AllocFlag::None, elems * sizeof(ElemT)));
        }

        constexpr void deallocate(_In_opt_ _Post_ptr_invalid_ ElemT* ptr, _In_ const std::size_t elems) const
        {
            return PerformDeallocation(AllocFlag::None, ptr, elems * sizeof(ElemT));
        }
    };
}


// Example operator new/delete overloads for your program:
/*

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

*/
