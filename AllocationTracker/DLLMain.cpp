#include <Windows.h>

#include <detours.h>

#include "AllocationTrackerInternal.h"

#include "..\Testing\SimpleLoggingLibrary.h"
#include "StringUtils.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <print>
#include <ranges>
#include <shared_mutex>
#include <stacktrace>


namespace AllocationTracking
{
    using HeapAllocFn = LPVOID(WINAPI*)(HANDLE, DWORD, SIZE_T);
    using HeapReAllocFn = LPVOID(WINAPI*)(HANDLE, DWORD, LPVOID, SIZE_T);
    using HeapFreeFn = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID);

    HeapAllocFn s_RealHeapAlloc{nullptr};
    HeapReAllocFn s_RealHeapReAlloc{nullptr};
    HeapFreeFn s_RealHeapFree{nullptr};

    using ExitUserThreadFn = void(WINAPI*)(DWORD);
    ExitUserThreadFn s_RealExitUserThread{nullptr};

    // Internal function, guessing the signature here based on disassembly.
    using BaseThreadInitThunkFn = void(WINAPI*)(DWORD, LPTHREAD_START_ROUTINE, LPVOID);
    BaseThreadInitThunkFn s_RealBaseThreadInitThunk{nullptr};

    thread_local std::uint32_t gtl_ReentryCount{0};
    struct ScopedReentryCountIncrementer
    {
        ScopedReentryCountIncrementer() noexcept { ++gtl_ReentryCount; }
        ~ScopedReentryCountIncrementer() noexcept { --gtl_ReentryCount; }
    };

    static DWORD s_dwTlsIndex{TLS_OUT_OF_INDEXES};

    static bool IsThreadLocalStorageReady()
    {
        if (s_dwTlsIndex == TLS_OUT_OF_INDEXES)
        {
            return false;
        }

        return !!TlsGetValue(s_dwTlsIndex);
    }

    template <OpFlag MemoryOp>
    static bool ShouldAddToBacklog()
    {
        if constexpr (!(MemoryOp & OpFlag::Free))
        {
            // Only frees are expected atm.
            return false;
        }

        if (!IsThreadLocalStorageReady())
        {
            // We need to account for late cleanup steps
            // that can occur after TLS/FLS is cleaned up.
            return true;
        }

        if (!gtl_ThreadTracker.m_bRegistered)
        {
            return true;
        }

        if (gtl_IsNoTrackAllocationOrFree > 0)
        {
            return false;
        }

        if (gtl_IsInternalAllocationOrFree > 0)
        {
            return false;
        }

        if (!g_bTrackingEnabled)
        {
            return false;
        }

        return true;
    }

    template <OpFlag MemoryOp>
    static bool ShouldSkipTracking()
    {
        if (!IsThreadLocalStorageReady())
        {
            // Thread hasn't attached yet, or has detached - no go.
            return true;
        }

        if ((gtl_IsNoTrackAllocationOrFree > 0)
            || (gtl_IsInternalAllocationOrFree > 0)
            || (gtl_ReentryCount > 0))
        {
            // This is a no-track scenario, an internal tracker allocation,
            // or HeapAlloc has called back into HeapAlloc - no go.
            return true;
        }

        // For all other cases, only track if global tracking is enabled.
        return !g_bTrackingEnabled;
    }

    static void* HandleCommonAllocTracking(_Inout_ MemoryInfo&& info)
    {
        if (!info.m_pMem)
        {
            return info.m_pMem;
        }

        auto pTracker{GlobalTracker::InstanceIfTrackingEnabled()};
        if (!pTracker)
        {
            return info.m_pMem;
        }

        pTracker->TrackAllocation(std::move(info));
        return info.m_pMem;
    }

    static void* HandleCommonAllocTracking(_Inout_ ReallocMemoryInfo&& info)
    {
        auto pTracker{GlobalTracker::InstanceIfTrackingEnabled()};
        if (!pTracker)
        {
            return info.m_pMem;
        }

        auto [syntheticFree, syntheticAlloc] = info.Synthesize();
        if (!!syntheticFree.m_pMem)
        {
            pTracker->TrackDeallocation(std::move(syntheticFree));
        }
        if (!!syntheticAlloc.m_pMem)
        {
            pTracker->TrackAllocation(std::move(syntheticAlloc));
        }

        return info.m_pMem;
    }


    LPVOID WINAPI HeapAllocDetour(HANDLE hHeap, DWORD dwFlags, SIZE_T bytes)
    {
        //
        // Note:
        //  TIL that HeapAlloc can be naturally re-entrant.
        //  There are some internal flags that get used, namely 0x0080'0000,
        //  combined with HEAP_NO_SERIALIZE (0x1).
        //
        //  For example:
        //  Re-entrant call, processed first:
        //      Heap:   0x000001e9bd440000
        //      Flags:  8388609 ((<UNKNOWN_HEAP_FLAG>)0x0080'0000 | HEAP_NO_SERIALIZE(0x1))
        //      Bytes:  512
        //      Ptr:    0x000001e9bd457ae0
        //
        //  Origin call, receives address in range of re-entrant HeapAlloc call:
        //      Heap:   0x000001e9bd440000
        //      Flags:  0
        //      Bytes:  6
        //      Ptr:    0x000001e9bd457b80 [0x000001e9bd457ae0, 0x000001e9bd457ce0]
        //
        //  The re-entrant call allocates a pointer with size that is in range of the address
        //  the origin call gets.  No idea what this is doing under the hood, but we'll treat
        //  it as internal heap management and ignore these re-entrant calls.
        //

        static constexpr auto s_OpFlag{OpFlag::Alloc};

        if (ShouldSkipTracking<s_OpFlag>())
        {
            return s_RealHeapAlloc(hHeap, dwFlags, bytes);
        }

        auto AllocTS = [=]()
        {
            ScopedReentryCountIncrementer scopedReentryCountIncrementer;
            MemoryInfo info{
                .m_pMem = s_RealHeapAlloc(hHeap, dwFlags, bytes),
                .m_Bytes = bytes,
                .m_OpFlagMask = s_OpFlag};
            return info;
        };
        return HandleCommonAllocTracking(AllocTS());
    }

    LPVOID WINAPI HeapReAllocDetour(HANDLE hHeap, DWORD dwFlags, LPVOID ptr, SIZE_T bytes)
    {
        static constexpr auto s_OpFlag{OpFlag::Realloc};

        if (ShouldSkipTracking<s_OpFlag>())
        {
            return s_RealHeapReAlloc(hHeap, dwFlags, ptr, bytes);
        }

        auto ReAllocTs = [=]()
        {
            // Note: grab original size of allocation before we realloc.
            const SIZE_T originalBytes{HeapSize(hHeap, dwFlags, ptr)};

            ScopedReentryCountIncrementer scopedReentryCountIncrementer;
            ReallocMemoryInfo info{
                .m_pOriginalMem = ptr,
                .m_OriginalBytes = originalBytes,
                .m_pMem = s_RealHeapReAlloc(hHeap, dwFlags, ptr, bytes),
                .m_Bytes = bytes};
            return info;
        };
        return HandleCommonAllocTracking(ReAllocTs());
    }

    BOOL WINAPI HeapFreeDetour(HANDLE hHeap, DWORD dwFlags, LPVOID ptr)
    {
        static constexpr auto s_OpFlag{OpFlag::Free};

        if (!ptr)
        {
            return TRUE;
        }

        if (ShouldSkipTracking<s_OpFlag>())
        {
            // We have some extra skip-tracking logic for free scenarios, to try
            // and catch free's that happen around the time thread is exiting after
            // deregistration or TLS-free has occurred.
            if (ShouldAddToBacklog<s_OpFlag>())
            {
                // TLS is gone and/or this thread has deregistered from GlobalTracker.
                // Add this free to the no-TLS-safe queue so we track these frees correctly.
                auto pTracker{GlobalTracker::Instance()};
                if (!!pTracker)
                {
                    pTracker->AddToDeregisteredFreeQueue(
                        DeregisteredMemoryFreeInfo{
                            .m_pMem = ptr,
                            .m_Bytes = HeapSize(hHeap, dwFlags, ptr)});
                }
            }

            return s_RealHeapFree(hHeap, dwFlags, ptr);
        }

        BOOL bSuccess{FALSE};
        auto FreeTs = [hHeap, dwFlags, ptr, &bSuccess]()
        {
            const SIZE_T bytes{HeapSize(hHeap, dwFlags, ptr)};

            ScopedReentryCountIncrementer scopedReentryCountIncrementer;
            bSuccess = s_RealHeapFree(hHeap, dwFlags, ptr);

            MemoryInfo info{
                .m_pMem = ptr,
                .m_Bytes = bytes,
                .m_OpFlagMask = s_OpFlag};
            return info;
        };
        auto info{FreeTs()};
        if (!ptr || !bSuccess)
        {
            return bSuccess;
        }

        auto pTracker{AllocationTracking::GlobalTracker::InstanceIfTrackingEnabled()};
        if (!pTracker)
        {
            return bSuccess;
        }

        pTracker->TrackDeallocation(std::move(info));
        return bSuccess;
    }

    void WINAPI RtlExitUserThreadDetour(DWORD dwReturnCode)
    {
        if (IsThreadLocalStorageReady())
        {
            gtl_ThreadTracker.DeregisterWithGlobalTracker();
            LPVOID pTlsVal{TlsGetValue(s_dwTlsIndex)};
            if (!!pTlsVal)
            {
                TlsSetValue(s_dwTlsIndex, nullptr);
                LocalFree((HLOCAL)pTlsVal);
            }
        }

        s_RealExitUserThread(dwReturnCode);
    }

    void WINAPI BaseThreadInitThunkDetour(
        DWORD dwUnknown,
        LPTHREAD_START_ROUTINE lpThreadStartAddr,
        LPVOID lpUnknownOrThreadParam)
    {
        if (IsThreadLocalStorageReady())
        {
            gtl_ThreadTracker.EnableTracking(true);
        }

        s_RealBaseThreadInitThunk(dwUnknown, lpThreadStartAddr, lpUnknownOrThreadParam);
    }
}


