// Media Player Classic PS5: the interface's languages.
//
// English is the default and the source: every text on the screen is written in English in the code and passed
// through T(), which returns it in the language picked in View > Options > Player > Language, or as it is when
// there is no translation. A format string keeps its % specifiers in every language (tests/check_i18n.py checks
// them). Menu texts keep their & (the accelerator) in the translation too.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace mpc
{
enum class Language : int
{
	English = 0,
	Spanish,
	PortugueseBrazil,
	French,
	German,
	Count
};

// The language's own name ("Español"...), for the Options' Language box.
const char* LanguageName(int lang);
// The text in the current language (AppSettings().language).
const char* T(const char* english);
inline std::string T(const std::string& english)
{
	return T(english.c_str());
}
// A text that means two things in English ("Type": a column, and the keyboard's "type here"): looked up as
// "context|english" in the table, shown as english in English.
const char* TC(const char* context, const char* english);
// snprintf into a string: Fmt(T("Chapter %d: %s"), n, name)
std::string Fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
} // namespace mpc
