/*******************************************************************************
 * Copyright IBM Corp. and others 2020
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
 * @brief Shared Semaphores
 */

#include <stddef.h>
#include <errno.h>
#include "omrportpriv.h"
#include "omrport.h"
#include "portnls.h"
#include "ut_omrport.h"
#include "omrsharedhelper.h"
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "omrshsem.h"
#include "omrsysv_ipcwrappers.h"

/*These flags are only used internally*/
#define CREATE_ERROR -10
#define OPEN_ERROR  -11
#define WRITE_ERROR -12
#define READ_ERROR -13
#define MALLOC_ERROR -14

#define OMRSH_NO_DATA -21
#define OMRSH_BAD_DATA -22

#define RETRY_COUNT 10
#define SLEEP_TIME 5

#define SHSEM_FTOK_PROJID 0xad

static intptr_t omrshsem_ensureBaseFile(struct OMRPortLibrary *portLibrary, char *filename);
static intptr_t omrshsem_getSemHandle(struct OMRPortLibrary * portLibrary, const char * baseFile, int setSize, uint32_t permissions, uint8_t projid, omrshsem_handle **handle);
static omrshsem_handle* omrshsem_createSemHandle(struct OMRPortLibrary *portLibrary, int semid, int nsems, const char* baseFile);
static intptr_t omrshsem_checkControlFileDate(OMRPortLibrary *portLibrary, struct omrshsem_handle *handle, const char *controlFile);

int32_t
omrshsem_params_init(struct OMRPortLibrary *portLibrary, struct OMRPortShSemParameters *params) 
{
 	params->semName = NULL; /* Unique identifier of the semaphore. */
 	params->setSize = 1; /* number of semaphores to be created in this set */
 	params->permission = S_IRUSR | S_IWUSR; /* Posix-style file permissions */
 	params->controlFileDir = NULL; /* Directory in which to create control files (SysV semaphores only */
 	params->proj_id = 1; /* parameter used with semName to generate semaphore key */
 	params->deleteBasefile = 1; /* set to 0 to retain the basefile when destroying the semaphore. */
 	return 0;
}

intptr_t 
omrshsem_open (struct OMRPortLibrary *portLibrary, struct omrshsem_handle **handle, const struct OMRPortShSemParameters *params) 
{
	/* TODO: needs to be longer? dynamic?*/
	intptr_t rc;
	char baseFile[OMRSH_MAXPATH];

	Trc_PRT_shsem_omrshsem_open_Entry(params->semName, params->setSize, params->permission);

	*handle = NULL;
	
	if (params->controlFileDir == NULL) {
		Trc_PRT_shsem_omrshsem_open_Exit4();
		return OMRPORT_ERROR_SHSEM_OPFAILED;
	}

	omrstr_printf(portLibrary, baseFile, OMRSH_MAXPATH,"%s/%s", params->controlFileDir, params->semName);
	
	if (omrshsem_ensureBaseFile(portLibrary, baseFile) != OMRSH_SUCCESS) {
		Trc_PRT_shsem_omrshsem_open_ensureBaseFile_Failed(baseFile);
		return OMRPORT_ERROR_SHSEM_OPFAILED; 
	}
	
	rc = omrshsem_getSemHandle(portLibrary, baseFile, params->setSize, params->permission, params->proj_id, handle);
	if (rc == OMRSH_FAILED) {
		Trc_PRT_shsem_omrshsem_open_getInfoStruct_Failed();
		return OMRPORT_ERROR_SHSEM_OPFAILED; 
	}

	if (rc == OMRPORT_INFO_SHSEM_OPENED) {
		rc = omrshsem_checkControlFileDate(portLibrary, *handle, baseFile);
	}
	(*handle)->deleteBasefile = params->deleteBasefile;
	
	Trc_PRT_shsem_omrshsem_open_ExitWithRC(rc);
	return rc;
}