class [[nodiscard]] ScopedDetourInit
{
private:
    bool m_bDetourSuccess{false};

    bool PopulateRealFunctionPointers()
    {
        using namespace AllocationTracking;
        {
            const HMODULE hNtdllModule{GetModuleHandleA("ntdll.dll")};
            if (!hNtdllModule)
            {
                return false;
            }

            s_RealHeapAlloc = reinterpret_cast<HeapAllocFn>(
                GetProcAddress(hNtdllModule, "RtlAllocateHeap"));
            if (!s_RealHeapAlloc)
            {
                return false;
            }

            s_RealHeapReAlloc = reinterpret_cast<HeapReAllocFn>(
                GetProcAddress(hNtdllModule, "RtlReAllocateHeap"));
            if (!s_RealHeapReAlloc)
            {
                return false;
            }

            s_RealHeapFree = reinterpret_cast<HeapFreeFn>(
                GetProcAddress(hNtdllModule, "RtlFreeHeap"));
            if (!s_RealHeapFree)
            {
                return false;
            }

            s_RealExitUserThread = reinterpret_cast<ExitUserThreadFn>(
                GetProcAddress(hNtdllModule, "RtlExitUserThread"));
            if (!s_RealExitUserThread)
            {
                return false;
            }
        }

        {
            const HMODULE hKernel32Module{GetModuleHandleA("kernel32.dll")};
            if (!hKernel32Module)
            {
                return false;
            }

            s_RealBaseThreadInitThunk = reinterpret_cast<BaseThreadInitThunkFn>(
                GetProcAddress(hKernel32Module, "BaseThreadInitThunk"));
            if (!s_RealBaseThreadInitThunk)
            {
                return false;
            }
        }

        return true;
    }

