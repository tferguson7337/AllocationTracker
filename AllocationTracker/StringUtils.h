#pragma once

#if defined(_WIN32)

#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#define STR_UTIL_CLEANUP_WIN32_LEAN_AND_MEAN_DEFINE__
#endif

#include <Windows.h>
#else
// Linux header for string conversions?
#endif

#include <algorithm>
#include <array>
#include <concepts>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>


//
//
// TODO: Header file --> Module //
//
//


namespace StringUtils
{
    // Common Types/Concepts //

    template <typename CharT>
    concept ValidCharType =
        std::same_as<CharT, char> ||
        std::same_as<CharT, wchar_t> ||
        std::same_as<CharT, char8_t> ||
        std::same_as<CharT, char16_t> ||
        std::same_as<CharT, char32_t>;

    using DefaultCharType =
#if defined (STRING_UTILS_DEFAULT_CHAR_TYPE__)
        STRING_UTILS_DEFAULT_CHAR_TYPE__;
#else
        char;
#endif
    static_assert(ValidCharType<DefaultCharType>);

    template <typename ContainerT>
    concept ContainerIteratorTypeIsInputIterator = requires (ContainerT cont, const ContainerT constCont)
    {
        requires std::input_iterator<decltype(std::begin(cont))>;
        requires std::input_iterator<decltype(std::cbegin(cont))>;

        requires std::input_iterator<decltype(std::begin(constCont))>;
        requires std::input_iterator<decltype(std::cbegin(constCont))>;
    };

    template <typename ContainerT>
    concept ContainerSupportsForwardIteration = requires (ContainerT cont, const ContainerT constCont)
    {
        requires std::forward_iterator<decltype(std::begin(cont))>;
        requires std::forward_iterator<decltype(std::cbegin(cont))>;

        requires std::forward_iterator<decltype(std::begin(constCont))>;
        requires std::forward_iterator<decltype(std::cbegin(constCont))>;
    };

    template <typename ContainerT>
    concept ContainerSupportsBidirectionalIteration = requires (ContainerT cont, const ContainerT constCont)
    {
        requires ContainerSupportsForwardIteration<ContainerT>;

        requires std::bidirectional_iterator<decltype(std::begin(cont))>;
        requires std::bidirectional_iterator<decltype(std::cbegin(cont))>;

        requires std::bidirectional_iterator<decltype(std::begin(constCont))>;
        requires std::bidirectional_iterator<decltype(std::cbegin(constCont))>;
    };

    template <typename ContainerT>
    concept ContainerSupportsRandomAccessIteration = requires (ContainerT cont, const ContainerT constCont)
    {
        requires ContainerSupportsBidirectionalIteration<ContainerT>;

        requires std::random_access_iterator<decltype(std::begin(cont))>;
        requires std::random_access_iterator<decltype(std::cbegin(cont))>;

        requires std::random_access_iterator<decltype(std::begin(constCont))>;
        requires std::random_access_iterator<decltype(std::cbegin(constCont))>;
    };

    template <typename ContainerT>
    concept ContainerSupportsContiguousIteration = requires (ContainerT cont, const ContainerT constCont)
    {
        requires ContainerSupportsRandomAccessIteration<ContainerT>;

        requires std::contiguous_iterator<decltype(std::begin(cont))>;
        requires std::contiguous_iterator<decltype(std::cbegin(cont))>;

        requires std::contiguous_iterator<decltype(std::begin(constCont))>;
        requires std::contiguous_iterator<decltype(std::cbegin(constCont))>;
    };


    template <ValidCharType CharT, bool bIsConst>
    struct [[nodiscard]] StringIterator
    {
        using ThisT = StringIterator<CharT, bIsConst>;
        using OtherT = StringIterator<CharT, !bIsConst>;

        using difference_type = std::ptrdiff_t;
        using value_type = CharT;
        using pointer = std::conditional_t<bIsConst, const value_type*, value_type*>;
        using reference = std::conditional_t<bIsConst, const value_type&, value_type&>;
        using iterator_concept = std::contiguous_iterator_tag;
        using iterator_category = std::random_access_iterator_tag;

        pointer m_pData{nullptr};

        constexpr StringIterator(_In_opt_ pointer p = nullptr) noexcept : m_pData{p} { }
        constexpr StringIterator(const ThisT&) noexcept = default;
        constexpr StringIterator(_In_ const OtherT& other) noexcept : m_pData{other.m_pData} { }
        constexpr ~StringIterator() noexcept = default;
        constexpr StringIterator& operator=(const ThisT&) noexcept = default;
        constexpr StringIterator& operator=(_In_ const OtherT& other) noexcept
        {
            m_pData = const_cast<pointer>(other.m_pData);
            return *this;
        }

        // Forward Iterator //
        [[nodiscard]] constexpr bool operator==(_In_ const ThisT&) const noexcept = default;
        [[nodiscard]] constexpr bool operator==(_In_ const OtherT& other) const noexcept
        {
            return m_pData == other.m_pData;
        }
        [[nodiscard]] constexpr std::strong_ordering operator<=>(_In_ const ThisT&) const noexcept = default;
        [[nodiscard]] constexpr std::strong_ordering operator<=>(_In_ const OtherT& other) const noexcept
        {
            return m_pData <=> other.m_pData;
        }
        [[nodiscard]] constexpr reference operator*() const noexcept
        {
            return *m_pData;
        }
        constexpr pointer operator->() const noexcept
        {
            return m_pData;
        }
        constexpr StringIterator& operator++() noexcept
        {
            ++m_pData;
            return *this;
        }
        constexpr StringIterator operator++(int) noexcept
        {
            return StringIterator{m_pData++};
        }

        // Bidirectional Iterator //
        constexpr StringIterator& operator--() noexcept
        {
            --m_pData;
            return *this;
        }
        constexpr StringIterator operator--(int) noexcept
        {
            return StringIterator{m_pData--};
        }

        // RandomAccess Iterator //
        [[nodiscard]] friend constexpr StringIterator operator+(_In_ const StringIterator itr, _In_ const difference_type offset) noexcept
        {
            return StringIterator{itr.m_pData + offset};
        }
        [[nodiscard]] friend constexpr StringIterator operator+(_In_ const difference_type offset, _In_ const StringIterator itr) noexcept
        {
            return itr + offset;
        }
        constexpr StringIterator& operator+=(_In_ const difference_type offset) noexcept
        {
            m_pData += offset;
            return *this;
        }
        [[nodiscard]] friend constexpr StringIterator operator-(_In_ const StringIterator itr, _In_ const difference_type offset) noexcept
        {
            return StringIterator{itr.m_pData - offset};
        }
        [[nodiscard]] friend constexpr difference_type operator-(_In_ const StringIterator itr, _In_ const StringIterator other) noexcept
        {
            return itr.m_pData - other.m_pData;
        }
        constexpr StringIterator& operator-=(_In_ const difference_type offset) noexcept
        {
            m_pData -= offset;
            return *this;
        }
        [[nodiscard]] constexpr reference operator[](_In_ const difference_type offset) const noexcept
        {
            return m_pData[offset];
        }
    };


    template <typename CharArrayT>
    concept ValidCharArrayType = requires(CharArrayT arr, const CharArrayT constArr)
    {
        typename CharArrayT::value_type;
        requires ValidCharType<typename CharArrayT::value_type>;

        requires ContainerSupportsContiguousIteration<CharArrayT>;

        { constArr.Capacity() } noexcept -> std::same_as<std::uint32_t>;
        { constArr.Length() } noexcept -> std::same_as<std::uint32_t>;
        { arr.Data() } noexcept -> std::same_as<std::add_pointer_t<typename CharArrayT::value_type>>;
        { constArr.Data() } noexcept -> std::same_as<std::add_pointer_t<std::add_const_t<typename CharArrayT::value_type>>>;
        { constArr.ToStringView() } noexcept -> std::same_as<std::basic_string_view<typename CharArrayT::value_type>>;
        { constArr.ToString() } -> std::same_as<std::basic_string<typename CharArrayT::value_type>>;
    };

    // Note: Building out a string using this class is intended for internal use only.
    //       External receivers of an object of this class should use this as const only, and should likely use `auto`.
    template <ValidCharType CharT, std::uint32_t MaxLength>
    struct [[nodiscard]] CharArray
    {
        static_assert(MaxLength > 0, "Zero length array is not supported");
        using value_type = CharT;

        std::uint32_t m_Length{};
        CharT m_CharArray[MaxLength]{};

        constexpr CharArray() noexcept = default;

        template <ValidCharType OtherCharT, std::size_t OtherMaxLength>
        constexpr CharArray(_In_ const CharArray<OtherCharT, OtherMaxLength>& other) noexcept
        {
            *this = other;
        }

        constexpr ~CharArray() noexcept = default;

        template <ValidCharType OtherCharT, std::size_t OtherMaxLength>
        constexpr CharArray& operator=(_In_ const CharArray<OtherCharT, OtherMaxLength>& other) const noexcept
        {
            static_assert(std::same_as<CharT, OtherCharT>);
            static_assert(MaxLength >= OtherMaxLength);
            if (this != &other)
            {
                m_Length = other.m_Length;
                std::ranges::copy(other, m_CharArray + MaxLength - other.m_Length);
            }

            return *this;
        }

        template <ValidCharType OtherCharT, std::size_t OtherMaxLength>
        [[nodiscard]] constexpr bool operator==(_In_ const CharArray<OtherCharT, OtherMaxLength>& other) const noexcept
        {
            static_assert(std::same_as<CharT, OtherCharT>);
            return std::ranges::equal(*this, other);
        }

        [[nodiscard]] constexpr std::uint32_t Capacity() const noexcept { return MaxLength; }
        [[nodiscard]] constexpr std::uint32_t Length() const noexcept { return m_Length; }
        [[nodiscard]] constexpr CharT* Data() noexcept { return m_CharArray + MaxLength - m_Length; }
        [[nodiscard]] constexpr const CharT* Data() const noexcept { return m_CharArray + MaxLength - m_Length; }

        [[nodiscard]] constexpr std::basic_string_view<CharT> ToStringView() const noexcept
        {
            return std::basic_string_view<CharT>{Data(), m_Length};
        }

        [[nodiscard]] constexpr std::basic_string<CharT> ToString() const
        {
            return std::basic_string<CharT>{Data(), m_Length};
        }

        using IteratorT = StringIterator<CharT, false>;
        using ConstIteratorT = StringIterator<CharT, true>;

