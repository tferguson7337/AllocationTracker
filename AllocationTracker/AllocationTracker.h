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

    ALLOCTRACKER_DECLSPEC_DLL void LogAllocations(
        _In_ const LogCallback& logFn,
        _In_ const LogSummaryType type,
        _In_ const bool waitForWorkerThreadLull = false);
}


namespace AllocationTracking
{
    ALLOCTRACKER_DECLSPEC_DLL void EnableTracking(_In_ const bool bEnabled);

    // E.g., "MyExecutable!"
    ALLOCTRACKER_DECLSPEC_DLL void SetTargetModuleNamePrefix(_In_ const std::string_view prefixSV);
    ALLOCTRACKER_DECLSPEC_DLL void RegisterExternalStackEntryMarker(_In_ const std::string_view markerSV);
    ALLOCTRACKER_DECLSPEC_DLL void RegisterExternalStackEntryMarkers(_In_ const std::vector<std::string_view>& markers);

    ALLOCTRACKER_DECLSPEC_DLL void SetCollectFullStackTraces(_In_ const bool bCollect);
}
