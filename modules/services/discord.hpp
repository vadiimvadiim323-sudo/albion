// modules/services/discord.hpp
#pragma once
#include "modules/core/utilities.hpp"
class DiscordPresence
{
public:
    ~DiscordPresence() { Stop(); }

    void Start()
    {
        bool expected = false;
        if (!mRunning.compare_exchange_strong(expected, true)) return;
        mStartTime = static_cast<long long>(std::time(nullptr));
        // FIX-V11.1-DISCORD: a worker that exited by itself (guarded exception)
        // leaves a joinable std::thread; assigning over it calls std::terminate.
        if (mThread.joinable()) mThread.join();
        // FIX-V11-03/10: No uncaught worker or thread-construction exception.
        mCancelled.store(false);
        try { mThread = std::thread([this] {
            albion::GuardWorker([this] { Worker(); }, [](const char* e) { DevLog("DISCORD", e); },
                [this] { ClosePipe(); mRunning = false; });
        }); } catch (...) { mRunning = false; mCancelled = true; DevLog("DISCORD", "Worker could not start"); }
    }

    void Stop()
    {
        mCancelled.store(true, std::memory_order_release);
        mRunning.store(false);
        if (mThread.joinable()) mThread.join();
    }

private:
    static constexpr const char* kClientId = "1346937672492515451";
    static constexpr std::uint32_t kHandshake = 0;
    static constexpr std::uint32_t kFrame = 1;
    static constexpr std::uint32_t kClose = 2;
    static constexpr std::uint32_t kPing = 3;
    static constexpr std::uint32_t kPong = 4;
    static constexpr std::uint32_t kMaxPayload = 1024u * 1024u;

    struct Header
    {
        std::uint32_t opcode;
        std::uint32_t length;
    };
    static_assert(sizeof(Header) == 8, "Discord IPC header must be 8 bytes");

    std::atomic<bool> mRunning{ false }, mCancelled{ true };
    std::thread mThread;
    HANDLE mPipe = INVALID_HANDLE_VALUE;
    long long mStartTime = 0;
    unsigned long long mNonce = 0;

    void ClosePipe()
    {
        if (mPipe != INVALID_HANDLE_VALUE)
        {
            CloseHandle(mPipe);
            mPipe = INVALID_HANDLE_VALUE;
        }
    }

    bool SleepInterruptible(DWORD milliseconds)
    {
        const DWORD step = 50;
        for (DWORD elapsed = 0; elapsed < milliseconds && mRunning.load(); elapsed += step)
            Sleep((std::min)(step, milliseconds - elapsed));
        return mRunning.load();
    }

