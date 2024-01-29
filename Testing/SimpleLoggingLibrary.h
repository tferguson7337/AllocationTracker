#ifndef INCL_GUARD_SIMPLE_LOGGING_LIBRARY__
#define INCL_GUARD_SIMPLE_LOGGING_LIBRARY__

#include <sal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <numeric>
#include <ranges>
#include <set>
#include <source_location>
#include <string_view>
#include <thread>
#include <vector>


namespace SLL
{
    using namespace ::std::literals::string_view_literals;

    template <typename StringT>
    concept ValidFormatStringType = std::constructible_from<std::string_view, StringT>;

    // Convenience macro that makes removing consteval qualifier from FormatLocationInfo functions easier, for debugging purposes.
#ifndef SLL_FORMATLOCATIONINFO_NO_CONSTEVAL__
#define SLL_FORMATLOCATIONINFO_CONSTEVAL_DEFINED__
#define SLL_FORMATLOCATIONINFO_CONSTEVAL__ consteval
#else
#define SLL_FORMATLOCATIONINFO_CONSTEVAL_DEFINED__
#define SLL_FORMATLOCATIONINFO_CONSTEVAL__
#endif

    class [[nodiscard]] SimplifiedFunctionName
    {
    private:

        static constexpr std::array s_cOpenPunctChars = {'(', '<', '[', '{'};
        static constexpr std::array s_cClosePunctChars = {')', '>', ']', '}'};

        std::string_view m_FunctionName;

        enum class FindBoundaryType { Begin, End };

        struct [[nodiscard]] CharacterDebt
        {
            std::size_t m_CharacterDebt{0};

            SLL_FORMATLOCATIONINFO_CONSTEVAL__ bool Update(_In_ const char c) noexcept
            {
                if (std::ranges::contains(s_cOpenPunctChars, c))
                {
                    --m_CharacterDebt;
                    return false;
                }
                if (std::ranges::contains(s_cClosePunctChars, c))
                {
                    ++m_CharacterDebt;
                    return false;
                }

                return m_CharacterDebt == 0;
            }
        };

        [[nodiscard]] static SLL_FORMATLOCATIONINFO_CONSTEVAL__ auto GetSimplifiedFunctionNameEndStartingFindItr(
            _In_ const std::string_view funcName)
        {
            const auto pos{funcName.find_last_of(')')};
            return std::next(funcName.crbegin(), ((pos == std::string_view::npos) ? 0 : funcName.size() - pos - 1));
        }

        template <FindBoundaryType FBT>
        [[nodiscard]] static SLL_FORMATLOCATIONINFO_CONSTEVAL__ auto FindSimplifiedFunctionNameBoundary(
            _In_ auto startRItr,
            _In_ const auto rEnd)
        {
            CharacterDebt charDebt;
            while (startRItr != rEnd)
            {
                const char c{*startRItr};
                if (charDebt.Update(c))
                {
                    if constexpr (FBT == FindBoundaryType::Begin)
                    {
                        if (c == ' ')
                        {
                            return startRItr;
                        }
                    }
                    else
                    {
                        static_assert(FBT == FindBoundaryType::End);
                        return startRItr;
                    }
                }

                ++startRItr;
            }

            return startRItr;
        }

        [[nodiscard]] static SLL_FORMATLOCATIONINFO_CONSTEVAL__ std::string_view Simplify(_In_ const std::string_view funcName) noexcept
        {
            const auto rEnd = funcName.crend();
            const auto rSubStrEnd = FindSimplifiedFunctionNameBoundary<FindBoundaryType::End>(GetSimplifiedFunctionNameEndStartingFindItr(funcName), rEnd);
            return {FindSimplifiedFunctionNameBoundary<FindBoundaryType::Begin>(rSubStrEnd, rEnd).base(), rSubStrEnd.base()};
        }

    public:

        SLL_FORMATLOCATIONINFO_CONSTEVAL__ SimplifiedFunctionName(_In_ const std::string_view funcName) :
            m_FunctionName{Simplify(funcName)}
        { }

        [[nodiscard]] constexpr operator std::string_view() const noexcept
        {
            return m_FunctionName;
        }
    };

    class [[nodiscard]] SimplifiedFileName
    {
    private:

        std::string_view m_FileName;

