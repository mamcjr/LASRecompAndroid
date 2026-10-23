// The menus' text in Brazilian Portuguese on a phone set to Portuguese (Android; settings_host_portuguese): each
// English string looked up in ui/i18n_pt.inc, anything not in it shown as written. Elsewhere tr() is the string itself.
// Display text only: labels used as ImGui IDs stay the English ones.
#pragma once
#include <string>

const char *tr(const char *en);
// In capitals (section titles, row labels): ASCII, and the accented Latin-1 letters in UTF-8 (á -> Á, ç -> Ç)
std::string ui_upper(const char *s);