        using ReverseIteratorT = std::reverse_iterator<StringIterator<CharT, false>>;
        using ConstReverseIteratorT = std::reverse_iterator<StringIterator<CharT, true>>;

        [[nodiscard]] constexpr IteratorT begin() noexcept { return IteratorT(m_CharArray + MaxLength - m_Length); }
        [[nodiscard]] constexpr IteratorT end() noexcept { return IteratorT(m_CharArray + MaxLength); }

        [[nodiscard]] constexpr ConstIteratorT begin() const noexcept { return ConstIteratorT(m_CharArray + MaxLength - m_Length); }
        [[nodiscard]] constexpr ConstIteratorT end() const noexcept { return ConstIteratorT(m_CharArray + MaxLength); }

        [[nodiscard]] constexpr ConstIteratorT cbegin() const noexcept { return ConstIteratorT(m_CharArray + MaxLength - m_Length); }
        [[nodiscard]] constexpr ConstIteratorT cend() const noexcept { return ConstIteratorT(m_CharArray + MaxLength); }

        [[nodiscard]] constexpr ReverseIteratorT rbegin() noexcept { return ReverseIteratorT(end()); }
        [[nodiscard]] constexpr ReverseIteratorT rend() noexcept { return ReverseIteratorT(begin()); }

        [[nodiscard]] constexpr ConstReverseIteratorT rbegin() const noexcept { return ConstReverseIteratorT(end()); }
        [[nodiscard]] constexpr ConstReverseIteratorT rend() const noexcept { return ConstReverseIteratorT(begin()); }

        [[nodiscard]] constexpr ConstReverseIteratorT crbegin() const noexcept { return ConstReverseIteratorT(end()); }
        [[nodiscard]] constexpr ConstReverseIteratorT crend() const noexcept { return ConstReverseIteratorT{begin()}; }
    };
    static_assert(ValidCharArrayType<CharArray<DefaultCharType, 1>>);
}

// Comparison Operators - StringUtils::CharArray vs Iterable Container //

[[nodiscard]] constexpr bool operator==(_In_ const StringUtils::ValidCharArrayType auto& charArr, _In_ const StringUtils::ContainerSupportsForwardIteration auto& cont) noexcept
{
    using CharArrT = std::remove_reference_t<decltype(charArr)>;
    using ContainerItrT = decltype(std::begin(cont));
    static_assert(std::same_as<typename CharArrT::value_type, typename ContainerItrT::value_type>);
    return std::ranges::equal(charArr, cont);
}

[[nodiscard]] constexpr bool operator!=(_In_ const StringUtils::ValidCharArrayType auto& charArr, _In_ const StringUtils::ContainerSupportsForwardIteration auto& cont) noexcept
{
    return !(charArr == cont);
}

[[nodiscard]] constexpr bool operator<(_In_ const StringUtils::ValidCharArrayType auto& charArr, _In_ const StringUtils::ContainerSupportsForwardIteration auto& cont) noexcept
{
    using CharArrT = std::remove_reference_t<decltype(charArr)>;
    using ContainerItrT = decltype(std::begin(cont));
    static_assert(std::same_as<typename CharArrT::value_type, typename ContainerItrT::value_type>);
    return std::ranges::lexicographical_compare(charArr, cont);
}

[[nodiscard]] constexpr bool operator>=(_In_ const StringUtils::ValidCharArrayType auto& charArr, _In_ const StringUtils::ContainerSupportsForwardIteration auto& cont) noexcept
{
    return !(charArr < cont);
}

[[nodiscard]] constexpr bool operator>(_In_ const StringUtils::ValidCharArrayType auto& charArr, _In_ const StringUtils::ContainerSupportsForwardIteration auto& cont) noexcept
{
    using CharArrT = std::remove_reference_t<decltype(charArr)>;
    using ContainerItrT = decltype(std::begin(cont));
    static_assert(std::same_as<typename CharArrT::value_type, typename ContainerItrT::value_type>);
    return std::ranges::lexicographical_compare(cont, charArr);
}

[[nodiscard]] constexpr bool operator<=(_In_ const StringUtils::ValidCharArrayType auto& charArr, _In_ const StringUtils::ContainerSupportsForwardIteration auto& cont) noexcept
{
    return !(charArr > cont);
}


// Comparison Operators - Iterable Container vs StringUtils::CharArray //

[[nodiscard]] constexpr bool operator==(_In_ const StringUtils::ContainerSupportsForwardIteration auto& cont, _In_ const StringUtils::ValidCharArrayType auto& charArr) noexcept
{
    using CharArrT = std::remove_reference_t<decltype(charArr)>;
    using ContainerItrT = decltype(std::begin(cont));
    static_assert(std::same_as<typename CharArrT::value_type, typename ContainerItrT::value_type>);
    return std::ranges::equal(cont, charArr);
}

[[nodiscard]] constexpr bool operator!=(_In_ const StringUtils::ContainerSupportsForwardIteration auto& cont, _In_ const StringUtils::ValidCharArrayType auto& charArr) noexcept
{
    return !(cont == charArr);
}

[[nodiscard]] constexpr bool operator<(_In_ const StringUtils::ContainerSupportsForwardIteration auto& cont, _In_ const StringUtils::ValidCharArrayType auto& charArr) noexcept
{
    using CharArrT = std::remove_reference_t<decltype(charArr)>;
    using ContainerItrT = decltype(std::begin(cont));
    static_assert(std::same_as<typename CharArrT::value_type, typename ContainerItrT::value_type>);
    return std::ranges::lexicographical_compare(cont, charArr);
}

[[nodiscard]] constexpr bool operator>=(_In_ const StringUtils::ContainerSupportsForwardIteration auto& cont, _In_ const StringUtils::ValidCharArrayType auto& charArr) noexcept
{
    return !(cont < charArr);
}

[[nodiscard]] constexpr bool operator>(_In_ const StringUtils::ContainerSupportsForwardIteration auto& cont, _In_ const StringUtils::ValidCharArrayType auto& charArr) noexcept
{
    using CharArrT = std::remove_reference_t<decltype(charArr)>;
    using ContainerItrT = decltype(std::begin(cont));
    static_assert(std::same_as<typename CharArrT::value_type, typename ContainerItrT::value_type>);
    return std::ranges::lexicographical_compare(charArr, cont);
}

[[nodiscard]] constexpr bool operator<=(_In_ const StringUtils::ContainerSupportsForwardIteration auto& cont, _In_ const StringUtils::ValidCharArrayType auto& charArr) noexcept
{
    return !(cont > charArr);
}


// Formatter overload for numeric conversion's return type.
template <StringUtils::ValidCharType CharT, std::uint32_t MaxLength>
struct std::formatter<StringUtils::CharArray<CharT, MaxLength>, CharT> : public std::formatter<std::basic_string_view<CharT>, CharT>
{
    template <typename FormatContext>
    constexpr auto format(_In_ const StringUtils::CharArray<CharT, MaxLength>& charArray, _Inout_ FormatContext& fmtCtx) const
    {
        return std::formatter<std::basic_string_view<CharT>, CharT>::format(charArray.ToStringView(), fmtCtx);
    }
};

template <typename StreamT, StringUtils::ValidCharType CharT, std::size_t MaxLength>
constexpr StreamT& operator<<(_Inout_ StreamT& stream, _In_ const StringUtils::CharArray<CharT, MaxLength>& charArr)
{
    stream << charArr.ToStringView();
    return stream;
}


namespace StringUtils
{
    // UTF8 <--> UTF16 Conversions //

    template <typename CharT>
    concept ValidUTFConversionCharType =
        ValidCharType<CharT>
#if defined(_WIN32)
        && !std::same_as<char32_t, CharT> /* No UTF-32 conversion support for Windows */
#endif
        ;

#if defined(_WIN32)
    template <ValidUTFConversionCharType ToCharT, ValidUTFConversionCharType FromCharT>
    [[nodiscard]] std::basic_string<ToCharT> ConvertTo_UTFImpl_(_In_ const std::basic_string_view<FromCharT> strView)
    {
        static constexpr bool s_bConvertingToUTF8{std::same_as<ToCharT, char> || std::same_as<ToCharT, char8_t>};
        static constexpr bool s_bConvertingToUTF16{std::same_as<ToCharT, wchar_t> || std::same_as<ToCharT, char16_t>};

        static constexpr bool s_bConvertingFromUTF8{std::same_as<FromCharT, char> || std::same_as<FromCharT, char8_t>};
        static constexpr bool s_bConvertingFromUTF16{std::same_as<FromCharT, wchar_t> || std::same_as<FromCharT, char16_t>};

        static_assert(!s_bConvertingToUTF8 || s_bConvertingFromUTF16, "Invalid conversion - UTF8 --> UTF8");
        static_assert(!s_bConvertingToUTF16 || s_bConvertingFromUTF8, "Invalid conversion - UTF16 --> UTF16");

        if (strView.empty()) { return {}; }

        const int requiredLen = [strView]()
        {
            if constexpr (s_bConvertingToUTF8)
            {
                return ::WideCharToMultiByte(
                    CP_UTF8, WC_ERR_INVALID_CHARS,
                    reinterpret_cast<const wchar_t*>(strView.data()), static_cast<int>(strView.length()),
                    nullptr, 0,
                    nullptr, nullptr);
            }
            else
            {
                return ::MultiByteToWideChar(
                    CP_UTF8, MB_ERR_INVALID_CHARS,
                    reinterpret_cast<const char*>(strView.data()), static_cast<int>(strView.length()),
                    nullptr, 0);
            }
        }();

        if (requiredLen == 0) { return {}; }

        std::basic_string<ToCharT> ret(static_cast<std::size_t>(requiredLen), ToCharT{});
        if constexpr (s_bConvertingToUTF8)
        {
            if (::WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS,
                reinterpret_cast<const wchar_t*>(strView.data()), static_cast<int>(strView.length()),
                reinterpret_cast<char*>(ret.data()), static_cast<int>(ret.size()),
                nullptr, nullptr) == 0)
            {
                return {};
            }
        }
        else
        {
            if (::MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS,
                reinterpret_cast<const char*>(strView.data()), static_cast<int>(strView.length()),
                reinterpret_cast<wchar_t*>(ret.data()), static_cast<int>(ret.size())) == 0)
            {
                return {};
            }
        }

        return ret;
    }
