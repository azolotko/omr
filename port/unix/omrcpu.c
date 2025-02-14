/*******************************************************************************
 * Copyright IBM Corp. and others 1991
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

/**
 * @file
 * @ingroup Port
 * @brief CPU Control.
 *
 * Functions setting CPU attributes.
 */


#include <stdlib.h>
#if defined(RS6000) || defined (LINUXPPC) || defined (PPC)
#include <string.h>
#endif
#include "omrport.h"
#include "omrutil.h"
#if defined(RS6000) || defined (LINUXPPC) || defined (PPC)
#include "omrportpriv.h"
#include "omrportpg.h"
#endif

#if defined(OSX)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

/**
 * PortLibrary startup.
 *
 * This function is called during startup of the portLibrary.  Any resources that are required for
 * the exit operations may be created here.  All resources created here should be destroyed
 * in @ref omrcpu_shutdown.
 *
 * @param[in] portLibrary The port library
 *
 * @return 0 on success, negative error code on failure.  Error code values returned are
 * \arg OMRPORT_ERROR_STARTUP_CPU
 *
 * @note Most implementations will simply return success.
 */
int32_t
omrcpu_startup(struct OMRPortLibrary *portLibrary)
{
#if defined(LINUXPPC) || defined(PPC) || defined(RS6000)
	/* initialize the ppc level 1 cache line size */
	PPG_mem_ppcCacheLineSize = getCacheLineSize();
#endif /* defined(LINUXPPC) || defined(PPC) || defined(RS6000) */

	return 0;
}

/**
 * PortLibrary shutdown.
 *
 * This function is called during shutdown of the portLibrary.  Any resources that were created by @ref omrcpu_startup
 * should be destroyed here.
 *
 * @param[in] portLibrary The port library
 *
 * @note Most implementations will be empty.
 */
void
omrcpu_shutdown(struct OMRPortLibrary *portLibrary)
{
}

/**
 * @brief CPU Control operations.
 *
 * Flush the instruction cache to memory.
 *
 * @param[in] portLibrary The port library
 * @param[in] memoryPointer The base address of memory to flush.
 * @param[in] byteAmount Number of bytes to flush.
 */
void
omrcpu_flush_icache(struct OMRPortLibrary *portLibrary, void *memoryPointer, uintptr_t byteAmount)
{
#if defined(LINUXPPC) || defined(PPC) || defined(RS6000)
	uintptr_t cacheLineSize = PPG_mem_ppcCacheLineSize;
	unsigned char *addr = NULL;
	unsigned char *limit = (unsigned char *)
			(((uintptr_t)memoryPointer + byteAmount + (cacheLineSize - 1))
					/ cacheLineSize * cacheLineSize);

	/* for each cache line, do a data cache block flush */
	for (addr = (unsigned char *)memoryPointer ; addr < limit; addr += cacheLineSize) {
		__asm__(
			"dcbst 0,%0"
			: /* no outputs */
			: "r"(addr));
	}

	__asm__("sync");

	/* for each cache line  do an icache block invalidate */
	for (addr = (unsigned char *)memoryPointer; addr < limit; addr += cacheLineSize) {
		__asm__(
			"icbi 0,%0"
			: /* no outputs */
			: "r"(addr));
	}

	__asm__("sync");
	__asm__("isync");

#elif defined(AARCH64) || defined(ARM) /* defined(LINUXPPC) || defined(PPC) || defined(RS6000) */
#if defined(__GNUC__)
	// GCC built-in function
	__builtin___clear_cache(memoryPointer, (void *)((unsigned char *)memoryPointer + byteAmount));
#else /* defined(__GNUC__) */
#error Not supported
#endif /* defined(__GNUC__) */
#endif /* defined(LINUXPPC) || defined(PPC) || defined(RS6000) */
}

int32_t
omrcpu_get_cache_line_size(struct OMRPortLibrary *portLibrary, int32_t *lineSize)
{
	int32_t rc = OMRPORT_ERROR_NOT_SUPPORTED_ON_THIS_PLATFORM;
	if (NULL == lineSize) {
		rc = OMRPORT_ERROR_INVALID_ARGUMENTS;
	} else {
#if defined(RS6000) || defined (LINUXPPC) || defined (PPC)
		*lineSize = PPG_mem_ppcCacheLineSize;
		rc = 0;
#elif defined (OSX)
		int queryPath[] = {CTL_HW, HW_CACHELINE};
		int64_t result = 0;
		size_t resultSize = sizeof(result);
		if (0 != sysctl(queryPath, 2, &result, &resultSize, NULL, 0)) {
			rc = OMRPORT_ERROR_SYSINFO_OPFAILED;
		} else {
			*lineSize = (int32_t) result;
			rc = 0;
		}
#endif /* defined(OSX) */
	}
	return rc;
}
