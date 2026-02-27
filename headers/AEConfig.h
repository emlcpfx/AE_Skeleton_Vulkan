#ifndef AECONFIG_H
#define AECONFIG_H

// OS detection
#if defined(_WIN32)
	#define AE_OS_WIN
#elif defined(__GNUC__) && defined(__MACH__)
	#include "TargetConditionals.h"
	#if TARGET_OS_MAC
		#define AE_OS_MAC
	#endif
#elif Rez
	#define AE_OS_MAC
#else
	#error "unrecognized AE platform"
#endif

// Processor detection
#if defined(__i386__) || defined(_M_IX86)
	#define AE_PROC_INTEL
#elif defined(_M_X64) || defined(__amd64__) || defined(__x86_64__)
	#define AE_PROC_INTELx64
#elif defined(__arm64__) || defined(__aarch64__) || defined(_M_ARM64)
	#define AE_PROC_ARM64
#else
	#error "unrecognized AE processor"
#endif

// Byte order (x86 and ARM in AE context are little-endian)
#define AE_LITTLE_ENDIAN

#endif // AECONFIG_H