#else
    template <ValidUTFConversionCharType ToCharT, ValidUTFConversionCharType FromCharT>
    std::basic_string<ToCharT> ConvertToImpl(_In_ const std::basic_string<FromCharT>& str)
    {
        // Naive impl.
        // TODO: Implement proper UTF conversion for non-Windows.
        std::basic_string<ToCharT> ret;
        ret.reserve(str.size());
        std::for_each(str.cbegin(), str.cend(), [&ret](const FromCharT c) { ret.push_back(static_cast<ToCharT>(c)); });
        return ret;
    }
#endif


    template <typename ContainerT, typename ToCharT>
    concept ValidConversionContainerType = requires (ContainerT cont)
    {
        !std::is_pointer_v<ContainerT>;
        !std::is_array_v<ContainerT>;
        requires ContainerSupportsContiguousIteration<ContainerT>;
        requires !std::same_as<ToCharT, typename decltype(cont.begin())::value_type>;
    };

    // Converts UTF8 --> UTF16 or UTF16 --> UTF8.
    template <ValidUTFConversionCharType ToCharT>
    struct [[nodiscard]] ToUTFConverter
    {
        consteval ToUTFConverter() noexcept = default;
        constexpr ~ToUTFConverter() noexcept = default;

        template <ValidUTFConversionCharType FromCharT>
        [[nodiscard]] std::basic_string<ToCharT> operator()(_In_ const std::basic_string_view<FromCharT> sv) const
        {
            return ConvertTo_UTFImpl_<ToCharT>(sv);
        }

        template <ValidUTFConversionCharType FromCharT>
        [[nodiscard]] std::basic_string<ToCharT> operator()(_In_z_ const FromCharT* pStr) const
        {
            return operator()(std::basic_string_view<FromCharT>{pStr});
        }

        template <ValidUTFConversionCharType FromCharT>
        [[nodiscard]] std::basic_string<ToCharT> operator()(_In_count_(len) const FromCharT* str, _In_ const std::size_t len) const
        {
            return operator()(std::basic_string_view<FromCharT>{str, len});
        }

        [[nodiscard]] std::basic_string<ToCharT> operator()(_In_ const ValidConversionContainerType auto& container) const
        {
            using ContainerValueType = decltype(std::begin(container))::value_type;
            const std::basic_string_view<ContainerValueType> view(std::begin(container), std::end(container));
            return operator()(view);
        }
    };

#if defined(_WIN32)
    using ToUTF8Converter = ToUTFConverter<char>;
    using ToUTF16Converter = ToUTFConverter<wchar_t>;
#endif
}


namespace StringUtils
{
    // Numeric --> String Conversions //
    namespace Numeric
    {
        template <typename BaseTypeT>
        concept ValidBaseType = requires (const BaseTypeT base)
        {
            { base() } noexcept -> std::same_as<std::uint32_t>;
            { (2 <= base()) && (base() <= 36) };
        };

        template <std::uint32_t BaseIntegral_>
        struct Base
        {
            constexpr std::uint32_t operator()() const noexcept
            {
                return BaseIntegral_;
            }

            template <ValidCharType CharT>
            static constexpr auto BaseStr() noexcept
            {
                static_assert(2 <= BaseIntegral_ && BaseIntegral_ <= 36);
                return std::array
                {
                    CharT{'B'}, CharT{'a'}, CharT{'s'}, CharT{'e'},
                    static_cast<CharT>(CharT{'0'} + (BaseIntegral_ / 10)),
                    static_cast<CharT>(CharT{'0'} + (BaseIntegral_ % 10))
                };
            }
        };
        static_assert(ValidBaseType<Base<3>>);

        using BinaryBase = Base<2>;
        static_assert(ValidBaseType<BinaryBase>);

        using OctalBase = Base<8>;
        static_assert(ValidBaseType<OctalBase>);

        using DecimalBase = Base<10>;
        static_assert(ValidBaseType<DecimalBase>);

        using HexadecimalBase = Base<16>;
        static_assert(ValidBaseType<HexadecimalBase>);

        template <typename ContainerT, typename CharT>
        concept ValidRandomAccessCharContainerType = requires (const ContainerT cont)
        {
            requires StringUtils::ContainerSupportsRandomAccessIteration<ContainerT>;
            requires StringUtils::ValidCharType<CharT>;

            typename ContainerT::value_type;
            requires std::same_as<typename ContainerT::value_type, CharT>;
        };

        // Defines valid return types for a BaseConverterConfig's `GetPrefix` function.
        template <typename GetPrefixReturnT, typename CharT>
        concept ValidToBaseConverterConfigGetPrefixReturnType = requires
        {
            requires StringUtils::ValidCharType<CharT>;
            requires ValidRandomAccessCharContainerType<GetPrefixReturnT, CharT> ||
            std::same_as<GetPrefixReturnT, std::pair<CharT, CharT>> ||
            std::same_as<GetPrefixReturnT, CharT> ||
            std::same_as<GetPrefixReturnT, std::nullopt_t>;
        };

        template <StringUtils::ValidCharType CharT, bool bIncludeNulls>
        [[nodiscard]] consteval std::size_t GetBaseConverterPrefixLength(
            _In_ const ValidToBaseConverterConfigGetPrefixReturnType<CharT> auto& prefix) noexcept
        {
            using PrefixT = std::remove_cvref_t<decltype(prefix)>;
            if constexpr (ValidRandomAccessCharContainerType<PrefixT, CharT>)
            {
                if constexpr (bIncludeNulls)
                {
                    return std::ranges::size(prefix);
                }
                else
                {
                    std::size_t len = 0;
                    for (const auto c : prefix)
                    {
                        if (c == CharT{})
                        {
                            break;
                        }

                        ++len;
                    }

                    return len;
                }
            }
            else if constexpr (std::same_as<std::pair<CharT, CharT>, PrefixT>)
            {
                return 2;
            }
            else if constexpr (std::same_as<CharT, PrefixT>)
            {
                return 1;
            }
            else
            {
                static_assert(std::is_same_v<std::nullopt_t, PrefixT>, "Unsupported Prefix type");
                return 0;
            }
        }

        template <StringUtils::ValidCharType CharT>
        constexpr void CopyBaseConverterPrefixToOutputPtr(
            _Inout_ CharT* pOut,
            _In_ const ValidToBaseConverterConfigGetPrefixReturnType<CharT> auto& prefix) noexcept
        {
            using PrefixT = std::remove_cvref_t<decltype(prefix)>;
            if constexpr (ValidRandomAccessCharContainerType<PrefixT, CharT>)
            {
                for (const auto c : prefix)
                {
                    if (c == CharT{})
                    {
                        continue;
                    }

                    *(pOut++) = c;
                }
            }
            else if constexpr (std::same_as<std::pair<CharT, CharT>, PrefixT>)
            {
#pragma warning(push)
#pragma warning(disable : 6386)
                pOut[0] = prefix.first;
                pOut[1] = prefix.second;
#pragma warning(pop)
            }
            else if constexpr (std::same_as<CharT, PrefixT>)
            {
                *pOut = prefix;
            }
            else
            {
                static_assert(std::is_same_v<std::nullopt_t, PrefixT>, "Unsupported Prefix type");
            }
        }

        // Defines valid return types for a BaseConverterConfig's `GetSuffix` function.
        template <typename GetSuffixReturnT, typename CharT>
        concept ValidToBaseConverterConfigGetSuffixReturnType = ValidToBaseConverterConfigGetPrefixReturnType<GetSuffixReturnT, CharT>;

        template <StringUtils::ValidCharType CharT, bool bIncludeNull>
        [[nodiscard]] consteval std::size_t GetBaseConverterSuffixLength(
            _In_ const ValidToBaseConverterConfigGetSuffixReturnType<CharT> auto& suffix) noexcept
        {
            return GetBaseConverterPrefixLength<CharT, bIncludeNull>(suffix);
        }

        template <StringUtils::ValidCharType CharT>
        constexpr void CopyBaseConverterSuffixToOutputPtr(
            _Out_ CharT* pOut,
            _In_ const ValidToBaseConverterConfigGetSuffixReturnType<CharT> auto& suffix) noexcept
        {
            return CopyBaseConverterPrefixToOutputPtr<CharT>(pOut, suffix);
        }

        // Defines valid return types for a BaseConverterConfig's `GetSeparator` function.
        template <typename GetSeparatorReturnT, typename CharT>
        concept ValidToBaseConverterConfigGetSeparatorReturnType = requires
        {
            requires StringUtils::ValidCharType<CharT>;
            requires ValidRandomAccessCharContainerType<GetSeparatorReturnT, CharT> ||
            std::same_as<GetSeparatorReturnT, std::pair<CharT, CharT>> ||
            std::same_as<GetSeparatorReturnT, CharT> ||
            std::same_as<GetSeparatorReturnT, std::nullopt_t>;
        };

        // Defines API requirements for built-in and custom ToBaseConverter Configs types.
        template <typename ConfigT, typename BaseT, typename CharT>
        concept ValidToBaseConverterConfig = requires (const ConfigT config)
        {
            requires StringUtils::Numeric::ValidBaseType<BaseT>;
            requires StringUtils::ValidCharType<CharT>;

            // Requirements - GetPrefix<BaseT, CharT>:
            //  Must be noexcept.
            //  Return type must be valid as defined by supporting concept.
            { config.template GetPrefix<BaseT, CharT>() } noexcept;
            requires ValidToBaseConverterConfigGetPrefixReturnType<decltype(config.template GetPrefix<BaseT, CharT>()), CharT>;

            // Requirements - GetSuffix<BaseT, CharT>:
            //  Must be noexcept.
            //  Return type must be valid as defined by supporting concept.
            { config.template GetSuffix<BaseT, CharT>() } noexcept;
            requires ValidToBaseConverterConfigGetSuffixReturnType<decltype(config.template GetSuffix<BaseT, CharT>()), CharT>;

            // Requirements - GetSeparator<BaseT, CharT>:
            //  Must be noexcept.
            //  Return type must be valid as defined by supporting concept.
            { config.template GetSeparator<BaseT, CharT>() } noexcept;
            requires ValidToBaseConverterConfigGetSeparatorReturnType<decltype(config.template GetSeparator<BaseT, CharT>()), CharT>;

            // Requirements - GetSeparatorFrequency<BaseT>:
            //  Must be noexcept.
            //  Must return std::size_t.
            //  When BaseT is BinaryBase, function must return a value that is evenly divisible by 4 (e.g., 0, 4, 8, etc.).
            //  When BaseT is OctalBase, function must return a value that is evenly divisible by 3 (e.g., 0, 3, 6, etc.).
            //  When BaseT is DecimalBase, function must return either 0 or 3.
            //  When BaseT is HexadecimalBase, function must return a value that is evenly divisible by 4 (e.g., 0, 4, 8, etc.).
            { config.template GetSeparatorFrequency<BaseT>() } noexcept -> std::same_as<std::size_t>;
            { (config.template GetSeparatorFrequency<BinaryBase>() % 4) == 0 } noexcept;
            { (config.template GetSeparatorFrequency<OctalBase>() % 3) == 0 } noexcept;
            { ((config.template GetSeparatorFrequency<DecimalBase>() == 0) || (config.template GetSeparatorFrequency<DecimalBase>() == 3)) } noexcept;
            { (config.template GetSeparatorFrequency<HexadecimalBase>() % 4) == 0 } noexcept;

            // Requirements - ShouldDoZeroPadding<BaseT>:
            //  Must be noexcept
            //  Must return bool
            { config.template ShouldDoZeroPadding<BaseT>() } noexcept -> std::same_as<bool>;
        };

