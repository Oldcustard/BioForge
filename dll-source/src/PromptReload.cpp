#include "pch.h"

#include "PromptReload.h"
#include "StagingStore.h"

#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>

#pragma comment(lib, "winhttp.lib")

// WinHTTP rather than raw winsock: winsock2.h must precede every windows.h in
// the TU, which is unwinnable under CMake's force-included PCH. It also replaces
// an earlier WinInet attempt that failed at InternetOpen inside the game process
// - WinHTTP has no such trouble and reports a real error code when it does fail.

namespace
{
    // SkyrimNet's web config names the port somewhere in this file; the
    // nesting has changed between versions, so scan rather than parse YAML.
    int PortFromConfig(const std::filesystem::path& a_path, bool& a_enabled)
    {
        int  port = 8080;
        a_enabled = true;

        std::ifstream in{ a_path };
        if (!in) {
            return port;   // not shipped / not found: assume the default port
        }

        std::string line;
        while (std::getline(in, line)) {
            const auto colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            if (line.find("enabled") != std::string::npos) {
                a_enabled = line.find("true") != std::string::npos;
            } else if (line.find("port") != std::string::npos) {
                const auto parsed = std::atoi(line.c_str() + colon + 1);
                if (parsed > 0 && parsed < 65536) {
                    port = parsed;
                }
            }
        }
        return port;
    }

    std::string WithError(std::string_view a_what)
    {
        return std::string{ a_what } + " (error " + std::to_string(GetLastError()) + ")";
    }
}

namespace
{
    // Coalescing state for Schedule(). One worker at a time; it waits for a
    // quiet gap before reloading, so a run of commits collapses into one.
    std::mutex g_mutex;
    bool       g_pending = false;
    bool       g_running = false;

    // Long enough to swallow a player clicking Commit down a list, short
    // enough that a single commit goes live while they are still looking at it.
    constexpr auto kQuietPeriod = std::chrono::milliseconds{ 750 };
}

namespace BioForge::PromptReload
{
    void Schedule()
    {
        {
            std::lock_guard lock{ g_mutex };
            g_pending = true;
            if (g_running) {
                return;   // the worker will pick this up
            }
            g_running = true;
        }

        // Detached: this is pure WinHTTP and a file read, no game state, and
        // Skyrim has no reliable shutdown hook to join against. If the process
        // dies mid-request the OS tears the thread down with it.
        std::thread([] {
            for (;;) {
                // Wait for things to go quiet - each new request restarts the
                // clock, so a burst of commits produces exactly one reload.
                for (;;) {
                    {
                        std::lock_guard lock{ g_mutex };
                        if (!g_pending) {
                            break;
                        }
                        g_pending = false;
                    }
                    std::this_thread::sleep_for(kQuietPeriod);
                }

                const auto error = Request();
                if (error.empty()) {
                    logs::info("commit: SkyrimNet reloaded its prompt cache"sv);
                } else {
                    logs::warn("commit: prompt cache not reloaded - {}"sv, error);
                }

                std::lock_guard lock{ g_mutex };
                if (!g_pending) {
                    g_running = false;
                    return;
                }
            }
        }).detach();
    }

    std::string Request()
    {
        bool       enabled = true;
        const auto port    = PortFromConfig(
            Staging::PromptsDir().parent_path() / "config" / "WebServer.yaml", enabled);
        if (!enabled) {
            return "web server disabled in WebServer.yaml";
        }

        const HINTERNET session = WinHttpOpen(L"BioForge", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) {
            return WithError("WinHttpOpen failed");
        }

        // A hung dashboard request must never stall a commit. The reload itself
        // takes ~1.6s on a full rescan, so allow real headroom on receive.
        WinHttpSetTimeouts(session, 3000, 3000, 3000, 30000);

        std::string result;

        const HINTERNET connect =
            WinHttpConnect(session, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
        if (connect) {
            const HINTERNET request =
                WinHttpOpenRequest(connect, L"POST", L"/prompts?api=reload", nullptr,
                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
            if (request) {
                static char       body[]   = "{}";
                constexpr wchar_t headers[] = L"Content-Type: application/json\r\n";

                if (WinHttpSendRequest(request, headers, static_cast<DWORD>(-1), body, 2, 2, 0) &&
                    WinHttpReceiveResponse(request, nullptr)) {
                    DWORD status = 0;
                    DWORD size   = sizeof(status);
                    if (WinHttpQueryHeaders(request,
                                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                            WINHTTP_NO_HEADER_INDEX)) {
                        result = status == 200
                                     ? ""
                                     : "reload endpoint answered " + std::to_string(status);
                    } else {
                        result = WithError("could not read response status");
                    }
                } else {
                    result = WithError("request failed");
                }
                WinHttpCloseHandle(request);
            } else {
                result = WithError("WinHttpOpenRequest failed");
            }
            WinHttpCloseHandle(connect);
        } else {
            result = WithError("could not connect on port " + std::to_string(port));
        }

        WinHttpCloseHandle(session);
        return result;
    }
}