    template <bool bInit>
    bool HandleDetourWork()
    {
        using namespace AllocationTracking;

        if (m_bDetourSuccess == bInit)
        {
            // If we're initializing, but we've already detoured, return true.
            // If we're uninitializing, but we haven't detoured, return true.
            return true;
        }

        if constexpr (bInit)
        {
            if (!PopulateRealFunctionPointers())
            {
                return false;
            }
        }

        if constexpr (bInit)
        {
            if (DetourRestoreAfterWith() != NO_ERROR)
            {
                return false;
            }
        }
        if (DetourTransactionBegin() != NO_ERROR)
        {
            return false;
        }

        //
        // TODO:
        //  DetourUpdateThread on the current thread is a no-op.
        //  This function is intended to suspend the specified thread until
        //  the detour-transaction is committed, at which point the instructions are flushed.
        //
        if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR)
        {
            DetourTransactionAbort();
            return false;
        }

        if constexpr (bInit)
        {
            if ((DetourAttach(&(LPVOID&)(s_RealHeapAlloc), HeapAllocDetour) != NO_ERROR) ||
                (DetourAttach(&(LPVOID&)(s_RealHeapReAlloc), HeapReAllocDetour) != NO_ERROR) ||
                (DetourAttach(&(LPVOID&)(s_RealHeapFree), HeapFreeDetour) != NO_ERROR) ||
                (DetourAttach(&(LPVOID&)(s_RealExitUserThread), RtlExitUserThreadDetour) != NO_ERROR) ||
                (DetourAttach(&(LPVOID&)(s_RealBaseThreadInitThunk), BaseThreadInitThunkDetour) != NO_ERROR))

            {
                DetourTransactionAbort();
                return false;
            }
        }
        else
        {
            if ((DetourDetach(&(LPVOID&)(s_RealBaseThreadInitThunk), BaseThreadInitThunkDetour) != NO_ERROR) ||
                (DetourDetach(&(LPVOID&)(s_RealExitUserThread), RtlExitUserThreadDetour) != NO_ERROR) ||
                (DetourDetach(&(LPVOID&)(s_RealHeapFree), HeapFreeDetour) != NO_ERROR) ||
                (DetourDetach(&(LPVOID&)(s_RealHeapReAlloc), HeapReAllocDetour) != NO_ERROR) ||
                (DetourDetach(&(LPVOID&)(s_RealHeapAlloc), HeapAllocDetour) != NO_ERROR))
            {
                DetourTransactionAbort();
                return false;
            }
        }