        [[nodiscard]] static SLL_FORMATLOCATIONINFO_CONSTEVAL__ std::string_view Simplify(_In_ const std::string_view path)
        {
            const auto pos = path.find_last_of("/\\"sv);
            const bool bTrim = (pos != std::string_view::npos) && ((pos + 1) < path.length());
            return (bTrim) ? path.substr(pos + 1) : path;
        }

    public:

        SLL_FORMATLOCATIONINFO_CONSTEVAL__ SimplifiedFileName(_In_ const std::string_view path) :
            m_FileName{Simplify(path)}
        { }

        [[nodiscard]] constexpr operator std::string_view() const noexcept
        {
            return m_FileName;
        }
    };

    class [[nodiscard]] FormatLocationInfo
    {
    private:

        std::string_view m_Format;
        SimplifiedFunctionName m_FunctionName;
        SimplifiedFileName m_FileName;
        std::uint32_t m_Line;
        bool m_bNoLogPrefixGeneration;

        static SLL_FORMATLOCATIONINFO_CONSTEVAL__ bool DoesFormatStringWarrantNoPrefixContent(_In_ const std::string_view fmt) noexcept
        {
            auto IsMatchingChar = [](_In_ const char c)
            {
                switch (c)
                {
                case '\n':  [[fallthrough]];
                case '\r':  [[fallthrough]];
                case '-':   [[fallthrough]];
                case '=':   [[fallthrough]];
                case '_':   return true;
                }

                return false;
            };

            if (fmt.empty())
            {
                return true;
            }

            if (std::ranges::all_of(fmt, [firstC = fmt.front()](_In_ const char c) { return c == firstC; }))
            {
                return true;
            }

            if (std::ranges::all_of(fmt, IsMatchingChar))
            {
                return true;
            }

            return false;
        }

    public:

        template <ValidFormatStringType FormatStringT>
        SLL_FORMATLOCATIONINFO_CONSTEVAL__ FormatLocationInfo(
            _In_ const FormatStringT& fmt,
            _In_ const std::source_location srcLoc = std::source_location::current()) noexcept :
            m_Format{fmt},
            m_FunctionName{srcLoc.function_name()},
            m_FileName{srcLoc.file_name()},
            m_Line{srcLoc.line()},
            m_bNoLogPrefixGeneration{DoesFormatStringWarrantNoPrefixContent(fmt)}
        { }

        [[nodiscard]] constexpr std::string_view GetFormat() const noexcept { return m_Format; }
        [[nodiscard]] constexpr std::string_view GetFunctionName() const noexcept { return m_FunctionName; }
        [[nodiscard]] constexpr std::string_view GetFileName() const noexcept { return m_FileName; }
        [[nodiscard]] constexpr std::uint32_t GetLine() const noexcept { return m_Line; }
        [[nodiscard]] constexpr bool NoLogPrefixGeneration() const noexcept { return m_bNoLogPrefixGeneration; }
    };

#ifdef SLL_FORMATLOCATIONINFO_CONSTEVAL_DEFINED__
#undef SLL_FORMATLOCATIONINFO_CONSTEVAL__
#undef SLL_FORMATLOCATIONINFO_CONSTEVAL_DEFINED__
#endif

    inline std::atomic<bool> g_bUseLocalTimeForFormattedLogMsg{true};

    class [[nodiscard]] FormattedLogMsg
    {
    public:

        template <typename... Args>
        constexpr FormattedLogMsg(_In_ const FormatLocationInfo& formatLocInfo, Args&&... args)
        {
            if (!formatLocInfo.NoLogPrefixGeneration())
            {
                static constexpr std::size_t s_cDefaultStringReserveLen{256};
                m_Buffer.reserve(s_cDefaultStringReserveLen);

                UpdateDateTimePrefix();
                m_Buffer += std::format("{}[{}@{}]:  "sv, formatLocInfo.GetFunctionName(), formatLocInfo.GetFileName(), formatLocInfo.GetLine());
                m_Buffer += std::vformat(formatLocInfo.GetFormat(), std::make_format_args(std::forward<Args>(args)...));
            }
            else
            {
                m_Buffer.reserve(formatLocInfo.GetFormat().size() + 1);
                m_Buffer += formatLocInfo.GetFormat();
                m_bUpdatedTimestamp = true; // Ensure any calls to UpdateDateTimePrefix don't add a timestamp prefix.
            }

            m_Buffer.push_back('\n');
        }