/**
 * post operation increments the counter in the semaphore by 1 if there is no one in wait for the semaphore. 
 * if there are other processes suspended by wait then one of them will become runnable and 
 * the counter remains the same. 
 * 
 * @param[in] portLibrary The port library.
 * @param[in] handle Semaphore set handle.
 * @param[in] semset The no of semaphore in the semaphore set that you want to post.
 * @param[in] flag The semaphore operation flag:
 * \arg OMRPORT_SHSEM_MODE_DEFAULT The default operation flag, same as 0
 * \arg OMRPORT_SHSEM_MODE_UNDO The changes made to the semaphore will be undone when this process finishes.
 *
 * @return 0 on success, -1 on failure.
 */
intptr_t 
omrshsem_post(struct OMRPortLibrary *portLibrary, struct omrshsem_handle* handle, uintptr_t semset, uintptr_t flag)
{
	struct sembuf buffer;
	intptr_t rc;

	Trc_PRT_shsem_omrshsem_post_Entry1(handle, semset, flag, handle ? handle->semid : -1);
	if(handle == NULL) {
		Trc_PRT_shsem_omrshsem_post_Exit1();
		return OMRPORT_ERROR_SHSEM_HANDLE_INVALID;
	}
	
	if (semset >= handle->nsems) {
		Trc_PRT_shsem_omrshsem_post_Exit2();
		return OMRPORT_ERROR_SHSEM_SEMSET_INVALID;
	}

	buffer.sem_num = semset;
	buffer.sem_op = 1; /* post */
	if(flag & OMRPORT_SHSEM_MODE_UNDO) {
		buffer.sem_flg = SEM_UNDO;
	} else {
		buffer.sem_flg = 0;
	}

	rc = omr_semopWrapper(portLibrary,handle->semid, &buffer, 1);
	
	if (-1 == rc) {
		int32_t myerrno = omrerror_last_error_number(portLibrary);
		Trc_PRT_shsem_omrshsem_post_Exit3(rc, myerrno);
	} else {
		Trc_PRT_shsem_omrshsem_post_Exit(rc);
	}

	return rc;
}
/**
 * Wait operation decrements the counter in the semaphore set if the counter > 0
 * if counter == 0 then the caller will be suspended.
 * 
 * @param[in] portLibrary The port library.
 * @param[in] handle Semaphore set handle.
 * @param[in] semset The no of semaphore in the semaphore set that you want to post.
 * @param[in] flag The semaphore operation flag:
 * \arg OMRPORT_SHSEM_MODE_DEFAULT The default operation flag, same as 0
 * \arg OMRPORT_SHSEM_MODE_UNDO The changes made to the semaphore will be undone when this process finishes.
 * \arg OMRPORT_SHSEM_MODE_NOWAIT The caller will not be suspended if sem == 0, a -1 is returned instead.
 * 
 * @return 0 on success, -1 on failure.
 */
intptr_t
omrshsem_wait(struct OMRPortLibrary *portLibrary, struct omrshsem_handle* handle, uintptr_t semset, uintptr_t flag)
{
	struct sembuf buffer;
 	intptr_t rc;

	Trc_PRT_shsem_omrshsem_wait_Entry1(handle, semset, flag, handle ? handle->semid : -1);  
	if(handle == NULL) {
		Trc_PRT_shsem_omrshsem_wait_Exit1();
		return OMRPORT_ERROR_SHSEM_HANDLE_INVALID;
	}

	if (semset >= handle->nsems) {
		Trc_PRT_shsem_omrshsem_wait_Exit2();
		return OMRPORT_ERROR_SHSEM_SEMSET_INVALID;
	}

	buffer.sem_num = semset;
	buffer.sem_op = -1; /* wait */

	if( flag & OMRPORT_SHSEM_MODE_UNDO ) {
		buffer.sem_flg = SEM_UNDO;
	} else {
		buffer.sem_flg = 0;
	}

	if( flag & OMRPORT_SHSEM_MODE_NOWAIT ) {
		buffer.sem_flg = buffer.sem_flg | IPC_NOWAIT;
	}

	rc = omr_semopWrapper(portLibrary,handle->semid, &buffer, 1);
	if(-1 == rc) {
		int32_t myerrno = omrerror_last_error_number(portLibrary);
		Trc_PRT_shsem_omrshsem_wait_Exit3(rc, myerrno);
	} else {
		Trc_PRT_shsem_omrshsem_wait_Exit(rc);
	}
	
	return rc;
}
/**
 * reading the value of the semaphore in the set. This function
 * uses no synchronisation prmitives
  * 
 * @pre caller has to deal with synchronisation issue.
 *
 * @param[in] portLibrary The port library.
 * @param[in] handle Semaphore set handle.
 * @param[in] semset The number of semaphore in the semaphore set that you want to post.
 * 
 * @return -1 on failure, the value of the semaphore on success
 * 
 * @warning: The user will need to make sure locking is done correctly when
 * accessing semaphore values. This is because getValue simply reads the semaphore
 * value without stopping the access to the semaphore. Therefore the value of the semaphore
 * can change before the function returns. 
 */
