#pragma once

#include <cstdio>
#include <string>
#include <string_view>

namespace BioForge::Json
{
    // Escape a string for embedding in the context JSON the DLL hands to
    // SkyrimNet. There is no JSON library in this plugin on purpose: the two
    // SkyrimNet payloads we consume are flat and fixed-shape, and the context
    // we build is ours, so a dependency would buy nothing. Adding one would be
    // a deliberate reversal, not a cleanup.
    inline std::string Escape(std::string_view a_s)
    {
        std::string out;
        out.reserve(a_s.size() + 8);
        for (const char ch : a_s) {
            switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    out += buf;
                } else {
                    out += ch;
                }
            }
        }
        return out;
    }
}