    bool WriteAll(HANDLE pipe, const void* data, size_t size)
    {
        const char* bytes = static_cast<const char*>(data);
        while (size != 0)
        {
            const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(size, 64u * 1024u));
            DWORD written = 0;
            const auto result = albion::Write(pipe, bytes, chunk, 500, &mCancelled);
            written = result.bytes;
            if (!result.ok || written == 0 || written > chunk) return false;
            bytes += written;
            size -= written;
        }
        return true;
    }

    bool ReadAll(HANDLE pipe, void* data, size_t size)
    {
        char* bytes = static_cast<char*>(data);
        while (size != 0)
        {
            const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(size, 64u * 1024u));
            DWORD read = 0;
            const auto result = albion::Read(pipe, bytes, chunk, 500, &mCancelled);
            read = result.bytes;
            if (!result.ok || read == 0 || read > chunk) return false;
            bytes += read;
            size -= read;
        }
        return true;
    }

    bool Send(std::uint32_t opcode, const std::string& payload)
    {
        if (!mRunning.load() || mCancelled.load() || mPipe == INVALID_HANDLE_VALUE || payload.size() > kMaxPayload) return false;
        const Header header{ opcode, static_cast<std::uint32_t>(payload.size()) };
        return WriteAll(mPipe, &header, sizeof(header)) &&
            WriteAll(mPipe, payload.data(), payload.size());
    }

    // Returns 1 when a frame was read, 0 when no complete frame is available,
    // and -1 when the pipe is broken or the frame is invalid.
    int TryRead(std::uint32_t& opcode, std::string& payload)
    {
        Header peek{};
        DWORD peeked = 0, available = 0;
        if (!PeekNamedPipe(mPipe, &peek, sizeof(peek), &peeked, &available, nullptr))
            return -1;
        if (peeked < sizeof(peek)) return 0;
        if (peek.length > kMaxPayload) return -1;
        if (available < sizeof(peek) + peek.length) return 0;

        Header header{};
        if (!ReadAll(mPipe, &header, sizeof(header))) return -1;
        payload.assign(header.length, '\0');
        if (header.length != 0 && !ReadAll(mPipe, payload.data(), header.length)) return -1;
        opcode = header.opcode;
        return 1;
    }

    bool OpenPipe()
    {
        ClosePipe();
        for (int index = 0; index < 10 && mRunning.load(); ++index)
        {
            const std::wstring name = L"\\\\?\\pipe\\discord-ipc-" + std::to_wstring(index);
            mPipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (mPipe != INVALID_HANDLE_VALUE) return true;
        }
        return false;
    }

    bool Handshake()
    {
        const std::string hello = std::string("{\"v\":1,\"client_id\":\"") +
            kClientId + "\"}";
        if (!Send(kHandshake, hello)) return false;

        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (mRunning.load() && GetTickCount64() < deadline)
        {
            std::uint32_t opcode = 0;
            std::string payload;
            const int result = TryRead(opcode, payload);
            if (result < 0) return false;
            if (result == 0)
            {
                Sleep(20);
                continue;
            }
            if (opcode == kPing)
            {
                if (!Send(kPong, payload)) return false;
                continue;
            }
            if (opcode == kClose) return false;
            if (opcode == kFrame && payload.find("\"READY\"") != std::string::npos)
                return true;
        }
        return false;
    }

    std::string NextNonce()
    {
        return std::to_string(GetTickCount64()) + "-" + std::to_string(++mNonce);
    }

    bool PublishActivity()
    {
        const std::string payload =
            "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" +
            std::to_string(GetCurrentProcessId()) +
            ",\"activity\":{\"timestamps\":{\"start\":" +
            std::to_string(mStartTime) +
            "},\"assets\":{\"large_image\":\"large_image4\","
            "\"large_text\":\"AlbionAI\",\"small_text\":\"Playing\"}}},"
            "\"nonce\":\"" + NextNonce() + "\"}";
        return Send(kFrame, payload);
    }

    void ClearActivity()
    {
        if (mPipe == INVALID_HANDLE_VALUE) return;
        const std::string payload =
            "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" +
            std::to_string(GetCurrentProcessId()) +
            ",\"activity\":null},\"nonce\":\"" + NextNonce() + "\"}";
        Send(kFrame, payload);
    }

    bool PumpConnection()
    {
        std::uint32_t opcode = 0;
        std::string payload;
        const int result = TryRead(opcode, payload);
        if (result < 0) return false;
        if (result == 0) return true;
        if (opcode == kPing) return Send(kPong, payload);
        return opcode != kClose;
    }

    void Worker()
    {
        while (mRunning.load())
        {
            if (!OpenPipe() || !Handshake() || !PublishActivity())
            {
                ClosePipe();
                SleepInterruptible(5000);
                continue;
            }

            // Keep the IPC session alive and reconnect automatically when the
            // Discord desktop client is restarted after this application.
            while (mRunning.load())
            {
                if (!PumpConnection()) break;
                SleepInterruptible(250);
            }

            // FIX-V11-10: Closing the pipe clears presence; never start a new write after Stop().
            ClosePipe();
            if (mRunning.load()) SleepInterruptible(2000);
        }
        ClosePipe();
    }
};