intptr_t 
omrshsem_getVal(struct OMRPortLibrary *portLibrary, struct omrshsem_handle* handle, uintptr_t semset) 
{	
	intptr_t rc;
	Trc_PRT_shsem_omrshsem_getVal_Entry1(handle, semset, handle ? handle->semid : -1);
	if(handle == NULL) {
		Trc_PRT_shsem_omrshsem_getVal_Exit1();
		return OMRPORT_ERROR_SHSEM_HANDLE_INVALID;
	}
	
	if (semset >= handle->nsems) {
		Trc_PRT_shsem_omrshsem_getVal_Exit2();
		return OMRPORT_ERROR_SHSEM_SEMSET_INVALID; 
	}

	rc = omr_semctlWrapper(portLibrary, TRUE, handle->semid, semset, GETVAL);
	if (-1 == rc) {
		int32_t myerrno = omrerror_last_error_number(portLibrary);
		Trc_PRT_shsem_omrshsem_getVal_Exit3(rc, myerrno);
	} else {
		Trc_PRT_shsem_omrshsem_getVal_Exit(rc);
	}
	return rc;
}

/**
 * 
 * setting the value of the semaphore specified in semset. This function
 * uses no synchronisation prmitives
 * 
 * @pre Caller has to deal with synchronisation issue.
 * 
 * @param[in] portLibrary The port Library.
 * @param[in] handle Semaphore set handle.
 * @param[in] semset The no of semaphore in the semaphore set that you want to post.
 * @param[in] value The value that you want to set the semaphore to
 * 
 * @warning The user will need to make sure locking is done correctly when
 * accessing semaphore values. This is because setValue simply set the semaphore
 * value without stopping the access to the semaphore. Therefore the value of the semaphore
 * can change before the function returns. 
 *
 * @return 0 on success, -1 on failure.
 */
intptr_t 
omrshsem_setVal(struct OMRPortLibrary *portLibrary, struct omrshsem_handle* handle, uintptr_t semset, intptr_t value)
{
	semctlUnion sem_union;
	
	intptr_t rc;
	
	Trc_PRT_shsem_omrshsem_setVal_Entry1(handle, semset, value, handle ? handle->semid : -1);

	if(handle == NULL) {
		Trc_PRT_shsem_omrshsem_setVal_Exit1();
		return OMRPORT_ERROR_SHSEM_HANDLE_INVALID;
	}
	if (semset >= handle->nsems) {
		Trc_PRT_shsem_omrshsem_setVal_Exit2();
		return OMRPORT_ERROR_SHSEM_SEMSET_INVALID; 
	}
    
	sem_union.val = value;
	
	rc = omr_semctlWrapper(portLibrary, TRUE, handle->semid, semset, SETVAL, sem_union);

	if (-1 == rc) {
		int32_t myerrno = omrerror_last_error_number(portLibrary);
		Trc_PRT_shsem_omrshsem_setVal_Exit3(rc, myerrno);
	} else {
		Trc_PRT_shsem_omrshsem_setVal_Exit(rc);
	}
	return rc;
}


/**
 * Release the resources allocated for the semaphore handles.
 * 
 * @param[in] portLibrary The port library.
 * @param[in] handle Semaphore set handle.
 * 
 * @note The actual semaphore is not destroyed.  Once the close operation has been performed 
 * on the semaphore handle, it is no longer valid and user needs to reissue @ref omrshsem_open call.
 */