        constexpr FormattedLogMsg(_Inout_ const FormattedLogMsg& other) noexcept
        {
            other.TransferTo(*this);
        }

        constexpr FormattedLogMsg(_Inout_ FormattedLogMsg&& other) noexcept
        {
            other.TransferTo(*this);
        }

        constexpr FormattedLogMsg& operator=(_Inout_ const FormattedLogMsg& other) noexcept
        {
            if (this != &other) { other.TransferTo(*this); }
            return *this;
        }

        constexpr FormattedLogMsg& operator=(_Inout_ FormattedLogMsg&& other) noexcept
        {
            if (this != &other) { other.TransferTo(*this); }
            return *this;
        }

        void UpdateDateTimePrefix() const
        {
            if (m_bUpdatedTimestamp)
            {
                return;
            }

            static constexpr auto s_cLogTimestampPrefixFormat{"[{0:%m}/{0:%d}/{0:%Y} {0:%T}] "sv};

            const auto currentTimeMs = []()
            {
                const auto time{std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now())};

                if (g_bUseLocalTimeForFormattedLogMsg)
                {
                    const auto pCurrentZone = std::chrono::current_zone();
                    if (!pCurrentZone) [[unlikely]]
                    {
                        g_bUseLocalTimeForFormattedLogMsg = false;
                    }
                    else
                    {
                        try
                        {
                            return pCurrentZone->to_local(time);
                        }
                        catch (...)
                        {
                            g_bUseLocalTimeForFormattedLogMsg = false;
                        }
                    }
                }

                using LocalTimeMsT = decltype(std::chrono::current_zone()->to_local(time));
                return LocalTimeMsT{time.time_since_epoch()};
            }();

            if (m_Buffer.empty())
            {
                // Generate the prefix.
                m_Buffer = std::vformat(s_cLogTimestampPrefixFormat, std::make_format_args(currentTimeMs));
            }
            else
            {
                // Update the prefix.
                //
                // Note: We only allow one update.  This prevents extra updates in cases
                //       where using nested loggers can result in multiple updates.
                //       E.g.: ThreadedLogger<DispatchLogger> that contains a SyncLogger.
                //             The enqueue to the ThreadedLogger would update the timestamp,
                //             and then the message logged to the SyncLogger would result in a 2nd update.
                //             This would cause timestamp inconsistencies for the DispatchLogger.
                //             W/ only one update allowed, all loggers in the set write the same timestamp.
                std::vformat_to(m_Buffer.begin(), s_cLogTimestampPrefixFormat, std::make_format_args(currentTimeMs));
                m_bUpdatedTimestamp = true;
            }
        }

        [[nodiscard]] constexpr std::string_view ToStringView() const noexcept
        {
            return std::string_view{m_Buffer};
        }

    private:

        // Note: `mutable` so we can do some sneaky stuff on const& instances, such as
        //       updating timestamps in the prefix, or transferring buffer content to internal Logger queues.
        mutable std::string m_Buffer;
        mutable bool m_bUpdatedTimestamp{false};

