// Media Player Classic PS5: MediaPlayerClassicPS5-helper.elf built into the app (ProsperoJailbreak.h). Only the native eboot links
// this file; the Makefile passes HELPER_ELF, the helper payload's path.
//
// SPDX-License-Identifier: MIT

#include "ProsperoJailbreak.h"

__asm__(".section .rodata\n"
		".balign 16\n"
		".global mpcps5_helper_elf_begin\n"
		"mpcps5_helper_elf_begin:\n"
		".incbin \"" HELPER_ELF "\"\n"
		".global mpcps5_helper_elf_end\n"
		"mpcps5_helper_elf_end:\n"
		".previous\n");
extern "C" const unsigned char mpcps5_helper_elf_begin[];
extern "C" const unsigned char mpcps5_helper_elf_end[];

namespace jailbreak
{
Blob EmbeddedHelper()
{
	return {mpcps5_helper_elf_begin, size_t(mpcps5_helper_elf_end - mpcps5_helper_elf_begin)};
}
} // namespace jailbreak
