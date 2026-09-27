#include "pch.h"

#include "SkyrimNetWeb.h"

#include "ContentLibrary.h"
#include "Json.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

// winhttp.h drags in windows.h macros; pch.h has already included the
// Windows headers via CommonLib, and the (std::min) defence in CLAUDE.md is
// about exactly this - no bare std::min is used below.
#include <winhttp.h>

namespace BioForge::Web
{
    namespace
    {
        struct Endpoint
        {
            std::string host{ "127.0.0.1" };
            int         port{ 8080 };
            bool        enabled{ true };
        };

        // WebServer.yaml is four flat scalar lines (host: 127.0.0.1 /
        // port: 8080 / ...). Hand-scanned for the same reason every other
        // small fixed-shape file in this plugin is: no JSON/YAML dependency
        // is wanted here, and this shape cannot surprise us - SkyrimNet
        // writes it.
        Endpoint ReadEndpoint()
        {
            Endpoint ep;

            std::ifstream in{ ContentLibrary::SkyrimNetDir() / "config" / "WebServer.yaml",
                              std::ios::binary };
            if (!in) {
                return ep;   // not written yet: server defaults apply
            }

            std::string line;
            while (std::getline(in, line)) {
                auto value = [](std::string_view a_line) {
                    const auto colon = a_line.find(':');
                    if (colon == std::string_view::npos) {
                        return std::string{};
                    }
                    auto v = a_line.substr(colon + 1);
                    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) {
                        v.remove_prefix(1);
                    }
                    while (!v.empty() &&
                           (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) {
                        v.remove_suffix(1);
                    }
                    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
                        v = v.substr(1, v.size() - 2);
                    }
                    return std::string{ v };
                };

                if (line.starts_with("host:")) {
                    const auto v = value(line);
                    if (!v.empty()) {
                        ep.host = v;
                    }
                } else if (line.starts_with("port:")) {
                    try {
                        ep.port = std::stoi(value(line));
                    } catch (...) {
                    }
                } else if (line.starts_with("enabled:")) {
                    auto v = value(line);
                    std::transform(v.begin(), v.end(), v.begin(),
                                   [](unsigned char c) {
                                       return static_cast<char>(std::tolower(c));
                                   });
                    ep.enabled = !(v == "false" || v == "0" || v == "no" || v == "off");
                }
            }
            return ep;
        }

        // "true" / "false" after the "success" key, defaulting to true when
        // the response omits it (a plain 200 is how the file-editor endpoint
        // answers). Hand-scanned: the payload is one flat object.
        bool ResponseSaysSuccess(std::string_view a_body)
        {
            const auto key = a_body.find("\"success\"");
            if (key == std::string_view::npos) {
                return true;
            }
            // The value right after the colon, not any "false" later in the
            // body - another field could carry one.
            auto at = a_body.find(':', key);
            if (at == std::string_view::npos) {
                return true;
            }
            at = a_body.find_first_not_of(" \t\r\n", at + 1);
            return at == std::string_view::npos || a_body.substr(at, 5) != "false";
        }

        std::string ResponseMessage(std::string_view a_body)
        {
            // The endpoints answer {"success":false,"message":"..."} (or
            // errorMessage). Best-effort extraction for the review note.
            for (const auto* key : { "\"message\"", "\"errorMessage\"" }) {
                const auto at = a_body.find(key);
                if (at == std::string_view::npos) {
                    continue;
                }
                const auto open = a_body.find('"', at + std::strlen(key));
                if (open == std::string_view::npos) {
                    continue;
                }
                const auto close = a_body.find('"', open + 1);
                if (close == std::string_view::npos) {
                    continue;
                }
                return std::string{ a_body.substr(open + 1, close - open - 1) };
            }
            return {};
        }

        // ASCII-only widen: hosts, verbs and query paths never carry
        // anything above 0x7F, so a per-char copy is exact.
        std::wstring Wide(std::string_view a_s)
        {
            std::wstring out;
            out.reserve(a_s.size());
            for (const char ch : a_s) {
                out += static_cast<wchar_t>(static_cast<unsigned char>(ch));
            }
            return out;
        }

        struct RequestOutcome
        {
            bool        httpOk{};
            int         status{};
            std::string body;
            std::string error;   // transport-level failure text
        };

