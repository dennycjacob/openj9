/*******************************************************************************
 * Copyright IBM Corp. and others 1998
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
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 *******************************************************************************/

#include "j9.h"
#include "j9consts.h"
#include "jclprots.h"
#include "j9protos.h"
#include "j9jclnls.h"
#include "j9vmnls.h"

jclass 
defineClassCommon(JNIEnv *env, jobject classLoaderObject,
	jstring className, jbyteArray classRep, jint offset, jint length, jobject protectionDomain, UDATA *options, J9Class *hostClass, J9ClassPatchMap *patchMap, BOOLEAN validateName)
{
#ifdef J9VM_OPT_DYNAMIC_LOAD_SUPPORT

/* Try a couple of GC passes (1 doesn't sem to be enough), but don't try forever */
#define MAX_RETRY_COUNT 2 

	J9VMThread *currentThread = (J9VMThread *)env;
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;
	J9TranslationBufferSet *dynFuncs = NULL;
	J9ClassLoader *classLoader = NULL;
	UDATA retried = FALSE;
	UDATA utf8Length = 0;
	char utf8NameStackBuffer[J9VM_PACKAGE_NAME_BUFFER_LENGTH];
	U_8 *utf8Name = NULL;
	U_8 *classBytes = NULL;
	J9Class *clazz = NULL;
	jclass result = NULL;
	PORT_ACCESS_FROM_JAVAVM(vm);
	UDATA isContiguousClassBytes = 0;
	J9ROMClass *loadedClass = NULL;
	U_8 *tempClassBytes = NULL;
	I_32 tempLength = 0;
	J9TranslationLocalBuffer localBuffer = {J9_CP_INDEX_NONE, LOAD_LOCATION_UNKNOWN, NULL};

	if (vm->dynamicLoadBuffers == NULL) {
		throwNewInternalError(env, "Dynamic loader is unavailable");
		goto done;
	}
	dynFuncs = vm->dynamicLoadBuffers;

	if (classRep == NULL) {
		throwNewNullPointerException(env, NULL);
		goto done;
	}

	if ((patchMap != NULL) && (patchMap->size != 0)) {
		localBuffer.patchMap = patchMap;
	}

	vmFuncs->internalEnterVMFromJNI(currentThread);
	isContiguousClassBytes = J9ISCONTIGUOUSARRAY(currentThread, *(J9IndexableObject **)classRep);
	if (!isContiguousClassBytes) {
		vmFuncs->internalExitVMToJNI(currentThread);
		/* Make a "flat" copy of classRep */
		if (length < 0) {
			throwNewIndexOutOfBoundsException(env, NULL);
			goto done;
		}
		classBytes = j9mem_allocate_memory(length, J9MEM_CATEGORY_CLASSES);
		if (classBytes == NULL) {
			vmFuncs->throwNativeOOMError(env, 0, 0);
			goto done;
		}
		(*env)->GetByteArrayRegion(env, classRep, offset, length, (jbyte *)classBytes);
		if ((*env)->ExceptionCheck(env)) {
			j9mem_free_memory(classBytes);
			goto done;
		}
		vmFuncs->internalEnterVMFromJNI(currentThread);
	}

	/* Allocate and initialize a UTF8 copy of the Unicode class-name */
	if (NULL != className) {
		j9object_t classNameObject = J9_JNI_UNWRAP_REFERENCE(className);
		UDATA stringFlags = J9_STR_NULL_TERMINATE_RESULT;

		if (!validateName) {
			stringFlags |= J9_STR_XLAT;
		}

		/* Perform maximum length check to avoid copy in extreme cases. */
		if (J9VMJAVALANGSTRING_LENGTH(currentThread, classNameObject) > J9VM_MAX_CLASS_NAME_LENGTH) {
			vmFuncs->setCurrentExceptionNLS(currentThread,
				J9VMCONSTANTPOOL_JAVALANGCLASSNOTFOUNDEXCEPTION,
				J9NLS_VM_CLASS_NAME_EXCEEDS_MAX_LENGTH);
			goto done;
		}

		utf8Name = (U_8*)vmFuncs->copyStringToUTF8WithMemAlloc(currentThread, classNameObject, stringFlags, "", 0, utf8NameStackBuffer, J9VM_PACKAGE_NAME_BUFFER_LENGTH, &utf8Length);

		if (NULL == utf8Name) {
			vmFuncs->setNativeOutOfMemoryError(currentThread, 0, 0);
			goto done;
		}

		if (utf8Length > J9VM_MAX_CLASS_NAME_LENGTH) {
			vmFuncs->setCurrentExceptionNLS(currentThread,
				J9VMCONSTANTPOOL_JAVALANGCLASSNOTFOUNDEXCEPTION,
				J9NLS_VM_CLASS_NAME_EXCEEDS_MAX_LENGTH);
			goto done;
		}

		if (validateName && (CLASSNAME_INVALID == vmFuncs->verifyQualifiedName(currentThread, utf8Name, utf8Length, CLASSNAME_VALID_NON_ARRARY))) {
			/* We don't yet know if the class being defined is exempt. Setting this option tells
			 * defineClassCommon() to fail if it discovers that the class is not exempt. That failure
			 * is distinguished by returning NULL with no exception pending.
			 */
			*options |= J9_FINDCLASS_FLAG_NAME_IS_INVALID;
		}
	}

	if (isContiguousClassBytes) {
		/* For ARRAYLETS case, we get free range checking from GetByteArrayRegion JNI call */
		if ((offset < 0) || (length < 0)
			|| (((U_32)offset + (U_32)length) > J9INDEXABLEOBJECT_SIZE(currentThread, *(J9IndexableObject **)classRep))
		) {
			vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGINDEXOUTOFBOUNDSEXCEPTION, NULL);
			goto done;
		}
	}

	classLoader = J9VMJAVALANGCLASSLOADER_VMREF(currentThread, J9_JNI_UNWRAP_REFERENCE(classLoaderObject));

	if (NULL == classLoader) {
		classLoader = vmFuncs->internalAllocateClassLoader(vm, J9_JNI_UNWRAP_REFERENCE(classLoaderObject));
		if (NULL == classLoader) {
			goto done;
		}
	}