        constexpr void TransferTo(_Inout_ FormattedLogMsg& other) const noexcept
        {
            other.m_Buffer = std::move(m_Buffer);
            other.m_bUpdatedTimestamp = m_bUpdatedTimestamp;
        }
    };

    template <typename LoggerT, typename ... Args>
    concept ValidLoggerType =
        requires(
            LoggerT logger,
            const FormattedLogMsg& fmtdLogMsg,
            const FormatLocationInfo& fmtLocInfo,
            Args&&... args)
    {
        { logger.Log(fmtdLogMsg) } -> std::same_as<bool>;
        { logger.Log(fmtLocInfo, std::forward<Args>(args)...) } -> std::same_as<bool>;

        { logger(fmtdLogMsg) } -> std::same_as<bool>;
        { logger(fmtLocInfo, std::forward<Args>(args)...) } -> std::same_as<bool>;
    };

    // TODO: If/when modules are a bit better supported, this type should be hidden (no export).
    using RefCountControlBlock = std::atomic<uint32_t>;

    template <ValidLoggerType LoggerT>
    class [[nodiscard]] SharedLoggerPtr
    {
        inline static constexpr bool s_bIsNoThrowDestructible =
            std::is_nothrow_destructible_v<LoggerT> &&
            std::is_nothrow_destructible_v<RefCountControlBlock>;

    private:

        // DispatchLogger needs to do some sneaky stuff while keeping ref-counts valid,
        // so it'll need access to the SharedLoggerPtr's guts to grab the ptr + ctrl block.
        friend class DispatchLogger;

        LoggerT* m_pLogger{nullptr};
        RefCountControlBlock* m_pControlBlock{nullptr};

        constexpr void IncRefCountIfValid() noexcept { if (m_pControlBlock) { m_pControlBlock->operator++(); } }
        constexpr bool DecRefCountIfValid() noexcept { return (m_pControlBlock && (m_pControlBlock->operator--() == 0)); }

    public:

        using value_type = LoggerT;

        constexpr SharedLoggerPtr() noexcept = default;

        constexpr SharedLoggerPtr(_In_ LoggerT* pLogger) :
            m_pLogger{pLogger},
            m_pControlBlock{new RefCountControlBlock{1}}
        { }

        constexpr SharedLoggerPtr(_In_ const SharedLoggerPtr& other) noexcept :
            m_pLogger{other.m_pLogger},
            m_pControlBlock{other.m_pControlBlock}
        {
            IncRefCountIfValid();
        }

        constexpr SharedLoggerPtr(_Inout_ SharedLoggerPtr&& other) noexcept :
            m_pLogger{other.m_pLogger},
            m_pControlBlock{other.m_pControlBlock}
        {
            other.m_pLogger = nullptr;
            other.m_pControlBlock = nullptr;
        }

        constexpr ~SharedLoggerPtr() noexcept(s_bIsNoThrowDestructible)
        {
            Reset();
        }

        constexpr SharedLoggerPtr& operator=(_In_ const SharedLoggerPtr& other) noexcept(s_bIsNoThrowDestructible)
        {
            if (this != &other)
            {
                Reset();

                m_pLogger = other.m_pLogger;
                m_pControlBlock = other.m_pControlBlock;

                IncRefCountIfValid();
            }

            return *this;
        }

        constexpr SharedLoggerPtr& operator=(_Inout_ SharedLoggerPtr&& other) noexcept(s_bIsNoThrowDestructible)
        {
            if (this != &other)
            {
                Reset();

                m_pLogger = other.m_pLogger;
                m_pControlBlock = other.m_pControlBlock;

                other.m_pLogger = nullptr;
                other.m_pControlBlock = nullptr;
            }

            return *this;
        }

        constexpr void Reset() noexcept(s_bIsNoThrowDestructible)
        {
            if (DecRefCountIfValid())
            {
                delete m_pLogger;
                delete m_pControlBlock;
            }

            m_pLogger = nullptr;
            m_pControlBlock = nullptr;
        }

        [[nodiscard]] constexpr LoggerT* operator->() noexcept { return m_pLogger; }
        [[nodiscard]] constexpr const LoggerT* operator->() const noexcept { return m_pLogger; }

        [[nodiscard]] constexpr LoggerT& operator*() noexcept { return *m_pLogger; }
        [[nodiscard]] constexpr const LoggerT& operator*() const noexcept { return *m_pLogger; }

        [[nodiscard]] constexpr LoggerT* Get() noexcept { return m_pLogger; }
        [[nodiscard]] constexpr const LoggerT* Get() const noexcept { return m_pLogger; }
    };

    // Can be inherited from by Loggers to receive special SLL::Factory treatment.
    // If a logger has no state, this will divert the creation of that logger to the
    // more appropriate SLL::UniqueFactory version, so multiple allocations are not required.
    struct StatelessLogger { };

    template <typename LoggerT>
    concept ValidStatelessLoggerType = requires
    {
        requires ValidLoggerType<LoggerT>;
        requires std::is_base_of_v<StatelessLogger, LoggerT>;
    };

    // An empty logger; ignores log message, outputs nothing.
    struct VoidLogger : public StatelessLogger
    {
        template <typename... Args>
        _Success_(return) constexpr bool Log(_In_ const FormatLocationInfo&, Args&&...) const noexcept { return true; }
        _Success_(return) constexpr bool Log(_In_ const FormattedLogMsg&) const noexcept { return true; }

        template <typename... Args>
        _Success_(return) constexpr bool operator()(_In_ const FormatLocationInfo&, Args&&...) const noexcept { return true; }
        _Success_(return) constexpr bool operator()(_In_ const FormattedLogMsg&) const noexcept { return true; }
    };
    static_assert(ValidStatelessLoggerType<VoidLogger>, "VoidLogger does not satisfy the ValidStatelessLoggerType concept.");
}


