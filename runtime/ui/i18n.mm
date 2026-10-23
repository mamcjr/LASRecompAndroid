// See i18n.h.
#include "i18n.h"

#include <ctype.h>
#include <string.h>
#include <unordered_map>

extern "C" {
#include "../settings.h"
}

#ifdef __ANDROID__
static const char *const kPt[][2] = {
#include "i18n_pt.inc"
};
#endif

const char *tr(const char *en) {
#ifdef __ANDROID__
    if (!en || !*en || !settings_host_portuguese()) return en;
    static const auto *pt = [] {  // (built once, thread-safe: the start screen's callbacks run on Java's threads)
        auto *m = new std::unordered_map<std::string, const char *>();
        for (const auto &e : kPt) m->emplace(e[0], e[1]);
        return m;
    }();
    auto it = pt->find(en);
    return it == pt->end() ? en : it->second;
#else
    return en;
#endif
}

std::string ui_upper(const char *s) {
    std::string t = s ? s : "";
    for (size_t i = 0; i < t.size(); i++) {
        unsigned char c = (unsigned char)t[i];
        if (c < 0x80) t[i] = (char)toupper(c);
#ifdef __ANDROID__  // (the desktop's menus are English: bytes as before)
        else if (c == 0xC3 && i + 1 < t.size()) {  // U+00E0..U+00FE: the lowercase accented letters (but ÷, U+00F7)
            unsigned char d = (unsigned char)t[i + 1];
            if (d >= 0xA0 && d <= 0xBE && d != 0xB7) t[i + 1] = (char)(d - 0x20);
            i++;
        }
#endif
    }
    return t;
}