        namespace ToBaseConverterConfigsV1
        {
            struct [[nodiscard]] MinimalConfig
            {
                consteval MinimalConfig() noexcept = default;
                constexpr ~MinimalConfig() noexcept = default;

                template <ValidBaseType BaseT, ValidCharType CharT>
                [[nodiscard]] consteval auto GetPrefix() const noexcept
                {
                    return std::nullopt;
                }

                template <ValidBaseType BaseT, ValidCharType CharT>
                [[nodiscard]] consteval auto GetSuffix() const noexcept
                {
                    return std::nullopt;
                }

                template <ValidBaseType BaseT, ValidCharType CharT>
                [[nodiscard]] consteval auto GetSeparator() const noexcept
                {
                    return std::nullopt;
                }

                template <ValidBaseType BaseT>
                [[nodiscard]] consteval std::size_t GetSeparatorFrequency() const noexcept
                {
                    return 0;
                }

                template <ValidBaseType BaseT>
                [[nodiscard]] consteval bool ShouldDoZeroPadding() const noexcept
                {
                    return false;
                }
            };
            static_assert(ValidToBaseConverterConfig<MinimalConfig, BinaryBase, DefaultCharType>);
            static_assert(ValidToBaseConverterConfig<MinimalConfig, OctalBase, DefaultCharType>);
            static_assert(ValidToBaseConverterConfig<MinimalConfig, DecimalBase, DefaultCharType>);
            static_assert(ValidToBaseConverterConfig<MinimalConfig, HexadecimalBase, DefaultCharType>);

            struct [[nodiscard]] FancyConfig
            {
                consteval FancyConfig() noexcept = default;
                constexpr ~FancyConfig() noexcept = default;

                template <ValidBaseType BaseT, ValidCharType CharT>
                [[nodiscard]] consteval auto GetPrefix() const noexcept
                {
                    if constexpr (std::is_same_v<BaseT, BinaryBase>)
                    {
                        return std::make_pair(static_cast<CharT>('0'), static_cast<CharT>('b'));
                    }
                    else if constexpr (std::is_same_v<BaseT, OctalBase>)
                    {
                        return std::make_pair(static_cast<CharT>('0'), static_cast<CharT>('o'));
                    }
                    else if constexpr (std::is_same_v<BaseT, DecimalBase>)
                    {
                        return std::make_pair(static_cast<CharT>('0'), static_cast<CharT>('n'));
                    }
                    else if constexpr (std::is_same_v<BaseT, HexadecimalBase>)
                    {
                        return std::make_pair(static_cast<CharT>('0'), static_cast<CharT>('x'));
                    }
                    else
                    {
                        return std::array
                        {
                            static_cast<CharT>('B'),
                            static_cast<CharT>('0' + (BaseT{}() / 10)),
                            static_cast<CharT>('0' + (BaseT{}() % 10)),
                            static_cast<CharT>('|')
                        };
                    }
                }

                template <ValidBaseType BaseT, ValidCharType CharT>
                [[nodiscard]] consteval auto GetSuffix() const noexcept
                {
                    return std::nullopt;
                }

                template <ValidBaseType BaseT, ValidCharType CharT>
                [[nodiscard]] consteval auto GetSeparator() const noexcept
                {
                    if constexpr (std::same_as<BaseT, StringUtils::Numeric::DecimalBase>)
                    {
                        return static_cast<CharT>(',');
                    }
                    else
                    {
                        return static_cast<CharT>('\'');
                    }
                }

                template <ValidBaseType BaseT>
                [[nodiscard]] consteval std::size_t GetSeparatorFrequency() const noexcept
                {
                    if constexpr (std::same_as<BaseT, BinaryBase>) { return 4; }
                    else if constexpr (std::same_as<BaseT, OctalBase>) { return 3; }
                    else if constexpr (std::same_as<BaseT, DecimalBase>) { return 3; }
                    else if constexpr (std::same_as<BaseT, HexadecimalBase>) { return 4; }
                    else { return 4; }
                }

                template <ValidBaseType BaseT>
                [[nodiscard]] consteval bool ShouldDoZeroPadding() const noexcept
                {
                    return std::same_as<BaseT, BinaryBase>
                        || std::same_as<BaseT, OctalBase>
                        || std::same_as<BaseT, HexadecimalBase>;
                }
            };
            static_assert(ValidToBaseConverterConfig<FancyConfig, BinaryBase, DefaultCharType>);
            static_assert(ValidToBaseConverterConfig<FancyConfig, OctalBase, DefaultCharType>);
            static_assert(ValidToBaseConverterConfig<FancyConfig, DecimalBase, DefaultCharType>);
            static_assert(ValidToBaseConverterConfig<FancyConfig, HexadecimalBase, DefaultCharType>);
        }

        using ToBaseConverterMinimalConfig = ToBaseConverterConfigsV1::MinimalConfig;
        using ToBaseConverterFancyConfig = ToBaseConverterConfigsV1::FancyConfig;

        using ToBaseConverterDefaultConfig = ToBaseConverterFancyConfig;
        static_assert(ValidToBaseConverterConfig<ToBaseConverterDefaultConfig, BinaryBase, DefaultCharType>);
        static_assert(ValidToBaseConverterConfig<ToBaseConverterDefaultConfig, OctalBase, DefaultCharType>);
        static_assert(ValidToBaseConverterConfig<ToBaseConverterDefaultConfig, DecimalBase, DefaultCharType>);
        static_assert(ValidToBaseConverterConfig<ToBaseConverterDefaultConfig, HexadecimalBase, DefaultCharType>);


        template <ValidCharType CharT>
        struct [[nodiscard]] DigitToCharConverter
        {
            // Q: Why have this outside of ToBaseConverter?
            // A: We don't want a separate array instance created for each base/char/config combination.
            inline static constexpr std::array s_DigitCharArray
            {
                CharT{'0'}, CharT{'1'}, CharT{'2'}, CharT{'3'},
                CharT{'4'}, CharT{'5'}, CharT{'6'}, CharT{'7'},
                CharT{'8'}, CharT{'9'}, CharT{'a'}, CharT{'b'},
                CharT{'c'}, CharT{'d'}, CharT{'e'}, CharT{'f'},
                CharT{'g'}, CharT{'h'}, CharT{'i'}, CharT{'j'},
                CharT{'k'}, CharT{'l'}, CharT{'m'}, CharT{'n'},
                CharT{'o'}, CharT{'p'}, CharT{'q'}, CharT{'r'},
                CharT{'s'}, CharT{'t'}, CharT{'u'}, CharT{'v'},
                CharT{'w'}, CharT{'x'}, CharT{'y'}, CharT{'z'}
            };

            inline static constexpr std::array s_BinarySequenceCharArrays
            {
                std::array{CharT{'0'}, CharT{'0'}, CharT{'0'}, CharT{'0'}},
                std::array{CharT{'0'}, CharT{'0'}, CharT{'0'}, CharT{'1'}},
                std::array{CharT{'0'}, CharT{'0'}, CharT{'1'}, CharT{'0'}},
                std::array{CharT{'0'}, CharT{'0'}, CharT{'1'}, CharT{'1'}},
                std::array{CharT{'0'}, CharT{'1'}, CharT{'0'}, CharT{'0'}},
                std::array{CharT{'0'}, CharT{'1'}, CharT{'0'}, CharT{'1'}},
                std::array{CharT{'0'}, CharT{'1'}, CharT{'1'}, CharT{'0'}},
                std::array{CharT{'0'}, CharT{'1'}, CharT{'1'}, CharT{'1'}},
                std::array{CharT{'1'}, CharT{'0'}, CharT{'0'}, CharT{'0'}},
                std::array{CharT{'1'}, CharT{'0'}, CharT{'0'}, CharT{'1'}},
                std::array{CharT{'1'}, CharT{'0'}, CharT{'1'}, CharT{'0'}},
                std::array{CharT{'1'}, CharT{'0'}, CharT{'1'}, CharT{'1'}},
                std::array{CharT{'1'}, CharT{'1'}, CharT{'0'}, CharT{'0'}},
                std::array{CharT{'1'}, CharT{'1'}, CharT{'0'}, CharT{'1'}},
                std::array{CharT{'1'}, CharT{'1'}, CharT{'1'}, CharT{'0'}},
                std::array{CharT{'1'}, CharT{'1'}, CharT{'1'}, CharT{'1'}}
            };

            consteval DigitToCharConverter() noexcept = default;
            constexpr ~DigitToCharConverter() noexcept = default;

            [[nodiscard]] constexpr CharT GetDigit(_In_ const std::integral auto digit) const noexcept
            {
                return s_DigitCharArray[digit];
            }

            template <ValidToBaseConverterConfig<BinaryBase, CharT> ConfigT, bool bFullSequence>
            [[nodiscard]] constexpr auto GetBinarySequence(_In_ const std::unsigned_integral auto bits) const noexcept
            {
                const auto& binarySequence = s_BinarySequenceCharArrays[bits];
                if constexpr (ConfigT{}.template ShouldDoZeroPadding<BinaryBase>())
                {
                    return binarySequence;
                }
                else if constexpr (bFullSequence)
                {
                    return std::basic_string_view<CharT>{binarySequence.cbegin(), binarySequence.cend()};
                }
                else
                {
                    auto GetIteratorOffset = [bits]() -> std::size_t
                    {
                        switch (bits)
                        {
                        case 0b0000:
                        case 0b0001:
                            return 3;
                        case 0b0010:
                        case 0b0011:
                            return 2;
                        case 0b0100:
                        case 0b0101:
                        case 0b0110:
                        case 0b0111:
                            return 1;
                        }

                        return 0;
                    };

                    return std::basic_string_view<CharT>{std::next(binarySequence.cbegin(), GetIteratorOffset()), binarySequence.cend()};
                }
            }
        };