// Helper function for stream-insertion operator support.
template <typename StreamT>
constexpr StreamT& operator<<(_Inout_ StreamT& stream, _In_ const SLL::FormattedLogMsg& msg)
{
    stream << msg.ToStringView();
    return stream;
}


namespace SLL
{
    struct [[nodiscard]] StdOutLogger : public StatelessLogger
    {
        _Success_(return) bool Log(_In_ const FormattedLogMsg& msg) const
        {
            return (std::cout << msg).flush().good();
        }

        template <typename... Args>
        _Success_(return) bool Log(_In_ const FormatLocationInfo& fmtLocInfo, Args&&... args) const
        {
            return Log(FormattedLogMsg{fmtLocInfo, std::forward<Args>(args)...});
        }

        template <typename... Args>
        _Success_(return) bool operator()(_In_ const FormatLocationInfo& fmtLocInfo, Args&&... args) const
        {
            return Log(FormattedLogMsg{fmtLocInfo, std::forward<Args>(args)...});
        }

        _Success_(return) bool operator()(_In_ const FormattedLogMsg& msg) const
        {
            return Log(msg);
        }
    };
    static_assert(ValidStatelessLoggerType<StdOutLogger>, "StdOutLogger does not satisfy the ValidStatelessLoggerType concept.");

    struct [[nodiscard]] StdErrLogger : public StatelessLogger
    {
        _Success_(return) bool Log(_In_ const FormattedLogMsg& msg) const
        {
            return (std::cerr << msg).good();
        }

        template <typename... Args>
        _Success_(return) bool Log(_In_ const FormatLocationInfo& fmtLocInfo, Args&&... args) const
        {
            return Log(FormattedLogMsg{fmtLocInfo, std::forward<Args>(args)...});
        }

        template <typename... Args>
        _Success_(return) bool operator()(_In_ const FormatLocationInfo& fmtLocInfo, Args&&... args) const
        {
            return Log(FormattedLogMsg{fmtLocInfo, std::forward<Args>(args)...});
        }

        _Success_(return) bool operator()(_In_ const FormattedLogMsg & msg) const
        {
            return Log(msg);
        }
    };
    static_assert(ValidStatelessLoggerType<StdErrLogger>, "StdErrLogger does not satisfy the ValidStatelessLoggerType concept.");
}


namespace SLL
{
    template <typename StringT>
    concept ValidFileLoggerPathStringType = std::constructible_from<std::filesystem::path, StringT>;

    class [[nodiscard]] FileLogger
    {
    protected:

        std::filesystem::path m_LogPath;
        std::ofstream m_FileStream;

    public:

        template <ValidFileLoggerPathStringType StringT>
        FileLogger(
            StringT&& path,
            _In_ const std::ios_base::openmode openmode = std::ios_base::binary | std::ios_base::app) :
            m_LogPath{std::forward<StringT>(path)},
            m_FileStream{m_LogPath, openmode}
        { }

        FileLogger(const FileLogger&) = delete;
        FileLogger& operator=(const FileLogger&) = delete;

        FileLogger(FileLogger&&) noexcept = default;
        FileLogger& operator=(FileLogger&&) noexcept = default;

        ~FileLogger() noexcept = default;

        auto GetPathView() const noexcept
        {
            return std::basic_string_view<std::filesystem::path::value_type>(m_LogPath.native());
        }

        auto GetPath() const noexcept { return m_LogPath; }

        bool Valid() const noexcept { return m_FileStream.good(); }

        _Success_(return) bool Log(_In_ const FormattedLogMsg& msg)
        {
            return (m_FileStream << msg).good();
        }

        template <typename... Args>
        _Success_(return) bool Log(_In_ const FormatLocationInfo& fmtLocInfo, Args&&... args)
        {
            return Log(FormattedLogMsg{fmtLocInfo, std::forward<Args>(args)...});
        }