        RequestOutcome Request(const char* a_verb, const Endpoint& a_ep,
                               const char* a_pathAndQuery, const std::string& a_body)
        {
            RequestOutcome out;

            const std::wstring host{ Wide(a_ep.host) };
            const std::wstring path{ Wide(a_pathAndQuery) };

            // The server is on the loopback interface and answers fast; the
            // generous receive window is for the content rescan a write
            // kicks off, which on a several-thousand-file library took
            // seconds even in Beta 24.
            void* session = WinHttpOpen(L"BioForge/1.1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!session) {
                out.error = "WinHttpOpen failed";
                return out;
            }
            WinHttpSetTimeouts(session, 5000, 5000, 5000, 20000);

            void* connection = WinHttpConnect(session, host.c_str(),
                                              static_cast<INTERNET_PORT>(a_ep.port), 0);
            if (!connection) {
                out.error = "could not connect to " + a_ep.host + ":" +
                            std::to_string(a_ep.port);
                WinHttpCloseHandle(session);
                return out;
            }

            void* request = WinHttpOpenRequest(connection, Wide(a_verb).c_str(),
                                               path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
            if (!request) {
                out.error = "WinHttpOpenRequest failed";
                WinHttpCloseHandle(connection);
                WinHttpCloseHandle(session);
                return out;
            }

            const std::string headers =
                "Content-Type: application/json\r\nContent-Length: " +
                std::to_string(a_body.size()) + "\r\n";

            bool sent = WinHttpSendRequest(
                request, Wide(headers).c_str(), static_cast<DWORD>(-1),
                const_cast<char*>(a_body.data()),
                static_cast<DWORD>(a_body.size()),
                static_cast<DWORD>(a_body.size()), 0);
            if (sent) {
                sent = WinHttpReceiveResponse(request, nullptr);
            }

            if (sent) {
                DWORD status = 0;
                DWORD size   = sizeof(status);
                WinHttpQueryHeaders(request,
                                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                    WINHTTP_NO_HEADER_INDEX);
                out.status = static_cast<int>(status);

                DWORD available = 0;
                while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
                    std::string chunk(available, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) {
                        break;
                    }
                    chunk.resize(read);
                    out.body += chunk;
                    available = 0;
                }
                out.httpOk = out.status >= 200 && out.status < 300;
            } else {
                out.error = "request failed (server not running?)";
            }

            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return out;
        }
    }

    bool ServerEnabled()
    {
        return ReadEndpoint().enabled;
    }

    CommitResult CreateOrUpdateBio(std::string_view a_displayName,
                                   std::uint64_t a_actorUUID, std::string_view a_stem,
                                   std::string_view a_content)
    {
        const auto ep = ReadEndpoint();
        if (!ep.enabled) {
            return { false,
                     "SkyrimNet's web server is disabled - enable it in the dashboard "
                     "(Web Server settings), then commit again" };
        }

        const std::string content = Json::Escape(a_content);

        // The dashboard's own generated-bio flow: create first (this is what
        // wires the file to the actor), update on 409-already-exists - which
        // is exactly how a re-commit presents. Both endpoints validate the
        // bio as a template before writing, so a malformed block layout is
        // refused rather than half-committed.
        {
            char uuidHex[32]{};
            std::snprintf(uuidHex, sizeof(uuidHex), "%llX",
                          static_cast<unsigned long long>(a_actorUUID));

            const std::string body = "{\"name\":\"" + Json::Escape(a_displayName) +
                                     "\",\"actorUUID\":\"" + uuidHex +
                                     "\",\"content\":\"" + content + "\"}";

            auto r = Request("POST", ep, "/characters?api=create", body);
            if (r.httpOk && ResponseSaysSuccess(r.body)) {
                logs::info("commit: created via characters api{}"sv,
                           r.body.empty() ? "" : " - " + r.body);
                return { true, "committed into your SkyrimNet overlay" };
            }
            logs::info("commit: create answered {}{}{}"sv, r.status,
                       r.error.empty() ? "" : " " + r.error,
                       r.body.empty() ? "" : " - " + r.body);
        }

        // Already exists (HTTP 409), or the UUID form was refused: the
        // update endpoint writes by content path alone, which BioForge
        // always knows - SkyrimNet's own template name or the derived
        // convention.
        {
            const std::string path = "characters/" + std::string{ a_stem } + ".prompt";
            const std::string body = "{\"path\":\"" + path + "\",\"content\":\"" + content +
                                     "\"}";

            auto r = Request("PUT", ep, "/characters?api=update", body);
            if (r.httpOk && ResponseSaysSuccess(r.body)) {
                logs::info("commit: updated {} via characters api"sv, path);
                return { true, "committed into your SkyrimNet overlay" };
            }

            const auto why = !r.error.empty() ? r.error
                              : !r.body.empty() ? ResponseMessage(r.body)
                                                : ("HTTP " + std::to_string(r.status));
            return { false,
                     "SkyrimNet would not take the bio (" + why +
                         "). Check the web server is running (dashboard, port " +
                         std::to_string(ep.port) + ")." };
        }
    }
}
