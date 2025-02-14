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

/*
 * Define the constants and structures used by the generated trace code to create
 * trace points for each module.
 */
#ifndef _IBM_UTE_MODULE_H
#define _IBM_UTE_MODULE_H

/*
 * @ddr_namespace: map_to_type=UteModuleConstants
 */

#define UTE_VERSION_1_1                0x7E000101

#include <stdio.h>

#if defined(LINUX) || defined(OSX)
#include <unistd.h>
#endif /* defined(LINUX) || defined(OSX) */

#include "omrcomp.h"


#if defined(OMR_RAS_TDF_TRACE)
#define UT_TRACE_ENABLED_IN_BUILD
#endif /* defined(OMR_RAS_TDF_TRACE) */

#ifdef  __cplusplus
extern "C" {
#endif

/*
 * =============================================================================
 *   Constants
 * =============================================================================
 */

#define UT_SPECIAL_ASSERTION 0x00400000

/*
 * =============================================================================
 *   Forward declarations
 * =============================================================================
 */

struct  UtServerInterface;
typedef struct UtServerInterface UtServerInterface;

struct  UtClientInterface;
typedef struct  UtClientInterface UtClientInterface;

struct  UtModuleInterface;
typedef struct UtModuleInterface UtModuleInterface;

/*
 * =============================================================================
 *   The combined UT server/client/module interface
 * =============================================================================
 */

/*
 * UtClientInterface is obsolete. It is included only to maintain binary compatibility
 * with existing binaries that access UtInterface.module (e.g. class library).
 */
typedef struct  UtInterface {
	UtServerInterface *server;
	UtClientInterface *client_unused;
	UtModuleInterface *module;
} UtInterface;

typedef struct UtGroupDetails {
	char                  *groupName;
	int32_t                 count;
	int32_t                *tpids;
	struct UtGroupDetails *next;
} UtGroupDetails;

typedef struct UtTraceVersionInfo {
	int32_t								 		traceVersion;
} UtTraceVersionInfo;

/*
 * =============================================================================
 *  UT module info
 * =============================================================================
 */

typedef struct UtModuleInfo {
	char              	*name;
	int32_t             	namelength;
	int32_t             	count;
	int32_t             	moduleId;
	unsigned char     	*active;
	UtModuleInterface 	*intf;
	char              	*properties;
	UtTraceVersionInfo	*traceVersionInfo;
	char              	*formatStringsFileName;
	unsigned char     	*levels;
	UtGroupDetails    	*groupDetails;
	struct UtModuleInfo	*next;
	struct UtModuleInfo	*containerModule;
	uint32_t				referenceCount;
	/* Fields added at version 8. Legacy binaries may use the interface
	 * so we must check the traceVersionInfo structure before
	 */
	int32_t				isAuxiliary;
} UtModuleInfo;

#define TRACE_VERSION_WITH_AUXILIARY 8
#define MODULE_IS_AUXILIARY(x) ( x->traceVersionInfo->traceVersion >= TRACE_VERSION_WITH_AUXILIARY && x->isAuxiliary )

/*
 * =============================================================================
 *   The module interface for indirect calls into UT
 * =============================================================================
 */

struct  UtModuleInterface {
	void (*Trace)(void *env, UtModuleInfo *modInfo, uint32_t traceId, const char *spec, ...);
	void (*TraceMem)(void *env, UtModuleInfo *modInfo, uint32_t traceId, uintptr_t length, void *mem);
	void (*TraceState)(void *env, UtModuleInfo *modInfo, uint32_t traceId, const char *, ...);
	void (*TraceInit)(void *env, UtModuleInfo *mod);
	void (*TraceTerm)(void *env, UtModuleInfo *mod);
};

#ifdef  __cplusplus
}
#endif

#endif /* !_IBM_UTE_MODULE_H */