        template <typename... Args>
        _Success_(return) bool operator()(_In_ const FormatLocationInfo& fmtLocInfo, Args&&... args)
        {
            return Log(FormattedLogMsg{fmtLocInfo, std::forward<Args>(args)...});
        }

        _Success_(return) bool operator()(_In_ const FormattedLogMsg& msg)
        {
            return Log(msg);
        }

        auto operator<=>(_In_ const FileLogger& other) const noexcept { return m_LogPath <=> other.m_LogPath; }
    };
    static_assert(ValidLoggerType<FileLogger>, "FileLogger does not satisfy the ValidLoggerType concept.");
    static_assert(!ValidStatelessLoggerType<FileLogger>, "FileLogger satisfies the ValidStatelessLoggerType concept.");
}


namespace SLL
{
    template <ValidLoggerType LoggerT>
    class [[nodiscard]] SyncLogger
    {
    protected:

        LoggerT m_Logger;
        std::mutex m_LoggerMutex;

    public:

        template <typename... Args>
        SyncLogger(Args&&... loggerArgs) noexcept(std::is_nothrow_constructible_v<LoggerT, Args...>) :
            m_Logger{std::forward<Args>(loggerArgs)...}
        { }

        ~SyncLogger() noexcept = default;

        _Success_(return) bool Log(_In_ const FormattedLogMsg& msg)
        {
            std::lock_guard<std::mutex> lg(m_LoggerMutex);
            msg.UpdateDateTimePrefix();
            return m_Logger.Log(msg);
        }

        template <typename... Args>
        _Success_(return) bool Log(_In_ const FormatLocationInfo& fmtLocInfo, Args&&... args)
        {
            return Log(FormattedLogMsg{fmtLocInfo, std::forward<Args>(args)...});
        }

        template <typename... Args>
        _Success_(return) bool operator()(_In_ const FormatLocationInfo& fmtLocInfo, Args&&... args)
        {
            return Log(FormattedLogMsg{fmtLocInfo, std::forward<Args>(args)...});
        }

        _Success_(return) bool operator()(_In_ const FormattedLogMsg& msg)
        {
            return Log(msg);
        }
    };
    static_assert(ValidLoggerType<SyncLogger<VoidLogger>>, "SyncLogger does not satisfy the ValidLoggerType concept.");
    static_assert(!ValidStatelessLoggerType<SyncLogger<VoidLogger>>, "SyncLogger satisfies the ValidStatelessLoggerType concept.");


    template <ValidLoggerType LoggerT>
    class [[nodiscard]] ThreadedLogger
    {
    protected:

        class Worker
        {
        private:

            LoggerT m_Logger;
            std::vector<FormattedLogMsg> m_Queue;
            std::atomic<size_t> m_QueueSize;
            std::atomic<size_t> m_QueueSizeWakeupThreshold;

            std::atomic<bool> m_bExit;
            std::mutex m_Mutex;
            std::condition_variable m_CV;
            std::jthread m_Thread;

            [[nodiscard]] std::vector<FormattedLogMsg> WaitForAndGetLogMessages()
            {
                std::vector<FormattedLogMsg> queue;
                std::unique_lock<std::mutex> ul(m_Mutex);
                m_CV.wait(ul, [this]() { return (m_Queue.size() >= m_QueueSizeWakeupThreshold) || m_bExit; });
                std::swap(m_Queue, queue);
                m_QueueSize = 0;
                return queue;
            }

            void WorkerLoop()
            {
                while ((m_QueueSize != 0) || !m_bExit)
                {
                    std::ranges::for_each(WaitForAndGetLogMessages(),
                        [this](const auto& msg)
                        {
                            m_Logger.Log(msg);
                        });
                }
            }

        public:

            template <typename... Args>
            Worker(Args&&... loggerArgs) :
                m_Logger{std::forward<Args>(loggerArgs)...},
                m_QueueSize{0},
                m_QueueSizeWakeupThreshold{8},
                m_bExit{false},
                m_Thread{&Worker::WorkerLoop, this}
            { }

            ~Worker()
            {
                m_bExit = true;
                m_CV.notify_one();
            }