retry:

	omrthread_monitor_enter(vm->classTableMutex);
	/* Hidden class is never added into the hash table */
	if ((NULL != utf8Name) && J9_ARE_NO_BITS_SET(*options, J9_FINDCLASS_FLAG_HIDDEN)) {
		if (NULL != vmFuncs->hashClassTableAt(classLoader, utf8Name, utf8Length)) {
			/* Bad, we have already defined this class - fail */
			omrthread_monitor_exit(vm->classTableMutex);
			if (J9_ARE_NO_BITS_SET(*options, J9_FINDCLASS_FLAG_NAME_IS_INVALID)) {
				/* TODO: This path is taken if Classloader.findClass is called on a persisted class.
				 * Once class objects are persisted, reaching this point is an error.
				 */
#if defined(J9VM_OPT_SNAPSHOTS)
				if (IS_RESTORE_RUN(vm)) {
					clazz = vmFuncs->hashClassTableAt(classLoader, utf8Name, utf8Length);

					if (!vmFuncs->loadWarmClassFromSnapshot(currentThread, classLoader, clazz)) {
						clazz = NULL;
						goto done;
					}

					if (NULL != protectionDomain) {
						J9VMJAVALANGCLASS_SET_PROTECTIONDOMAIN(
							currentThread,
							clazz->classObject,
							J9_JNI_UNWRAP_REFERENCE(protectionDomain));
					}
				} else
#endif /* defined(J9VM_OPT_SNAPSHOTS) */
				{
					vmFuncs->setCurrentExceptionNLSWithArgs(
						currentThread,
						J9NLS_JCL_DUPLICATE_CLASS_DEFINITION,
						J9VMCONSTANTPOOL_JAVALANGLINKAGEERROR,
						utf8Length,
						utf8Name);
				}
			}
			goto done;
		}
	}

	if (isContiguousClassBytes) {
		/* Always re-load the classBytes pointer, as GC required in OutOfMemory case may have moved things */
		/* TMP_J9JAVACONTIGUOUSARRAYOFBYTE_EA returns I_8 * (since Java bytes are I_8) so this cast will silence the warning */
		classBytes = (U_8 *) TMP_J9JAVACONTIGUOUSARRAYOFBYTE_EA(currentThread, *(J9IndexableObject **)classRep, offset);
	}

	tempClassBytes = classBytes;
	tempLength = length;

	/* Try to find classLocation. Ignore return code because there are valid cases where it might not find it (ie. bytecode spinning).
	 * If the class is not found the default class location is fine.
	 */
	dynFuncs->findLocallyDefinedClassFunction(currentThread, NULL, utf8Name, (U_32) utf8Length, classLoader, (UDATA) FALSE, &localBuffer);

	/* skip if we are anonClass or hidden classes */
	if (J9_ARE_NO_BITS_SET(*options, J9_FINDCLASS_FLAG_ANON | J9_FINDCLASS_FLAG_HIDDEN)) {
		/* Check for romClass cookie, it indicates that we are  defining a class out of a JXE not from class bytes */

		loadedClass = vmFuncs->romClassLoadFromCookie(currentThread, utf8Name, utf8Length, classBytes, (UDATA) length);

		if (NULL != loadedClass) {
			/* An existing ROMClass is found in the shared class cache.
			 * If -Xshareclasses:enableBCI is present, need to give VM a chance to trigger ClassFileLoadHook event.
			 */
			if ((NULL == vm->sharedClassConfig) || (0 == vm->sharedClassConfig->isBCIEnabled(vm))) {
				clazz = vmFuncs->internalCreateRAMClassFromROMClass(currentThread,
						classLoader,
						loadedClass,
						0,
						NULL,
						protectionDomain ? *(j9object_t*)protectionDomain : NULL,
						NULL,
						J9_CP_INDEX_NONE,
						localBuffer.loadLocationType,
						NULL,
						hostClass);
				/* Done if a class was found or and exception is pending, otherwise try to define the bytes */
				if ((NULL != clazz) || (NULL != currentThread->currentException)) {
					goto done;
				}
				loadedClass = NULL;
			} else {
				tempClassBytes = J9ROMCLASS_INTERMEDIATECLASSDATA(loadedClass);
				tempLength = loadedClass->intermediateClassDataLength;
				*options |= J9_FINDCLASS_FLAG_SHRC_ROMCLASS_EXISTS;
			}
		}
	}

	/* The defineClass helper requires you hold the class table mutex and releases it for you */
	
	clazz = dynFuncs->internalDefineClassFunction(currentThread, 
				utf8Name, utf8Length,
				tempClassBytes, (UDATA) tempLength, NULL,
				classLoader,
				protectionDomain ? *(j9object_t*)protectionDomain : NULL,
				*options | J9_FINDCLASS_FLAG_THROW_ON_FAIL | J9_FINDCLASS_FLAG_NO_CHECK_FOR_EXISTING_CLASS,
				loadedClass,
				hostClass,
				&localBuffer);

	/* If OutOfMemory, try a GC to free up some memory */
	
	if (currentThread->privateFlags & J9_PRIVATE_FLAGS_CLOAD_NO_MEM) {
		if (!retried) {
			/*Trc_VM_internalFindClass_gcAndRetry(vmThread);*/
			currentThread->javaVM->memoryManagerFunctions->j9gc_modron_global_collect_with_overrides(currentThread, J9MMCONSTANT_EXPLICIT_GC_NATIVE_OUT_OF_MEMORY);
			retried = TRUE;
			goto retry;
		}
		vmFuncs->setNativeOutOfMemoryError(currentThread, 0, 0);
	}

