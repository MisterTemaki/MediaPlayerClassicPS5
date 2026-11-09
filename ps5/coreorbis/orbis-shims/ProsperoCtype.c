/* Media Player Classic PS5 1.6: the C locale's remaining entry points, for the GPU build.
 *
 * The GPU build links ps5-native-runtime-mit's platform layer (mitrt/platform/src/ps5rt_locale.c: the C locale's
 * *_l functions RADV and glslang's libc++ iostreams call, and the C locale's table, _DefaultRuneLocale). The player's
 * code and its libraries (FFmpeg, libass, FreeType...) also call the plain ctype / locale entry points FreeBSD's
 * headers expand to (isalpha() of a byte >= 128 calls ___runetype, MB_CUR_MAX calls ___mb_cur_max...). The SDK's
 * libc.a has those only in no-locale.o, which defines the *_l functions too: linked together with the platform's they
 * would be defined twice. So these few come from here, on the platform layer's C locale, and no-locale.o stays out.
 * Every answer is the C locale's, as no-locale.o's are (a title has no other).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define __RUNETYPE_INTERNAL 1
#include <ctype.h>
#include <runetype.h>
#include <stdlib.h>
#include <xlocale.h>

unsigned long ___runetype_l(__ct_rune_t c, locale_t loc);
__ct_rune_t ___tolower_l(__ct_rune_t c, locale_t loc);
__ct_rune_t ___toupper_l(__ct_rune_t c, locale_t loc);

unsigned long ___runetype(__ct_rune_t c)
{
	return ___runetype_l(c, NULL);
}

__ct_rune_t ___tolower(__ct_rune_t c)
{
	return ___tolower_l(c, NULL);
}

__ct_rune_t ___toupper(__ct_rune_t c)
{
	return ___toupper_l(c, NULL);
}

int __tolower_l(int c, locale_t loc)
{
	return ___tolower_l(c, loc);
}

int __toupper_l(int c, locale_t loc)
{
	return ___toupper_l(c, loc);
}

int ___mb_cur_max(void)
{
	return 1; /* the C locale: one byte a character */
}

const _RuneLocale *__getCurrentRuneLocale(void)
{
	return &_DefaultRuneLocale;
}

/* the thread's locale: always the global one (the C locale); setting another is accepted and changes nothing */
locale_t uselocale(locale_t loc)
{
	(void)loc;
	return LC_GLOBAL_LOCALE;
}
