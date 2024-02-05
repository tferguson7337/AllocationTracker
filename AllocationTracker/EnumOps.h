#pragma once

///
//
//  Defines light-weight, common arithmetic, logical, and bit-wise operators for enum types.
//
//  Users are responsible for ensuring that the operator they use for an enum is coherent.
//  For instance, it's likely nonsensical to use increment operations on an enum intended to be used
//  as a bit-mask, or bit-wise operators on enums that are intended to not be used as a bit-mask.
//
//  Users are responsible for ensuring that final value obtained from operation is coherent.
//
//  Users are responsible for ensuring that values used for operation are coherent.
//  For instance: No divide-by-zero, bit-wise shifting by negative values, etc...
//
//  Users are responsible for ensuring that values used in operations do not result in undesired over/underflow.
//
//  It's also recommended to limit the scope of where this header file is included in your project
//  to avoid defining operators for enums that you do not wish to have defined. So avoid including
//  this file in other header files.
//
//  For bit-wise operators, the operators will internally convert the underlying type to unsigned
//  before performing the operation.  This is to avoid issues where the MSB==1 signifies a negative value and 
//  shifting the bits right will indefinitely shift the MSB right until all bits are 1.  The assumption is that
//  the caller does not wish to invoke this type of behavior when shifting, since the default underlying enum 
//  type is typically signed (int).
//
///

// SAL
#include <sal.h>

// C++
#include <concepts>
#include <type_traits>

// Helper Macros
#define ENUM_OPS_UT_ std::underlying_type_t<EnumT>
#define ENUM_OPS_USUT_ std::make_unsigned_t<ENUM_OPS_UT_>

namespace EnumOps
{
    template <typename TypeT>
    concept IsValidType = std::is_enum_v<TypeT>;
}

/// Increment Operators \\\

// Prefix Increment
template <EnumOps::IsValidType EnumT>
inline constexpr EnumT& operator++(_Inout_ EnumT& e) noexcept
{
    e = static_cast<EnumT>(static_cast<ENUM_OPS_UT_>(e) + static_cast<ENUM_OPS_UT_>(1));
    return e;
}

// Postfix Increment
template <EnumOps::IsValidType EnumT>
inline constexpr EnumT operator++(_Inout_ EnumT& e, int) noexcept
{
    const EnumT old = e;
    ++e;
    return old;
}


/// Decrement Operators \\\

// Prefix Decrement
template <EnumOps::IsValidType EnumT>
inline constexpr EnumT& operator--(_Inout_ EnumT& e) noexcept
{
    e = static_cast<EnumT>(static_cast<ENUM_OPS_UT_>(e) - static_cast<ENUM_OPS_UT_>(1));
    return e;
}

// Postfix Decrement
template <EnumOps::IsValidType EnumT>
inline constexpr EnumT operator--(_Inout_ EnumT& e, int) noexcept
{
    const EnumT old = e;
    --e;
    return old;
}


/// Addition Operators \\\

// Enum + Enum
template <EnumOps::IsValidType EnumT>
[[nodiscard]] inline constexpr EnumT operator+(_In_ const EnumT e1, _In_ const EnumT e2) noexcept
{
    return static_cast<EnumT>(static_cast<ENUM_OPS_UT_>(e1) + static_cast<ENUM_OPS_UT_>(e2));
}


/// Add-Assign Operators \\\

// Enum += Enum
template <EnumOps::IsValidType EnumT>
inline constexpr EnumT& operator+=(_Inout_ EnumT& e1, _In_ const EnumT e2) noexcept
{
    e1 = e1 + e2;
    return e1;
}


/// Subtraction Operators \\\

// Enum - Enum
template <EnumOps::IsValidType EnumT>
[[nodiscard]] inline constexpr EnumT operator-(_In_ const EnumT e1, _In_ const EnumT e2) noexcept
{
    return static_cast<EnumT>(static_cast<ENUM_OPS_UT_>(e1) - static_cast<ENUM_OPS_UT_>(e2));
}


/// Subtract-Assign Operators \\\

// Enum -= Enum
template <EnumOps::IsValidType EnumT>
inline constexpr EnumT& operator-=(_Inout_ EnumT& e1, _In_ const EnumT e2) noexcept
{
    e1 = e1 - e2;
    return e1;
}


/// Multiplication Operators \\\

// Enum * Enum
template <EnumOps::IsValidType EnumT>
[[nodiscard]] inline constexpr EnumT operator*(_In_ const EnumT e1, _In_ const EnumT e2) noexcept
{
    return static_cast<EnumT>(static_cast<ENUM_OPS_UT_>(e1) * static_cast<ENUM_OPS_UT_>(e2));
}


/// Multiplication-Assign Operator \\\

// Enum *= Enum
template <EnumOps::IsValidType EnumT>
inline constexpr EnumT& operator*=(_Inout_ EnumT& e1, _In_ const EnumT e2) noexcept
{
    e1 = e1 * e2;
    return e1;
}


