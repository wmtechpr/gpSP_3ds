#ifndef _3DSCHEAT_PARSE_H_
#define _3DSCHEAT_PARSE_H_

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

struct ParsedCheat {
    std::string name;
    std::string code;
    bool enabled = false;
};

namespace cheatparse_detail {
    inline std::string trim(const std::string& s) {
        size_t start = 0;
        while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n'))
            start++;
        size_t end = s.size();
        while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n'))
            end--;
        return s.substr(start, end - start);
    }

    inline std::string unquote(const std::string& s) {
        std::string t = trim(s);
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
            std::string out;
            out.reserve(t.size() - 2);
            for (size_t i = 1; i + 1 < t.size(); i++) {
                if (t[i] == '\\' && i + 2 < t.size()) {
                    i++;
                }
                out.push_back(t[i]);
            }
            return out;
        }
        return t;
    }

    inline bool isHexString(const std::string& s) {
        if (s.empty()) return false;
        for (char c : s) {
            if (!isxdigit((unsigned char)c))
                return false;
        }
        return true;
    }

    inline void appendSemicolon(std::string& dst, const std::string& token) {
        if (!dst.empty())
            dst.push_back(';');
        dst.append(token);
    }
}

// Parses a RetroArch .cht file into `out` (up to `maxCheats`).
inline void parseChtText(const char* text, std::vector<ParsedCheat>& out, size_t maxCheats = 256) {
    out.clear();
    if (!text) return;

    int declaredCount = -1;
    std::vector<ParsedCheat> entries;

    const char* p = text;
    while (*p) {
        const char* eol = strchr(p, '\n');
        size_t lineLen = eol ? (size_t)(eol - p) : strlen(p);
        std::string line = cheatparse_detail::trim(std::string(p, lineLen));
        p = eol ? eol + 1 : p + lineLen;

        if (line.empty() || line[0] == '#')
            continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = cheatparse_detail::trim(line.substr(0, eq));
        std::string val = cheatparse_detail::trim(line.substr(eq + 1));

        if (key == "cheats") {
            int n = atoi(cheatparse_detail::unquote(val).c_str());
            if (n > 0) {
                declaredCount = n;
                if ((size_t)declaredCount > maxCheats)
                    declaredCount = (int)maxCheats;
                if (entries.size() < (size_t)declaredCount)
                    entries.resize(declaredCount);
            }
            continue;
        }

        if (strncmp(key.c_str(), "cheat", 5) != 0)
            continue;

        char* endPtr = nullptr;
        unsigned long idx = strtoul(key.c_str() + 5, &endPtr, 10);
        if (!endPtr || *endPtr != '_' || idx >= maxCheats)
            continue;

        if (entries.size() <= idx)
            entries.resize(idx + 1);

        const char* field = endPtr + 1;
        if (strcmp(field, "desc") == 0) {
            entries[idx].name = cheatparse_detail::unquote(val);
        } else if (strcmp(field, "code") == 0) {
            entries[idx].code = cheatparse_detail::unquote(val);
        } else if (strcmp(field, "enable") == 0) {
            std::string b = cheatparse_detail::unquote(val);
            for (char& c : b) c = (char)tolower((unsigned char)c);
            entries[idx].enabled = (b == "true" || b == "1");
        }
    }

    size_t limit = entries.size();
    if (declaredCount >= 0 && (size_t)declaredCount < limit)
        limit = (size_t)declaredCount;

    for (size_t i = 0; i < limit; i++) {
        if (entries[i].code.empty() && entries[i].name.empty())
            continue;
        if (entries[i].name.empty())
            entries[i].name = entries[i].code;
        out.push_back(entries[i]);
    }
}

// Splits a cheat entry's code string (which may join subcodes with '+', ';', ',', or spaces)
// into semicolon-separated Game Genie and GameShark strings accepted by Gambatte.
inline void classifyCheatParts(const char* codeStr, std::string& outGameGenie, std::string& outGameShark) {
    if (!codeStr) return;

    std::string token;
    auto flushToken = [&]() {
        if (token.empty()) return;
        for (char& c : token) c = (char)toupper((unsigned char)c);

        if (token.find('-') != std::string::npos) {
            cheatparse_detail::appendSemicolon(outGameGenie, token);
        } else if (token.size() == 8 && cheatparse_detail::isHexString(token)) {
            cheatparse_detail::appendSemicolon(outGameShark, token);
        }
        token.clear();
    };

    for (const char* p = codeStr; *p; p++) {
        if (*p == '+' || *p == ';' || *p == ',' || *p == ' ' || *p == '\t') {
            flushToken();
        } else {
            token.push_back(*p);
        }
    }
    flushToken();
}

inline std::string serializeChtText(const std::vector<ParsedCheat>& cheats) {
    std::string out;
    char buf[64];
    snprintf(buf, sizeof(buf), "cheats = %zu\n", cheats.size());
    out.append(buf);

    auto appendEscaped = [&](const std::string& s) {
        out.push_back('"');
        for (char c : s) {
            if (c == '"' || c == '\\')
                out.push_back('\\');
            out.push_back(c);
        }
        out.push_back('"');
    };

    for (size_t i = 0; i < cheats.size(); i++) {
        snprintf(buf, sizeof(buf), "\ncheat%zu_desc = ", i);
        out.append(buf);
        appendEscaped(cheats[i].name);
        out.push_back('\n');

        snprintf(buf, sizeof(buf), "cheat%zu_code = ", i);
        out.append(buf);
        appendEscaped(cheats[i].code);
        out.push_back('\n');

        snprintf(buf, sizeof(buf), "cheat%zu_enable = %s\n", i, cheats[i].enabled ? "true" : "false");
        out.append(buf);
    }

    return out;
}

#endif