            void EnqueueMsg(_In_ const FormattedLogMsg& msg)
            {
                auto AcquireLockAndPushBack = [this, &msg]() -> size_t
                {
                    std::lock_guard<std::mutex> lg(m_Mutex);
                    msg.UpdateDateTimePrefix(); // Update msg timestamp now that we have the lock.
                    m_Queue.push_back(msg);
                    return ++m_QueueSize;
                };

                if (AcquireLockAndPushBack() >= m_QueueSizeWakeupThreshold)
                {
                    m_CV.notify_one();
                }
            }

            void SetQueueSizeWakeupThreshold(_In_ const size_t threshold)
            {
                m_QueueSizeWakeupThreshold = threshold;
                if (m_QueueSize >= threshold)
                {
                    m_CV.notify_one();
                }
            }
        };

        Worker m_Worker;

    public:

        template <typename... Args>
        ThreadedLogger(Args&&... loggerArgs) :
            m_Worker{std::forward<Args>(loggerArgs)...}
        { }

        ~ThreadedLogger() noexcept = default;

        _Success_(return) bool Log(_In_ const FormattedLogMsg& msg)
        {
            // Note: we don't actually log - just pass the msg on to the worker's queue.
            m_Worker.EnqueueMsg(msg);
            return true;
        }

        template <typename... Args>
        _Success_(return) bool Log(_In_ const FormatLocationInfo& fmtLocInfo, Args&&... args)
        {
            return Log(FormattedLogMsg{fmtLocInfo, std::forward<Args>(args)...});
        }

        template <typename... Args>
        _Success_(return) bool operator()(_In_ const FormatLocationInfo& fmtLocInfo, Args&&... args)
        {
            return Log(FormattedLogMsg{fmtLocInfo, std::forward<Args>(args)...});
        }

        _Success_(return) bool operator()(_In_ const FormattedLogMsg& msg)
        {
            return Log(msg);
        }

        void SetQueueSizeWakeupThreshold(_In_ const size_t threshold)
        {
            m_Worker.SetQueueSizeWakeupThreshold(threshold);
        }
    };
    static_assert(ValidLoggerType<ThreadedLogger<VoidLogger>>, "ThreadedLogger does not satisfy the ValidLoggerType concept.");
    static_assert(!ValidStatelessLoggerType<ThreadedLogger<VoidLogger>>, "ThreadedLogger satisfies the ValidStatelessLoggerType concept.");
}


namespace SLL
{
    class [[nodiscard]] DispatchLogger
    {
    public:

        template <typename... LoggerTs>
        DispatchLogger(LoggerTs&&... loggers) noexcept(sizeof...(loggers) == 0)
        {
            RegisterLoggers(std::forward<LoggerTs>(loggers)...);
        }

        template <ValidLoggerType LoggerT, typename... OtherLoggerTs>
        _Success_(return) bool RegisterLoggers(_In_ SharedLoggerPtr<LoggerT> pLogger, OtherLoggerTs&&... otherLoggers)
        {
            return m_LoggerSet.emplace(pLogger.m_pLogger, pLogger.m_pControlBlock, ReleaseOwnershipHelper<LoggerT>, LogHelper<LoggerT>).second
                && RegisterLoggers(std::forward<OtherLoggerTs>(otherLoggers)...);
        }

        _Success_(return) bool Log(_In_ const FormattedLogMsg& msg)
        {
            bool bSuccess{true};
            for (auto& pLogger : m_LoggerSet) { bSuccess &= pLogger.Log(msg); }
            return bSuccess;
        }

        template <typename... Args>
        _Success_(return) bool Log(_In_ const FormatLocationInfo& fmtLocInfo, Args&&... args)
        {
            return Log(FormattedLogMsg{fmtLocInfo, std::forward<Args>(args)...});
        }

        template <typename... Args>
        _Success_(return) bool operator()(_In_ const FormatLocationInfo& fmtLocInfo, Args&&... args)
        {
            return Log(FormattedLogMsg{fmtLocInfo, std::forward<Args>(args)...});
        }

        _Success_(return) bool operator()(_In_ const FormattedLogMsg & msg)
        {
            return Log(msg);
        }

    private:

        // Base-case for recursive RegisterLoggers call.
        _Success_(return) constexpr bool RegisterLoggers() const noexcept { return true; }

