#pragma once

#include <sal.h>

#include <functional>
#include <string_view>
#include <vector>

#if defined(ALLOCTRACKER_DLL_EXPORT)
#define ALLOCTRACKER_DECLSPEC_DLL __declspec(dllexport)
#else
#define ALLOCTRACKER_DECLSPEC_DLL __declspec(dllimport)
#endif


namespace AllocationTracking
{
    using LogCallback = std::function<void(_In_ const std::string_view logMsgSV)>;

    enum class LogSummaryType
    {
        Limited,
        Normal,
        FullStackTraces
    };

    using LogAllocationsFn = void(*)(
        _In_ const LogCallback& logFn,
        _In_ const LogSummaryType type);

    using EnableTracking = void(*)(
        _In_ const bool bEnabled);

    using SetTargetModuleNamePrefix = void(*)(
        _In_ const std::string_view prefixSV);

    using RegisterExternalStackEntryMarker = void(*)(
        _In_ const std::string_view markerSV);

    using RegisterExternalStackEntryMarkers = void(*)(
        _In_ const std::vector<std::string_view>& markers);

    using SetCollectFullStackTraces = void(*)(
        _In_ const bool bCollect);
}

extern "C"
{
    ALLOCTRACKER_DECLSPEC_DLL void AllocationTracker_LogAllocations(
        _In_ const AllocationTracking::LogCallback& logFn,
        _In_ const AllocationTracking::LogSummaryType type);

    ALLOCTRACKER_DECLSPEC_DLL void AllocationTracker_EnableTracking(_In_ const bool bEnabled);

    ALLOCTRACKER_DECLSPEC_DLL void AllocationTracker_SetTargetModuleNamePrefix(_In_ const std::string_view prefixSV);
    ALLOCTRACKER_DECLSPEC_DLL void AllocationTracker_RegisterExternalStackEntryMarker(_In_ const std::string_view markerSV);
    ALLOCTRACKER_DECLSPEC_DLL void AllocationTracker_RegisterExternalStackEntryMarkers(_In_ const std::vector<std::string_view>& markers);

    ALLOCTRACKER_DECLSPEC_DLL void AllocationTracker_SetCollectFullStackTraces(_In_ const bool bCollect);
}