/// Division Operators \\\

// Enum / Enum
template <EnumOps::IsValidType EnumT>
[[nodiscard]] inline constexpr EnumT operator/(_In_ const EnumT e1, _In_ const EnumT e2) noexcept
{
    return static_cast<EnumT>(static_cast<ENUM_OPS_UT_>(e1) / static_cast<ENUM_OPS_UT_>(e2));
}


/// Division-Assign Operator \\\

// Enum /= Enum
template <EnumOps::IsValidType EnumT>
inline constexpr EnumT& operator/=(_Inout_ EnumT& e1, _In_ const EnumT e2) noexcept
{
    e1 = e1 / e2;
    return e1;
}


/// Logical NOT \\\

template <EnumOps::IsValidType EnumT>
[[nodiscard]] inline constexpr bool operator!(_In_ const EnumT e) noexcept
{
    return static_cast<ENUM_OPS_UT_>(e) == 0;
}


/// Bit-wise NOT \\\

template <EnumOps::IsValidType EnumT>
[[nodiscard]] inline constexpr EnumT operator~(_In_ const EnumT e) noexcept
{
    return static_cast<EnumT>(~static_cast<ENUM_OPS_USUT_>(e));
}


/// Bit-wise AND \\\

template <EnumOps::IsValidType EnumT>
[[nodiscard]] inline constexpr EnumT operator&(_In_ const EnumT e1, _In_ const EnumT e2) noexcept
{
    return static_cast<EnumT>(static_cast<ENUM_OPS_USUT_>(e1) & static_cast<ENUM_OPS_USUT_>(e2));
}


/// Bit-wise AND-Assign \\\

template <EnumOps::IsValidType EnumT>
inline constexpr EnumT& operator&=(_Inout_ EnumT& e1, _In_ const EnumT e2) noexcept
{
    e1 = e1 & e2;
    return e1;
}


/// Bit-wise OR \\\

template <EnumOps::IsValidType EnumT>
[[nodiscard]] inline constexpr EnumT operator|(_In_ const EnumT e1, _In_ const EnumT e2) noexcept
{
    return static_cast<EnumT>(static_cast<ENUM_OPS_USUT_>(e1) | static_cast<ENUM_OPS_USUT_>(e2));
}


/// Bit-wise OR-Assign \\\

template <EnumOps::IsValidType EnumT>
inline constexpr EnumT& operator|=(_Inout_ EnumT& e1, _In_ const EnumT e2) noexcept
{
    e1 = e1 | e2;
    return e1;
}


/// Bit-wise XOR \\\

template <EnumOps::IsValidType EnumT>
[[nodiscard]] inline constexpr EnumT operator^(_In_ const EnumT e1, _In_ const EnumT e2) noexcept
{
    return static_cast<EnumT>(static_cast<ENUM_OPS_USUT_>(e1) ^ static_cast<ENUM_OPS_USUT_>(e2));
}


/// Bit-wise XOR-Assign \\\

template <EnumOps::IsValidType EnumT>
inline constexpr EnumT& operator^=(_Inout_ EnumT& e1, _In_ const EnumT e2) noexcept
{
    e1 = e1 ^ e2;
    return e1;
}


/// Bit-wise Shift Left Operators \\\

template <EnumOps::IsValidType EnumT, std::integral IntegralT>
[[nodiscard]] inline constexpr EnumT operator<<(_In_ const EnumT e, _In_range_(0, 63) const IntegralT d) noexcept
{
    return static_cast<EnumT>(static_cast<ENUM_OPS_USUT_>(e) << static_cast<ENUM_OPS_USUT_>(d));
}


/// Bit-wise Shift-Left-Assign Operators \\\

template <EnumOps::IsValidType EnumT, std::integral IntegralT>
inline constexpr EnumT& operator<<=(_Inout_ EnumT& e, _In_range_(0, 63) const IntegralT d) noexcept
{
    e = e << d;
    return e;
}


/// Bit-wise Shift Right Operators \\\

template <EnumOps::IsValidType EnumT, std::integral IntegralT>
[[nodiscard]] inline constexpr EnumT operator>>(_In_ const EnumT e, _In_range_(0, 63) const IntegralT d) noexcept
{
    return static_cast<EnumT>(static_cast<ENUM_OPS_USUT_>(e) >> static_cast<ENUM_OPS_USUT_>(d));
}


/// Bit-wise Shift-Right-Assign Operators \\\

template <EnumOps::IsValidType EnumT, std::integral IntegralT>
inline constexpr EnumT& operator>>=(_Inout_ EnumT& e, _In_range_(0, 63) const IntegralT d) noexcept
{
    e = e >> d;
    return e;
}


// Cleanup helper macros
#undef ENUM_OPS_USUT_
#undef ENUM_OPS_UT_