void 
omrshsem_close (struct OMRPortLibrary *portLibrary, struct omrshsem_handle **handle)
{
	Trc_PRT_shsem_omrshsem_close_Entry1(*handle, (*handle) ? (*handle)->semid : -1);

	/* On Unix you don't need to close the semaphore handles*/
	if(NULL == *handle) {
		Trc_PRT_shsem_omrshsem_close_ExitNullHandle();
		return;
	}
	omrmem_free_memory(portLibrary, (*handle)->baseFile);
	omrmem_free_memory(portLibrary, *handle);
	*handle = NULL;

	Trc_PRT_shsem_omrshsem_close_Exit();
}



/**
 * Destroy the semaphore and release the resources allocated for the semaphore handles.
 * 
 * @param[in] portLibrary The port library.
 * @param[in] handle Semaphore set handle.
 * 
 * @return 0 on success, -1 on failure.
 * @note Due to operating system restriction we may not be able to destroy the semaphore
 */
intptr_t 
omrshsem_destroy (struct OMRPortLibrary *portLibrary, struct omrshsem_handle **handle)
{
	/*pre: user has not closed the handle, and assume user has perm to remove */
	intptr_t rc, rcunlink;

	Trc_PRT_shsem_omrshsem_destroy_Entry1(*handle, (*handle) ? (*handle)->semid : -1);

	if(*handle == NULL) {
		Trc_PRT_shsem_omrshsem_destroy_ExitNullHandle();
		return OMRSH_SUCCESS;
	}
	
	if (omr_semctlWrapper(portLibrary, TRUE, (*handle)->semid, 0, IPC_RMID)==-1) {
		int32_t myerrno = omrerror_last_error_number(portLibrary);
		int32_t myerrnoMasked = (myerrno | 0xFFFF0000);
		if ((myerrnoMasked == OMRPORT_ERROR_SYSV_IPC_ERRNO_EINVAL)
			|| (myerrnoMasked == OMRPORT_ERROR_SYSV_IPC_ERRNO_EIDRM)
		) {
			/* EINVAL and EIDRM are okay - we just had a reboot so the semaphore id is no good */
			rc = 0;
		} else if ((myerrnoMasked == OMRPORT_ERROR_SYSV_IPC_ERRNO_EPERM)
				|| (myerrnoMasked == OMRPORT_ERROR_SYSV_IPC_ERRNO_EACCES)) {
			/* CMVC 112164 : The semaphore ID is valid, but we don't have permissions to use it. This either means that we've rebooted
			 and someone else now has our semid. OR it means we're trying to access an active cache in use by someone else.
			 In the former case, we want to delete the file and go for a new semid. In the latter case, we should fail - the file is not
			 ours to delete. However, given the dir permissions, we can still unlink it. Therefore, try an operation for which we need
			 to own the file. If it succeeds, that means we own it and can go ahead and unlink it. */
			Trc_PRT_shsem_omrshsem_destroy_Debug2((*handle)->semid, myerrno);
			rc = omrfile_chown(portLibrary,(*handle)->baseFile, OMRPORT_FILE_IGNORE_ID, getegid()); /* Completely benign - done anyway when the file is created */
		} else {
			Trc_PRT_shsem_omrshsem_destroy_Debug2((*handle)->semid, myerrno);
			rc = -1;
		}
	} else {
		rc = 0;
	}
	
	if ((1 == (*handle)->deleteBasefile) && (0 == rc)) {
		int32_t myerrno = omrerror_last_error_number(portLibrary);
		rcunlink = omrfile_unlink(portLibrary, (*handle)->baseFile);
		Trc_PRT_shsem_omrshsem_destroy_Debug1((*handle)->baseFile, rcunlink, myerrno);
	}

	omrshsem_close(portLibrary,handle);

	if(rc==0) {
		Trc_PRT_shsem_omrshsem_destroy_Exit();
	} else {
		Trc_PRT_shsem_omrshsem_destroy_Exit1();
	}
	
	return rc;
}


