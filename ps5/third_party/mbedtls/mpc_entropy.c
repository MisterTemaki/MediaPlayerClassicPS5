/* Mbed TLS randomness for MBEDTLS_ENTROPY_HARDWARE_ALT: the kernel's (getrandom on Linux,
   kern.arandom on the PS5's FreeBSD kernel, at most 256 bytes a call). */
#include <stddef.h>
#include <sys/types.h>
#if defined(__linux__)
#include <sys/random.h>
#else
#include <sys/sysctl.h>
#endif
int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen)
{
	(void)data;
	*olen = 0;
	while (*olen < len)
	{
#if defined(__linux__)
		const ssize_t n = getrandom(output + *olen, len - *olen, 0);
		if (n <= 0)
			break;
		*olen += (size_t)n;
#else
		int mib[2] = {CTL_KERN, KERN_ARND};
		size_t n = len - *olen > 256 ? 256 : len - *olen;
		if (sysctl(mib, 2, output + *olen, &n, NULL, 0) != 0 || n == 0)
			break;
		*olen += n;
#endif
	}
	return 0;
}