done:
	if (NULL == clazz) {
		if (J9_ARE_ANY_BITS_SET(*options, J9_FINDCLASS_FLAG_NAME_IS_INVALID)) {
			/*
			 * The caller signalled that the name is invalid. Leave the result NULL and
			 * clear any pending exception; the caller will throw NoClassDefFoundError.
			 */
			currentThread->currentException = NULL;
 			currentThread->privateFlags &= ~(UDATA)J9_PRIVATE_FLAGS_REPORT_EXCEPTION_THROW;
		} else if (NULL == currentThread->currentException) {
			/* should not get here -- throw the default exception just in case */
			vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGCLASSFORMATERROR, NULL);
		}
	} else {
		result = vmFuncs->j9jni_createLocalRef(env, J9VM_J9CLASS_TO_HEAPCLASS(clazz));
	}

	vmFuncs->internalExitVMToJNI(currentThread);

	if ((U_8*)utf8NameStackBuffer != utf8Name) {
		j9mem_free_memory(utf8Name);
	}

	if (!isContiguousClassBytes) {
		j9mem_free_memory(classBytes);
	}

	return result;

#else /* J9VM_OPT_DYNAMIC_LOAD_SUPPORT */
	throwNewInternalError(env, "Dynamic loading not supported");
	return NULL;
#endif /* J9VM_OPT_DYNAMIC_LOAD_SUPPORT */
}