        if (DetourTransactionCommit() != NO_ERROR)
        {
            DetourTransactionAbort();
            return false;
        }

        return true;
    }

public:

    ScopedDetourInit() :
        m_bDetourSuccess{HandleDetourWork<true>()}
    {
        if (m_bDetourSuccess)
        {
            AllocationTracking::GlobalTracker::Init();
        }
        else
        {
            __debugbreak();
        }
    }

    ~ScopedDetourInit()
    {
        if (m_bDetourSuccess)
        {
            HandleDetourWork<false>();
            AllocationTracking::GlobalTracker::DeInit();
        }
    }

    [[nodiscard]] constexpr bool WasDetourSuccessful() const noexcept
    {
        return m_bDetourSuccess;
    }
};


static std::unique_ptr<ScopedDetourInit> s_pScopedDetour;

static constexpr bool g_bEnableDetoursAndTracking{true};

BOOL WINAPI DllMain(
    [[maybe_unused]] HINSTANCE hinstDLL,  // handle to DLL module
    [[maybe_unused]] DWORD fdwReason,     // reason for calling function
    [[maybe_unused]] LPVOID lpvReserved)  // reserved
{
    if constexpr (g_bEnableDetoursAndTracking)
    {
        // Perform actions based on the reason for calling.
        switch (fdwReason)
        {
        case DLL_PROCESS_ATTACH:
        {
            __debugbreak();
            AllocationTracking::s_dwTlsIndex = TlsAlloc();
            if (AllocationTracking::s_dwTlsIndex == TLS_OUT_OF_INDEXES)
            {
                return FALSE;
            }

            HLOCAL hLoc{LocalAlloc(LMEM_FIXED, 1)};
            if (hLoc != nullptr)
            {
                TlsSetValue(AllocationTracking::s_dwTlsIndex, (LPVOID)hLoc);
            }

            s_pScopedDetour = std::make_unique<ScopedDetourInit>();
            if (s_pScopedDetour->WasDetourSuccessful())
            {
                AllocationTracking::gtl_ThreadTracker.RegisterWithGlobalTracker();
            }

            break;
        }

        case DLL_THREAD_ATTACH:
        {
            if (s_pScopedDetour->WasDetourSuccessful())
            {
                HLOCAL hLoc{LocalAlloc(LMEM_FIXED, 1)};
                if (hLoc != nullptr && !!TlsSetValue(AllocationTracking::s_dwTlsIndex, (LPVOID)hLoc))
                {
                    AllocationTracking::gtl_ThreadTracker.RegisterWithGlobalTracker();
                }
            }

            break;
        }

        case DLL_THREAD_DETACH:
        {
            /* We handle TLS cleanup in RtlExitUserThread detour */
            break;
        }

        case DLL_PROCESS_DETACH:
        {
            if (AllocationTracking::s_dwTlsIndex != TLS_OUT_OF_INDEXES)
            {
                LPVOID pTlsVal{TlsGetValue(AllocationTracking::s_dwTlsIndex)};
                if (!!pTlsVal)
                {
                    AllocationTracking::gtl_ThreadTracker.DeregisterWithGlobalTracker();
                    LocalFree((HLOCAL)pTlsVal);
                }

                TlsFree(AllocationTracking::s_dwTlsIndex);
                AllocationTracking::s_dwTlsIndex = TLS_OUT_OF_INDEXES;
            }

            s_pScopedDetour.reset();
            break;
        }

        default:
            // Nothing to do.
            break;
        }
    }

    return TRUE;  // Successful DLL_PROCESS_ATTACH.
}