        template <typename IntegralT>
        concept ValidToBaseConverterIntegral = std::integral<IntegralT> || std::is_enum_v<IntegralT> || std::is_pointer_v<IntegralT>;

        template <ValidBaseType ToBaseT, ValidCharType ToCharT = DefaultCharType, ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = ToBaseConverterDefaultConfig>
        class [[nodiscard]] ToBaseConverter
        {
        private:
            using ThisT = ToBaseConverter<ToBaseT, ToCharT, ConfigT>;

            template <bool bIncludeNull>
            [[nodiscard]] static consteval std::size_t GetPrefixLength() noexcept
            {
                return GetBaseConverterPrefixLength<ToCharT, bIncludeNull>(ConfigT{}.template GetPrefix<ToBaseT, ToCharT>());
            }

            template <bool bIncludeNull>
            [[nodiscard]] static consteval std::size_t GetSuffixLength() noexcept
            {
                return GetBaseConverterSuffixLength<ToCharT, bIncludeNull>(ConfigT{}.template GetSuffix<ToBaseT, ToCharT>());
            }

            static constexpr void ApplyPrefix(_Inout_ auto& itr) noexcept
            {
                itr -= GetPrefixLength<false>();
                constexpr auto cPrefix = ConfigT{}.template GetPrefix<ToBaseT, ToCharT>();
                CopyBaseConverterPrefixToOutputPtr(itr, cPrefix);
            }

            static constexpr void ApplySuffix(_Inout_ auto& itr) noexcept
            {
                itr -= GetSuffixLength<false>();
                constexpr auto cSuffix = ConfigT{}.template GetSuffix<ToBaseT, ToCharT>();
                CopyBaseConverterSuffixToOutputPtr(itr, cSuffix);
            }

            [[nodiscard]] static consteval std::size_t GetSeparatorLength() noexcept
            {
                using SeparatorT = decltype(ConfigT{}.template GetSeparator<ToBaseT, ToCharT>());
                if constexpr (ValidRandomAccessCharContainerType<SeparatorT, ToCharT>)
                {
                    return std::ranges::size(ConfigT{}.template GetSeparator<ToBaseT, ToCharT>());
                }
                else if constexpr (std::same_as<SeparatorT, std::pair<ToCharT, ToCharT>>)
                {
                    return 2;
                }
                else if constexpr (std::same_as<SeparatorT, ToCharT>)
                {
                    return 1;
                }
                else
                {
                    static_assert(std::same_as<SeparatorT, std::nullopt_t>);
                    return 0;
                }
            }

            [[nodiscard]] static consteval std::size_t GetSeparatorFrequency() noexcept
            {
                return ConfigT{}.template GetSeparatorFrequency<ToBaseT>();
            }

            static constexpr void ApplySeparator(_Inout_ auto& itr) noexcept
            {
                itr -= GetSeparatorLength();

                constexpr const auto& cSeparator = ConfigT{}.template GetSeparator<ToBaseT, ToCharT>();
                using SeparatorT = std::remove_cvref_t<decltype(cSeparator)>;
                if constexpr (ValidRandomAccessCharContainerType<SeparatorT, ToCharT>)
                {
                    std::ranges::copy(cSeparator, itr);
                }
                else if constexpr (std::same_as<SeparatorT, std::pair<ToCharT, ToCharT>>)
                {
                    itr[0] = cSeparator.first;
                    itr[1] = cSeparator.second;
                }
                else if constexpr (std::same_as<SeparatorT, ToCharT>)
                {
                    *itr = cSeparator;
                }
                else
                {
                    static_assert(std::same_as<SeparatorT, std::nullopt_t>);
                    // no-op.
                }
            }

            [[nodiscard]] static consteval bool IsBaseUnusual() noexcept
            {
                return !std::same_as<ToBaseT, BinaryBase>
                    && !std::same_as<ToBaseT, OctalBase>
                    && !std::same_as<ToBaseT, DecimalBase>
                    && !std::same_as<ToBaseT, HexadecimalBase>;
            }

            [[nodiscard]] static consteval bool IsBasePower2() noexcept
            {
                return (std::same_as<ToBaseT, Base<2>>)
                    || (std::same_as<ToBaseT, Base<4>>)
                    || (std::same_as<ToBaseT, Base<8>>)
                    || (std::same_as<ToBaseT, Base<16>>)
                    || (std::same_as<ToBaseT, Base<32>>);
            }

            [[nodiscard]] static consteval std::uint32_t GetBasePower2AdjustAmount() noexcept
            {
                static_assert(IsBasePower2());
                if constexpr (std::same_as<ToBaseT, Base<2>>)
                {
                    // Binary is handled in chunks of 4 bits.
                    return 4;
                }
                else if constexpr (std::same_as<ToBaseT, Base<4>>)
                {
                    return 2;
                }
                else if constexpr (std::same_as<ToBaseT, Base<8>>)
                {
                    return 3;
                }
                else if constexpr (std::same_as<ToBaseT, Base<16>>)
                {
                    return 4;
                }
                else
                {
                    static_assert(std::same_as<ToBaseT, Base<32>>);
                    return 5;
                }
            }

            template <std::integral IntegralT, bool bIncludeNegativeSign = true>
            [[nodiscard]] static consteval std::size_t GetRequiredMaxLength() noexcept
                requires (((std::same_as<ToBaseT, DecimalBase> || IsBaseUnusual()) && std::integral<IntegralT>) || std::unsigned_integral<IntegralT>)
            {
                constexpr std::size_t cPrefixLength{GetPrefixLength<true>()};
                constexpr std::size_t cSuffixLength{GetSuffixLength<true>()};

                constexpr auto CalculateTotalSeparatorsLength = []([[maybe_unused]] const std::size_t totalDigits) -> std::size_t
                {
                    constexpr std::size_t cSeparatorLength{GetSeparatorLength()};
                    constexpr std::size_t cSeparatorFrequency{GetSeparatorFrequency()};
                    if constexpr ((cSeparatorLength == 0) || (cSeparatorFrequency == 0))
                    {
                        return 0;
                    }
                    else
                    {
                        const std::size_t totalSeparators = (totalDigits / cSeparatorFrequency) - (((totalDigits % cSeparatorFrequency) == 0) ? 1 : 0);
                        return totalSeparators * cSeparatorLength;
                    }
                };

                if constexpr (std::same_as<ToBaseT, BinaryBase>)
                {
                    constexpr std::size_t cTotalDigits = (sizeof(IntegralT) * 8);
                    return cPrefixLength + cTotalDigits + CalculateTotalSeparatorsLength(cTotalDigits) + cSuffixLength;
                }
                else if constexpr (std::same_as<ToBaseT, OctalBase>)
                {
                    constexpr std::size_t cFullBitTripletCount = (sizeof(IntegralT) * 8) / 3;
                    constexpr std::size_t cPartialBitTripletCount = (((sizeof(IntegralT) * 8) % 3) != 0) ? 1 : 0;
                    constexpr std::size_t cTotalDigits = cFullBitTripletCount + cPartialBitTripletCount;
                    return cPrefixLength + cTotalDigits + CalculateTotalSeparatorsLength(cTotalDigits) + cSuffixLength;
                }
                else if constexpr (std::same_as<ToBaseT, DecimalBase>)
                {
                    constexpr std::size_t cTotalDigits = []() -> std::size_t
                    {
                        if constexpr (sizeof(IntegralT) == 1) { return 3; }
                        else if constexpr (sizeof(IntegralT) == 2) { return 5; }
                        else if constexpr (sizeof(IntegralT) == 4) { return 10; }
                        else { static_assert(sizeof(IntegralT) == 8); return 20; }
                    }();
                    constexpr std::size_t cNegativeSign{bIncludeNegativeSign ? 1 : 0};
                    return cPrefixLength + cNegativeSign + cTotalDigits + CalculateTotalSeparatorsLength(cTotalDigits) + cSuffixLength;
                }
                else if constexpr (std::same_as<ToBaseT, HexadecimalBase>)
                {
                    constexpr std::size_t cTotalDigits = (sizeof(IntegralT) * 2);
                    return cPrefixLength + cTotalDigits + CalculateTotalSeparatorsLength(cTotalDigits) + cSuffixLength;
                }
                else
                {
                    static_assert(IsBaseUnusual());
                    constexpr std::size_t cTotalDigits = []() -> std::size_t
                    {
                        if constexpr (sizeof(IntegralT) == 1) { return 6; }
                        else if constexpr (sizeof(IntegralT) == 2) { return 11; }
                        else if constexpr (sizeof(IntegralT) == 4) { return 21; }
                        else { static_assert(sizeof(IntegralT) == 8); return 41; }
                    }();
                    constexpr std::size_t cNegativeSign{1};
                    return cPrefixLength + cNegativeSign + cTotalDigits + CalculateTotalSeparatorsLength(cTotalDigits) + cSuffixLength;
                }
            }

            template <std::integral IntegralT>
            [[nodiscard]] static consteval auto GetDigitCharMask() noexcept
            {
                if constexpr (std::same_as<ToBaseT, BinaryBase>) { return IntegralT{0b1111}; } // Note: binary is handled in 4-bit chunks.
                else if constexpr (std::same_as<ToBaseT, OctalBase>) { return IntegralT{07}; }
                else if constexpr (std::same_as<ToBaseT, DecimalBase>) { return IntegralT{10}; }
                else if constexpr (std::same_as<ToBaseT, HexadecimalBase>) { return IntegralT{0xf}; }
                else { return IntegralT{ToBaseT{}()}; }
            }

            template <std::integral IntegralT>
            [[nodiscard]] static consteval IntegralT GetAdjustAmount() noexcept
            {
                if constexpr (IsBasePower2()) { return GetBasePower2AdjustAmount(); }
                else { return static_cast<IntegralT>(ToBaseT{}()); }
            }

            template <std::integral IntegralT, bool bNegative = false>
            [[nodiscard]] static constexpr auto GetNextCharDigitOffset(_In_ const IntegralT num) noexcept
            {
                if constexpr (std::same_as<ToBaseT, DecimalBase> || IsBaseUnusual())
                {
                    if constexpr (std::is_signed_v<IntegralT> && bNegative)
                    {
                        return static_cast<ToCharT>(-(num % GetDigitCharMask<IntegralT>()));
                    }
                    else
                    {
                        return static_cast<ToCharT>(num % GetDigitCharMask<IntegralT>());
                    }
                }
                else if constexpr (std::same_as<ToBaseT, BinaryBase> || std::same_as<ToBaseT, HexadecimalBase>)
                {
                    return static_cast<IntegralT>(num & GetDigitCharMask<IntegralT>());
                }
                else
                {
                    return static_cast<ToCharT>(num & GetDigitCharMask<IntegralT>());
                }
            }