/**
 * PortLibrary startup.
 *
 * This function is called during startup of the portLibrary.  Any resources that are required for
 * the file operations may be created here.  All resources created here should be destroyed
 * in @ref omrshsem_shutdown.
 *
 * @param[in] portLibrary The port library.
 *
 * @return 0 on success, negative error code on failure.  Error code values returned are
 * \arg OMRPORT_ERROR_STARTUP_SHSEM
 *
 * @note Most implementations will simply return success.
 */
int32_t
omrshsem_startup(struct OMRPortLibrary *portLibrary)
{
	return 0;
}

/**
 * PortLibrary shutdown.
 *
 * This function is called during shutdown of the portLibrary.  Any resources that were created by @ref omrshsem_startup
 * should be destroyed here.
 *
 * @param[in] portLibrary The port library.
 *
 * @note Most implementations will be empty.
 */
void
omrshsem_shutdown(struct OMRPortLibrary *portLibrary)
{
	/* Don't need to do anything for now, but maybe we will need to clean up
	 * some directories, etc over here.
	 */
}

static intptr_t 
omrshsem_ensureBaseFile(struct OMRPortLibrary *portLibrary, char *filename) 
{
	intptr_t fd;
	intptr_t rc = 0;
	gid_t gid = getegid();
	int32_t flags = EsOpenCreate | EsOpenWrite | EsOpenCreateNew;

	Trc_PRT_shsem_omrshsem_createbaseFile_Entry(filename);
 
	fd = omrfile_open(portLibrary, filename, flags, OMRSH_BASEFILEPERM);

	if (-1 == fd) {
		int32_t errorno = omrerror_last_error_number(portLibrary);

		if(OMRPORT_ERROR_FILE_EXIST == errorno) {
			Trc_PRT_shsem_omrshsem_createbaseFile_ExitExists();
			return OMRSH_SUCCESS;
		}

		Trc_PRT_shsem_omrshsem_createbaseFile_ExitError(errorno);
		return CREATE_ERROR;
	}
    
	omrfile_close(portLibrary, fd);

	Trc_PRT_shsem_omrshsem_createbaseFile_Event1(filename, gid);
	rc = omrfile_chown(portLibrary,filename, OMRPORT_FILE_IGNORE_ID, gid);
	Trc_PRT_shsem_omrshsem_createbaseFile_Event2(rc, errno);

	Trc_PRT_shsem_omrshsem_createbaseFile_Exit();
	return OMRSH_SUCCESS;
}

static intptr_t 
omrshsem_getSemHandle(struct OMRPortLibrary * portLibrary, const char * baseFile, int setSize, uint32_t permissions, uint8_t projid, omrshsem_handle **handle)
{
	/*ftok variables*/
	key_t fkey;
	
	/*semget variables*/
	int semid;
	int semflags_create;
	int semflags_open;
	int32_t iCreatedTheSemaphoreSet = 0;
	
	Trc_PRT_shsem_omrshsem_getInfoStruct_Entry();
	
	/* trim the permissions down to 9 least significant bits */
	permissions &= 0777;
	semflags_open  = OMRSHSEM_SEMFLAGS_OPEN | permissions;
	semflags_create = OMRSHSEM_SEMFLAGS  | permissions;
	/* Generate the key for creating the semaphore*/
	fkey = omr_ftokWrapper(portLibrary,baseFile, projid);
	if (-1 == fkey) {
		int32_t myerror = omrerror_last_error_number(portLibrary);
		Trc_PRT_shsem_omrshsem_stat_ftok_Failed(myerror);
		Trc_PRT_shsem_omrshsem_getInfoStruct_Exit();
		return OMRSH_FAILED;
	}
	
	/* Open or create the semaphore */
	/* 
	 * Notice that we create with setSize+1 - we create an extra semaphore
	 * so that we can put a "Marker" in there to say this is one of us 
	 */
	semid = omr_semgetWrapper(portLibrary, fkey, setSize + 1, semflags_create);
	if (-1 == semid) {
		int32_t lasterrno = omrerror_last_error_number(portLibrary);
		if ((lasterrno | 0xFFFF0000) == OMRPORT_ERROR_SYSV_IPC_ERRNO_EEXIST) {
			semid = omr_semgetWrapper(portLibrary, fkey, setSize + 1, semflags_open);		
		} 
		if (semid == -1) {
			Trc_PRT_shsem_omrshsem_stat_semget_Failed(lasterrno);
			Trc_PRT_shsem_omrshsem_getInfoStruct_Exit();
			return OMRSH_FAILED;
		}
		Trc_PRT_shsem_omrshsem_getInfoStruct_shared_sem_opened();
		iCreatedTheSemaphoreSet = 0;
	} else {
		Trc_PRT_shsem_omrshsem_getInfoStruct_shared_sem_created();
		iCreatedTheSemaphoreSet = 1;
	}
	*handle =  omrshsem_createSemHandle(portLibrary, semid, setSize, baseFile);
	if(*handle == NULL) {
		Trc_PRT_shsem_omrshsem_open_createSemHandle_Failed();
		return OMRPORT_ERROR_SHSEM_OPFAILED;
	}  
	
	/* set the timestamp */
	(*handle)->timestamp = omrfile_lastmod(portLibrary, baseFile);
		
	if (iCreatedTheSemaphoreSet==1) {
		Trc_PRT_shsem_omrshsem_getInfoStruct_Exit();
		return OMRPORT_INFO_SHSEM_CREATED;
	} else {
		Trc_PRT_shsem_omrshsem_getInfoStruct_Exit();
		return OMRPORT_INFO_SHSEM_OPENED;
	}

}