        template <ValidLoggerType LoggerT>
        static void ReleaseOwnershipHelper(_In_ void* pLogger, _Inout_ RefCountControlBlock* pRefCount)
        {
            if (--(*pRefCount) == 0)
            {
                delete static_cast<LoggerT*>(pLogger);
            }
        }

        template <ValidLoggerType LoggerT>
        static bool LogHelper(_In_ void* pLogger, _In_ const FormattedLogMsg& msg)
        {
            return static_cast<LoggerT*>(pLogger)->Log(msg);
        }

        struct [[nodiscard]] AmbiguousSharedLoggerWrapper
        {
            using ReleaseOwnershipCallback = void(*)(_In_ void*, _Inout_ RefCountControlBlock*);
            using LogCallback = bool(*)(_In_ void*, _In_ const FormattedLogMsg&);

            void* m_pLoggerObj;
            RefCountControlBlock* m_pLoggerObjRefCount;
            ReleaseOwnershipCallback m_fpReleaseOwnership;
            LogCallback m_fpLog;

            AmbiguousSharedLoggerWrapper(
                _In_ void* pLogger,
                _In_ RefCountControlBlock* pLoggerObjRefCount,
                _In_ ReleaseOwnershipCallback fpReleaseOwnership,
                _In_ LogCallback fpLog) :
                m_pLoggerObj{pLogger},
                m_pLoggerObjRefCount{pLoggerObjRefCount},
                m_fpReleaseOwnership{fpReleaseOwnership},
                m_fpLog{fpLog}
            {
                m_pLoggerObjRefCount->operator++();
            }

            // No copy/move expected during lifetime //
            AmbiguousSharedLoggerWrapper(const AmbiguousSharedLoggerWrapper&) = delete;
            AmbiguousSharedLoggerWrapper(AmbiguousSharedLoggerWrapper&&) = delete;
            AmbiguousSharedLoggerWrapper& operator=(const AmbiguousSharedLoggerWrapper&) = delete;
            AmbiguousSharedLoggerWrapper& operator=(AmbiguousSharedLoggerWrapper&&) = delete;

            ~AmbiguousSharedLoggerWrapper() 
            {
                m_fpReleaseOwnership(m_pLoggerObj, m_pLoggerObjRefCount);
            }

            _Success_(return) bool Log(_In_ const FormattedLogMsg& msg) const { return m_fpLog(m_pLoggerObj, msg); }

            [[nodiscard]] constexpr bool operator<(_In_ const AmbiguousSharedLoggerWrapper& rhs) const noexcept
            {
                return m_pLoggerObj < rhs.m_pLoggerObj;
            }
        };

        // TODO: Consider adding compile-time buffer size template parameter, then leverage PMR allocator to flatten the std::set.
        std::set<AmbiguousSharedLoggerWrapper> m_LoggerSet;
    };
    static_assert(ValidLoggerType<DispatchLogger>, "DispatchLogger does not satisfy the ValidLoggerType concept.");
    static_assert(!ValidStatelessLoggerType<DispatchLogger>, "DispatchLogger satisfies the ValidStatelessLoggerType concept.");
}


// Logger Factories
namespace SLL
{
    template <ValidLoggerType LoggerT>
    struct [[nodiscard]] Factory
    {
        consteval Factory() noexcept = default;
        constexpr ~Factory() noexcept = default;

        template <typename... Args>
        [[nodiscard]] constexpr SharedLoggerPtr<LoggerT> operator()(Args&&... loggerArgs)
        {
            return SharedLoggerPtr<LoggerT>{new LoggerT(std::forward<Args>(loggerArgs)...)};
        }
    };

    template <ValidLoggerType LoggerT>
    struct [[nodiscard]] UniqueFactory
    {
        consteval UniqueFactory() noexcept = default;
        constexpr ~UniqueFactory() noexcept = default;

        template <typename... Args>
        [[nodiscard]] constexpr SharedLoggerPtr<LoggerT> operator()(Args&&... loggerArgs)
        {
            static SharedLoggerPtr<LoggerT> s_pLogger{new LoggerT(std::forward<Args>(loggerArgs)...)};
            return s_pLogger;
        }
    };

}


#endif // #ifndef INCL_GUARD_SIMPLE_LOGGING_LIBRARY__