            template <std::integral IntegralT, bool bNegative = false>
            [[nodiscard]] static constexpr auto GetNextDigit(_In_ const IntegralT num) noexcept requires (!std::same_as<ToBaseT, BinaryBase>)
            {
                return DigitToCharConverter<ToCharT>{}.GetDigit(GetNextCharDigitOffset<IntegralT, bNegative>(num));
                /**
                if constexpr (ToBaseT{}() >= 10u)
                {
                    return DigitToCharConverter<ToCharT>{}.GetDigit(GetNextCharDigitOffset<IntegralT, bNegative>(num));
                }
                else
                {
                    return static_cast<ToCharT>(ToCharT{'0'} + GetNextCharDigitOffset<IntegralT, bNegative>(num));
                }
                /**/
            }

            template <std::unsigned_integral UIntegralT, bool bFullSequence = true>
            [[nodiscard]] static constexpr auto GetNextDigitSequence(_In_ const UIntegralT num) noexcept requires std::same_as<ToBaseT, BinaryBase>
            {
                return DigitToCharConverter<ToCharT>{}.template GetBinarySequence<ConfigT, bFullSequence>(GetNextCharDigitOffset<UIntegralT, false>(num));
            }

            template <std::integral IntegralT>
            [[nodiscard]] static constexpr void AdjustNumber(_Inout_ IntegralT& num) noexcept
            {
                if constexpr (IsBasePower2()) { num >>= GetAdjustAmount<IntegralT>(); }
                else { num /= GetAdjustAmount<IntegralT>(); }
            }

            template <std::integral IntegralT, bool bNegative = false>
            [[nodiscard]] static constexpr auto PerformConversionLoop(_Inout_ ToCharT*& itr, _In_ IntegralT num) noexcept
                requires ((GetSeparatorFrequency() == 0) || (GetSeparatorLength() == 0))
            {
                // Simple conversion - binary/octal/+decimal/hexadecimal - no separators

                if constexpr (ConfigT{}.template ShouldDoZeroPadding<ToBaseT>())
                {
                    // Zero-padding enabled.
                    const ToCharT* const end = itr - GetRequiredMaxLength<IntegralT, false>() + GetPrefixLength<false>() + GetSuffixLength<true>();
                    do
                    {
                        if constexpr (std::same_as<ToBaseT, BinaryBase>)
                        {
                            itr -= GetAdjustAmount<std::ptrdiff_t>();
                            std::ranges::copy(GetNextDigitSequence(num), itr);
                        }
                        else
                        {
                            *(--itr) = GetNextDigit<IntegralT, bNegative>(num);
                        }
                        AdjustNumber(num);
                    } while (itr != end);
                }
                else
                {
                    constexpr IntegralT cLoopEndIntegral = []()
                    {
                        return std::same_as<ToBaseT, BinaryBase> ? IntegralT{0b1111} : IntegralT{};
                    }();

                    // Zero-padding disabled.
                    do
                    {
                        if constexpr (std::same_as<ToBaseT, BinaryBase>)
                        {
                            itr -= GetAdjustAmount<std::ptrdiff_t>();
                            std::ranges::copy(GetNextDigitSequence(num), itr);
                        }
                        else
                        {
                            *(--itr) = GetNextDigit<IntegralT, bNegative>(num);
                        }
                        AdjustNumber(num);

                    } while ([num]() { if constexpr (bNegative) { return num < cLoopEndIntegral; } else { return num > cLoopEndIntegral; }}());

                    if constexpr (std::same_as<ToBaseT, BinaryBase>)
                    {
                        for (const auto digitChar : GetNextDigitSequence<IntegralT, false>(num))
                        {
                            *(--itr) = digitChar;
                        }
                    }
                }
            }

            template <std::integral IntegralT, bool bNegative = false>
            [[nodiscard]] static constexpr auto PerformConversionLoop(_Inout_ ToCharT*& itr, _In_ IntegralT num) noexcept
            {
                // We have a separator to account for, which complicates implementation.
                std::size_t separatorCounter = 0;
                if constexpr (ConfigT{}.template ShouldDoZeroPadding<ToBaseT>())
                {
                    // Zero-padding enabled.
                    static_assert(!bNegative || std::is_same_v<ToBaseT, DecimalBase>);

                    const ToCharT* const end = itr - GetRequiredMaxLength<IntegralT, false>() + GetPrefixLength<false>() + GetSuffixLength<true>();

                    do
                    {
                        if (separatorCounter == GetSeparatorFrequency())
                        {
                            separatorCounter = 0;
                            ApplySeparator(itr);
                        }

                        if constexpr (std::same_as<ToBaseT, BinaryBase>)
                        {
                            itr -= (GetAdjustAmount<std::ptrdiff_t>());
                            std::ranges::copy(GetNextDigitSequence(num), itr);
                            separatorCounter += GetAdjustAmount<std::size_t>();
                        }
                        else
                        {
                            *(--itr) = GetNextDigit<IntegralT, bNegative>(num);
                            ++separatorCounter;
                        }
                        AdjustNumber(num);
                    } while (itr != end);
                }
                else
                {
                    // Zero-padding disabled.
                    do
                    {
                        if (separatorCounter == GetSeparatorFrequency())
                        {
                            separatorCounter = 0;
                            ApplySeparator(itr);
                        }

                        if constexpr (std::same_as<ToBaseT, BinaryBase>)
                        {
                            const auto& digitSequence = [num]()
                            {
                                if (num >= GetDigitCharMask<IntegralT>())
                                {
                                    return GetNextDigitSequence<IntegralT, true>(num);
                                }
                                else
                                {
                                    return GetNextDigitSequence<IntegralT, false>(num);
                                }
                            }();
                            itr -= digitSequence.size();
                            separatorCounter += digitSequence.size();
                            std::ranges::copy(digitSequence, itr);
                        }
                        else
                        {
                            *(--itr) = GetNextDigit<IntegralT, bNegative>(num);
                            ++separatorCounter;
                        }
                        AdjustNumber(num);
                    } while (num != IntegralT{});
                }
            }

            template <std::integral IntegralT, bool bNegative = false>
            [[nodiscard]] static constexpr auto ConvertToBase(_In_ const IntegralT num) noexcept
            {
                CharArray<ToCharT, GetRequiredMaxLength<IntegralT>()> str;
                ToCharT* itr = str.m_CharArray + str.Capacity();

                ApplySuffix(itr);

                PerformConversionLoop<IntegralT, bNegative>(itr, num);

                if constexpr (bNegative)
                {
                    *(--itr) = ToCharT{'-'};
                }

                ApplyPrefix(itr);

                str.m_Length = static_cast<std::uint32_t>(str.m_CharArray + str.Capacity() - itr);

                return str;
            }

            template <std::signed_integral SIntegralT>
            [[nodiscard]] static constexpr auto ConvertToBaseSignedRouter(_In_ const SIntegralT num) noexcept
            {
                // operator() should have routed all other cases to the work-horse ConvertToBase call already.
                static_assert(std::same_as<ToBaseT, DecimalBase> || IsBaseUnusual());

                if (num >= 0)
                {
                    using UIntegralT = std::make_unsigned_t<SIntegralT>;
                    return ConvertToBase<UIntegralT, false>(static_cast<UIntegralT>(num));
                }

                return ConvertToBase<SIntegralT, true>(num);
            }

        public:

            consteval ToBaseConverter() noexcept = default;
            constexpr ~ToBaseConverter() noexcept = default;

            template <ValidToBaseConverterIntegral IntegralT>
            [[nodiscard]] constexpr auto operator()(_In_ const IntegralT num) const noexcept
            {
                if constexpr (std::is_enum_v<IntegralT>)
                {
                    return this->operator()(static_cast<std::underlying_type_t<IntegralT>>(num));
                }
                else if constexpr (std::is_pointer_v<IntegralT>)
                {
                    using UIntegralT = typename std::conditional_t<sizeof(IntegralT) == 4, std::uint32_t, std::uint64_t>;
                    return this->operator()(reinterpret_cast<UIntegralT>(num));
                }
                else if constexpr (std::unsigned_integral<IntegralT> || (!std::same_as<ToBaseT, DecimalBase> && !IsBaseUnusual()))
                {
                    using UIntegralT = std::make_unsigned_t<IntegralT>;
                    return ConvertToBase(static_cast<UIntegralT>(num));
                }
                else
                {
                    return ConvertToBaseSignedRouter(num);
                }
            }
        };

        template <ValidCharType ToCharT = DefaultCharType, ValidToBaseConverterConfig<BinaryBase, ToCharT> ConfigT = ToBaseConverterDefaultConfig>
        using ToBinaryConverter = ToBaseConverter<BinaryBase, ToCharT, ConfigT>;

        template <ValidCharType ToCharT = DefaultCharType, ValidToBaseConverterConfig<OctalBase, ToCharT> ConfigT = ToBaseConverterDefaultConfig>
        using ToOctalConverter = ToBaseConverter<OctalBase, ToCharT, ConfigT>;

        template <ValidCharType ToCharT = DefaultCharType, ValidToBaseConverterConfig<DecimalBase, ToCharT> ConfigT = ToBaseConverterDefaultConfig>
        using ToDecimalConverter = ToBaseConverter<DecimalBase, ToCharT, ConfigT>;

        template <ValidCharType ToCharT = DefaultCharType, ValidToBaseConverterConfig<HexadecimalBase, ToCharT> ConfigT = ToBaseConverterDefaultConfig>
        using ToHexadecimalConverter = ToBaseConverter<HexadecimalBase, ToCharT, ConfigT>;

        template <ValidCharType ToCharT = DefaultCharType, ValidToBaseConverterConfig<BinaryBase, ToCharT> ConfigT = ToBaseConverterDefaultConfig>
        [[nodiscard]] constexpr auto ToBinary(_In_ const ValidToBaseConverterIntegral auto num) noexcept
        {
            return ToBinaryConverter<ToCharT, ConfigT>{}(num);
        }