static omrshsem_handle* 
omrshsem_createSemHandle(struct OMRPortLibrary *portLibrary, int semid, int nsems, const char* baseFile) 
{
	omrshsem_handle* result;
	intptr_t baseFileLength = strlen(baseFile)+1;

	Trc_PRT_shsem_omrshsem_createSemHandle_Entry(baseFile, semid, nsems);
		
	result = omrmem_allocate_memory(portLibrary, sizeof(omrshsem_handle), OMR_GET_CALLSITE(),
					OMRMEM_CATEGORY_PORT_LIBRARY);
	if(result == NULL) {
		Trc_PRT_shsem_omrshsem_createSemHandle_ExitMallocError();
		return NULL; /* malloc error! */
	}
		
	result->semid = semid;
	result->nsems = nsems;

	result->baseFile = omrmem_allocate_memory(portLibrary, baseFileLength, OMR_GET_CALLSITE(),
						  OMRMEM_CATEGORY_PORT_LIBRARY);
	if(NULL == result->baseFile) {
		omrmem_free_memory(portLibrary, result);
		Trc_PRT_shsem_omrshsem_createSemHandle_ExitMallocError();
		return NULL;
	}

	omrstr_printf(portLibrary, result->baseFile, baseFileLength, "%s", baseFile );

	Trc_PRT_shsem_omrshsem_createSemHandle_Exit(result);
	return result;
}



static intptr_t 
omrshsem_checkControlFileDate(OMRPortLibrary *portLibrary,
		struct omrshsem_handle *handle, const char *controlFile)
{
	struct stat filestat;
	struct semid_ds semstat;
	semctlUnion semctlarg;
	
	semctlarg.buf = &semstat;
	
	if (stat(controlFile, &filestat) != -1) {
		if (omr_semctlWrapper(portLibrary, TRUE, handle->semid, 0, IPC_STAT, semctlarg) != -1) {
			if (semstat.sem_ctime < filestat.st_ctime) {
				Trc_PRT_shsem_omrshsem_checkControlFileDate_isstale(handle->semid,
						controlFile);
				/*
				 * CMVC 155652: caller should deal with stale semaphores.
				 */
				return OMRPORT_INFO_SHSEM_OPENED_STALE;
			} else {
				return OMRPORT_INFO_SHSEM_OPENED;
			}
		} else {
			Trc_PRT_shsem_omrshsem_checkControlFileDate_semctlfailed(handle->semid);
			return OMRPORT_ERROR_SHSEM_OPFAILED;
		}
	} else {
		Trc_PRT_shsem_omrshsem_checkControlFileDate_statfailed(controlFile);
		return OMRPORT_ERROR_SHSEM_OPFAILED;
	}
}