        template <ValidCharType ToCharT = DefaultCharType, ValidToBaseConverterConfig<OctalBase, ToCharT> ConfigT = ToBaseConverterDefaultConfig>
        [[nodiscard]] constexpr auto ToOctal(_In_ const ValidToBaseConverterIntegral auto num) noexcept
        {
            return ToOctalConverter<ToCharT, ConfigT>{}(num);
        }

        template <ValidCharType ToCharT = DefaultCharType, ValidToBaseConverterConfig<DecimalBase, ToCharT> ConfigT = ToBaseConverterDefaultConfig>
        [[nodiscard]] constexpr auto ToDecimal(_In_ const ValidToBaseConverterIntegral auto num) noexcept
        {
            return ToDecimalConverter<ToCharT, ConfigT>{}(num);
        }

        template <ValidCharType ToCharT = DefaultCharType, ValidToBaseConverterConfig<HexadecimalBase, ToCharT> ConfigT = ToBaseConverterDefaultConfig>
        [[nodiscard]] constexpr auto ToHexadecimal(_In_ const ValidToBaseConverterIntegral auto num) noexcept
        {
            return ToHexadecimalConverter<ToCharT, ConfigT>{}(num);
        }
    }
}

namespace StringUtils
{
    namespace Formatting
    {
        namespace Numeric
        {
            template <
                StringUtils::Numeric::ValidBaseType ToBaseT,
                ValidCharType ToCharT = DefaultCharType,
                StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
            struct [[nodiscard]] ToBaseFormatter
            {
                [[nodiscard]] constexpr auto operator()(_In_ const StringUtils::Numeric::ValidToBaseConverterIntegral auto num) const noexcept
                {
                    return StringUtils::Numeric::ToBaseConverter<ToBaseT, ToCharT, ConfigT>{}(num);
                }
            };

            template <
                std::uint32_t BaseIntegral_,
                ValidCharType ToCharT = DefaultCharType,
                StringUtils::Numeric::ValidToBaseConverterConfig<StringUtils::Numeric::Base<BaseIntegral_>,
                ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
            using ToBaseX = ToBaseFormatter<StringUtils::Numeric::Base<BaseIntegral_>, ToCharT, ConfigT>;

            template <
                ValidCharType ToCharT = DefaultCharType,
                StringUtils::Numeric::ValidToBaseConverterConfig<StringUtils::Numeric::BinaryBase, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
            using Bin = ToBaseFormatter<StringUtils::Numeric::BinaryBase, ToCharT, ConfigT>;

            template <
                ValidCharType ToCharT = DefaultCharType,
                StringUtils::Numeric::ValidToBaseConverterConfig<StringUtils::Numeric::OctalBase, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
            using Oct = ToBaseFormatter<StringUtils::Numeric::OctalBase, ToCharT, ConfigT>;

            template <
                ValidCharType ToCharT = DefaultCharType,
                StringUtils::Numeric::ValidToBaseConverterConfig<StringUtils::Numeric::DecimalBase, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
            using Dec = ToBaseFormatter<StringUtils::Numeric::DecimalBase, ToCharT, ConfigT>;

            template <
                ValidCharType ToCharT = DefaultCharType,
                StringUtils::Numeric::ValidToBaseConverterConfig<StringUtils::Numeric::HexadecimalBase, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
            using Hex = ToBaseFormatter<StringUtils::Numeric::HexadecimalBase, ToCharT, ConfigT>;
        }
    }

    // Convenience namespace alias (e.g., StringUtils::Formatting --> StringUtils::Fmt)
    namespace Fmt = ::StringUtils::Formatting;
}

namespace StringUtils
{
    namespace Formatting
    {
        namespace Memory
        {
            namespace UnitTags
            {
                struct Byte    // [   0, 2^10)
                {
                    [[nodiscard]] constexpr std::size_t Magnitude() const noexcept
                    {
                        return 0;
                    }
                    [[nodiscard]] constexpr std::string_view Name() const noexcept
                    {
                        return "Byte";
                    }

                    template <StringUtils::ValidCharType CharT>
                    [[nodiscard]] consteval auto Suffix() const noexcept
                    {
                        return std::array{CharT{' '}, CharT{'B'}, CharT{'\0'}, CharT{'\0'}};
                    }
                };
                struct Kibibyte // [2^10, 2^20)
                {
                    [[nodiscard]] constexpr std::size_t Magnitude() const noexcept
                    {
                        return 1;
                    }
                    [[nodiscard]] constexpr std::string_view Name() const noexcept
                    {
                        return "Kibibyte";
                    }

                    template <StringUtils::ValidCharType CharT>
                    [[nodiscard]] consteval auto Suffix() const noexcept
                    {
                        return std::array{CharT{' '}, CharT{'K'}, CharT{'i'}, CharT{'B'}};
                    }
                };
                struct Mebibyte // [2^20, 2^30)
                {
                    [[nodiscard]] constexpr std::size_t Magnitude() const noexcept
                    {
                        return 2;
                    }
                    [[nodiscard]] constexpr std::string_view Name() const noexcept
                    {
                        return "Mebibyte";
                    }

                    template <StringUtils::ValidCharType CharT>
                    [[nodiscard]] consteval auto Suffix() const noexcept
                    {
                        return std::array{CharT{' '}, CharT{'M'}, CharT{'i'}, CharT{'B'}};
                    }
                };
                struct Gibibyte // [2^30, 2^40)
                {
                    [[nodiscard]] constexpr std::size_t Magnitude() const noexcept
                    {
                        return 3;
                    }
                    [[nodiscard]] constexpr std::string_view Name() const noexcept
                    {
                        return "Gibibyte";
                    }

                    template <StringUtils::ValidCharType CharT>
                    [[nodiscard]] consteval auto Suffix() const noexcept
                    {
                        return std::array{CharT{' '}, CharT{'G'}, CharT{'i'}, CharT{'B'}};
                    }
                };
                struct Tebibyte // [2^40, 2^50)
                {
                    [[nodiscard]] constexpr std::size_t Magnitude() const noexcept
                    {
                        return 4;
                    }
                    [[nodiscard]] constexpr std::string_view Name() const noexcept
                    {
                        return "Tebibyte";
                    }

                    template <StringUtils::ValidCharType CharT>
                    [[nodiscard]] consteval auto Suffix() const noexcept
                    {
                        return std::array{CharT{' '}, CharT{'T'}, CharT{'i'}, CharT{'B'}};
                    }
                };
                struct Pebibyte // [2^50, 2^60)
                {
                    [[nodiscard]] constexpr std::size_t Magnitude() const noexcept
                    {
                        return 5;
                    }
                    [[nodiscard]] constexpr std::string_view Name() const noexcept
                    {
                        return "Pebibyte";
                    }

                    template <StringUtils::ValidCharType CharT>
                    [[nodiscard]] consteval auto Suffix() const noexcept
                    {
                        return std::array{CharT{' '}, CharT{'P'}, CharT{'i'}, CharT{'B'}};
                    }
                };
                struct Exbibyte // [2^60, 2^70)
                {
                    [[nodiscard]] constexpr std::size_t Magnitude() const noexcept
                    {
                        return 6;
                    }
                    [[nodiscard]] constexpr std::string_view Name() const noexcept
                    {
                        return "Exbibyte";
                    }

                    template <StringUtils::ValidCharType CharT>
                    [[nodiscard]] consteval auto Suffix() const noexcept
                    {
                        return std::array{CharT{' '}, CharT{'E'}, CharT{'i'}, CharT{'B'}};
                    }
                };
                struct Zebibyte // [2^70, 2^80)
                {
                    [[nodiscard]] constexpr std::size_t Magnitude() const noexcept
                    {
                        return 7;
                    }
                    [[nodiscard]] constexpr std::string_view Name() const noexcept
                    {
                        return "Zebibyte";
                    }

                    template <StringUtils::ValidCharType CharT>
                    [[nodiscard]] consteval auto Suffix() const noexcept
                    {
                        return std::array{CharT{' '}, CharT{'Z'}, CharT{'i'}, CharT{'B'}};
                    }
                };
                struct Yobibyte // [2^80, 2^90)
                {
                    [[nodiscard]] constexpr std::size_t Magnitude() const noexcept
                    {
                        return 8;
                    }
                    [[nodiscard]] constexpr std::string_view Name() const noexcept
                    {
                        return "Yobibyte";
                    }

                    template <StringUtils::ValidCharType CharT>
                    [[nodiscard]] consteval auto Suffix() const noexcept
                    {
                        return std::array{CharT{' '}, CharT{'Y'}, CharT{'i'}, CharT{'B'}};
                    }
                };

                using MaxUnitSupported = Yobibyte;

                template <std::size_t Mag_>
                using MagnitudeToUnitTagT =
                    std::conditional_t<Mag_ == Byte{}.Magnitude(), Byte,
                    std::conditional_t<Mag_ == Kibibyte{}.Magnitude(), Kibibyte,
                    std::conditional_t<Mag_ == Mebibyte{}.Magnitude(), Mebibyte,
                    std::conditional_t<Mag_ == Gibibyte{}.Magnitude(), Gibibyte,
                    std::conditional_t<Mag_ == Tebibyte{}.Magnitude(), Tebibyte,
                    std::conditional_t<Mag_ == Pebibyte{}.Magnitude(), Pebibyte,
                    std::conditional_t<Mag_ == Exbibyte{}.Magnitude(), Exbibyte,
                    std::conditional_t<Mag_ == Zebibyte{}.Magnitude(), Zebibyte,
                    std::conditional_t<Mag_ == Yobibyte{}.Magnitude(), Yobibyte, void>>>>>>>>>;
                static_assert(std::same_as<Byte, MagnitudeToUnitTagT<Byte{}.Magnitude()>>);
                static_assert(std::same_as<Kibibyte, MagnitudeToUnitTagT<Kibibyte{}.Magnitude()>>);
                static_assert(std::same_as<Mebibyte, MagnitudeToUnitTagT<Mebibyte{}.Magnitude()>>);
                static_assert(std::same_as<Gibibyte, MagnitudeToUnitTagT<Gibibyte{}.Magnitude()>>);
                static_assert(std::same_as<Tebibyte, MagnitudeToUnitTagT<Tebibyte{}.Magnitude()>>);
                static_assert(std::same_as<Pebibyte, MagnitudeToUnitTagT<Pebibyte{}.Magnitude()>>);
                static_assert(std::same_as<Exbibyte, MagnitudeToUnitTagT<Exbibyte{}.Magnitude()>>);
                static_assert(std::same_as<Zebibyte, MagnitudeToUnitTagT<Zebibyte{}.Magnitude()>>);
                static_assert(std::same_as<Yobibyte, MagnitudeToUnitTagT<Yobibyte{}.Magnitude()>>);
                static_assert(std::same_as<MaxUnitSupported, MagnitudeToUnitTagT<MaxUnitSupported{}.Magnitude()>>);

                template <typename UnitTagT>
                concept ValidUnitTag = requires (const UnitTagT tag)
                {
                    requires std::same_as<UnitTagT, Byte> ||
                        std::same_as<UnitTagT, Kibibyte> ||
                        std::same_as<UnitTagT, Mebibyte> ||
                        std::same_as<UnitTagT, Gibibyte> ||
                        std::same_as<UnitTagT, Tebibyte> ||
                        std::same_as<UnitTagT, Pebibyte> ||
                        std::same_as<UnitTagT, Exbibyte> ||
                        std::same_as<UnitTagT, Zebibyte> ||
                        std::same_as<UnitTagT, Yobibyte>;

                    { tag.Magnitude() } noexcept -> std::same_as<std::size_t>;
                    { tag.Name() } noexcept -> std::same_as<std::string_view>;
                    { tag.template Suffix<char>() } noexcept -> StringUtils::ContainerSupportsContiguousIteration;
                    { tag.template Suffix<wchar_t>() } noexcept -> StringUtils::ContainerSupportsContiguousIteration;
                    { tag.template Suffix<char8_t>() } noexcept -> StringUtils::ContainerSupportsContiguousIteration;
                    { tag.template Suffix<char16_t>() } noexcept -> StringUtils::ContainerSupportsContiguousIteration;
                    { tag.template Suffix<char32_t>() } noexcept -> StringUtils::ContainerSupportsContiguousIteration;
                };
            }

            namespace Fixed
            {
                template <
                    UnitTags::ValidUnitTag UnitTagT,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<StringUtils::Numeric::HexadecimalBase, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                struct ToBaseConverterMemConfig : ConfigT
                {
                    template <StringUtils::Numeric::ValidBaseType ToBaseT, ValidCharType CharT>
                    [[nodiscard]] consteval auto GetSuffix() const noexcept
                    {
                        constexpr auto cUnitSuffix{UnitTagT{}.template Suffix<CharT>()};
                        constexpr auto cNumericBaseSuffix{ConfigT{}.template GetSuffix<ToBaseT, ToCharT>()};
                        constexpr auto cNumericBaseSuffixLen{StringUtils::Numeric::GetBaseConverterSuffixLength<CharT, true>(cNumericBaseSuffix)};

                        if constexpr (cNumericBaseSuffixLen == 0)
                        {
                            return cUnitSuffix;
                        }
                        else
                        {
                            std::array<CharT, cNumericBaseSuffixLen + cUnitSuffix.size()> suffix;
                            StringUtils::Numeric::CopyBaseConverterSuffixToOutputPtr(suffix.data(), cNumericBaseSuffix);
                            StringUtils::Numeric::CopyBaseConverterSuffixToOutputPtr(suffix.data() + cNumericBaseSuffixLen, cUnitSuffix);
                            return suffix;
                        }
                    }
                };

                template <
                    UnitTags::ValidUnitTag UnitTagT,
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                struct Converter
                {
                    using InternalConverterT =
                        StringUtils::Numeric::ToBaseConverter<ToBaseT, ToCharT, ToBaseConverterMemConfig<UnitTagT, ToCharT, ConfigT>>;

                    // Note: Intentionally using std concept's here instead of ValidToBaseConverterIntegral.
                    template <std::unsigned_integral UIntegralT>
                    [[nodiscard]] constexpr auto operator()(_In_ const UIntegralT num) const noexcept
                    {
                        return InternalConverterT{}(num);
                    }

                    template <std::signed_integral SIntegralT>
                    [[nodiscard]] constexpr auto operator()(_In_ const SIntegralT num) const noexcept
                    {
                        return operator()(static_cast<std::make_unsigned_t<SIntegralT>>(num));
                    }
                };

                template <
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                using Byte = Converter<UnitTags::Byte, ToBaseT, ToCharT, ConfigT>;

                template <
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                     ValidCharType ToCharT = DefaultCharType,
                     StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                using Kibibyte = Converter<UnitTags::Kibibyte, ToBaseT, ToCharT, ConfigT>;

                template <
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                     ValidCharType ToCharT = DefaultCharType,
                     StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                using Mebibyte = Converter<UnitTags::Mebibyte, ToBaseT, ToCharT, ConfigT>;

                template <
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                using Gibibyte = Converter<UnitTags::Gibibyte, ToBaseT, ToCharT, ConfigT>;

                template <
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                using Tebibyte = Converter<UnitTags::Tebibyte, ToBaseT, ToCharT, ConfigT>;

                template <
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                using Pebibyte = Converter<UnitTags::Pebibyte, ToBaseT, ToCharT, ConfigT>;

                template <
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                using Exbibyte = Converter<UnitTags::Exbibyte, ToBaseT, ToCharT, ConfigT>;

                template <
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                using Zebibyte = Converter<UnitTags::Zebibyte, ToBaseT, ToCharT, ConfigT>;

                template <
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                using Yobibyte = Converter<UnitTags::Yobibyte, ToBaseT, ToCharT, ConfigT>;
            }


            namespace AutoConverting
            {
                struct NoLimitTag
                {
                    [[nodiscard]] constexpr std::size_t Magnitude() const noexcept
                    {
                        return (std::numeric_limits<std::size_t>::max)();
                    }

                    [[nodiscard]] constexpr std::string_view Name() const noexcept
                    {
                        return "NoLimit";
                    }
                };

                template <typename UnitTagT>
                concept ValidLimiterUnitTag = requires (const UnitTagT tag)
                {
                    requires UnitTags::ValidUnitTag<UnitTagT> || std::same_as<UnitTagT, NoLimitTag>;

                    { tag.Magnitude() } noexcept -> std::same_as<std::size_t>;
                    { tag.Name() } noexcept -> std::same_as<std::string_view>;
                };

                template <
                    UnitTags::ValidUnitTag UnitTagT,
                    ValidLimiterUnitTag LimiterUnitTagT = NoLimitTag,
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                struct Converter
                {
                public:

                    template <std::unsigned_integral UIntegralT>
                    [[nodiscard]] constexpr auto operator()(_In_ const UIntegralT num) const noexcept
                    {
                        /**
                        static_assert(UnitTagT{}.Magnitude() <= LimiterUnitTagT{}.Magnitude(),
                            "Cannot auto convert to units of lower magnitude (e.g., MiB --> KiB not allowed)");
                        /**/

                        if constexpr (std::same_as<UnitTagT, LimiterUnitTagT> || std::same_as<UnitTagT, UnitTags::MaxUnitSupported>)
                        {
                            return Fixed::Converter<UnitTagT, ToBaseT, ToCharT, ConfigT>{}(num);
                        }
                        else
                        {
                            if (num >= (1u << 10))
                            {
                                using NextUnitTagT = UnitTags::MagnitudeToUnitTagT < UnitTagT{}.Magnitude() + 1 > ;
                                return Converter<NextUnitTagT, LimiterUnitTagT, ToBaseT, ToCharT, ConfigT>{}(num >> 10);
                            }

                            return Fixed::Converter<UnitTagT, ToBaseT, ToCharT, ConfigT>{}(num);
                        }
                    }

                    template <std::signed_integral SIntegralT>
                    [[nodiscard]] constexpr auto operator()(_In_ const SIntegralT num) const noexcept
                    {
                        return operator()(static_cast<std::make_unsigned_t<SIntegralT>>(num));
                    }
                };

                template <
                    ValidLimiterUnitTag LimiterUnitTagT = NoLimitTag,
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                using Byte = Converter<UnitTags::Byte, LimiterUnitTagT, ToBaseT, ToCharT, ConfigT>;

                template <
                    ValidLimiterUnitTag LimiterUnitTagT = NoLimitTag,
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                using Kibibyte = Converter<UnitTags::Kibibyte, LimiterUnitTagT, ToBaseT, ToCharT, ConfigT>;

                template <
                    ValidLimiterUnitTag LimiterUnitTagT = NoLimitTag,
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                using Mebibyte = Converter<UnitTags::Mebibyte, LimiterUnitTagT, ToBaseT, ToCharT, ConfigT>;

                template <
                    ValidLimiterUnitTag LimiterUnitTagT = NoLimitTag,
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                using Gibibyte = Converter<UnitTags::Gibibyte, LimiterUnitTagT, ToBaseT, ToCharT, ConfigT>;

                template <
                    ValidLimiterUnitTag LimiterUnitTagT = NoLimitTag,
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                using Tebibyte = Converter<UnitTags::Tebibyte, LimiterUnitTagT, ToBaseT, ToCharT, ConfigT>;

                template <
                    ValidLimiterUnitTag LimiterUnitTagT = NoLimitTag,
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                using Pebibyte = Converter<UnitTags::Pebibyte, LimiterUnitTagT, ToBaseT, ToCharT, ConfigT>;

                template <
                    ValidLimiterUnitTag LimiterUnitTagT = NoLimitTag,
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                using Exbibyte = Converter<UnitTags::Exbibyte, LimiterUnitTagT, ToBaseT, ToCharT, ConfigT>;

                template <
                    ValidLimiterUnitTag LimiterUnitTagT = NoLimitTag,
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                using Zebibyte = Converter<UnitTags::Zebibyte, LimiterUnitTagT, ToBaseT, ToCharT, ConfigT>;

                template <
                    ValidLimiterUnitTag LimiterUnitTagT = NoLimitTag,
                    StringUtils::Numeric::ValidBaseType ToBaseT = StringUtils::Numeric::DecimalBase,
                    ValidCharType ToCharT = DefaultCharType,
                    StringUtils::Numeric::ValidToBaseConverterConfig<ToBaseT, ToCharT> ConfigT = StringUtils::Numeric::ToBaseConverterDefaultConfig>
                using Yobibyte = Converter<UnitTags::Yobibyte, LimiterUnitTagT, ToBaseT, ToCharT, ConfigT>;
            }

        #if defined(STR_UTIL_DEFAULT_TO_FIXED_MEMORY_FORMATTERS)
            using namespace Fixed;
        #else
            using namespace AutoConverting;
        #endif
        }
    }
}

#if defined(STR_UTIL_CLEANUP_WIN32_LEAN_AND_MEAN_DEFINE__)
#undef WIN32_LEAN_AND_MEAN
#undef STR_UTIL_CLEANUP_WIN32_LEAN_AND_MEAN_DEFINE__
#endif