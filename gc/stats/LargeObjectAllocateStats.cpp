/*******************************************************************************
 * Copyright IBM Corp. and others 2012
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
 
#include "LargeObjectAllocateStats.hpp"

#include <math.h>
#include <stdlib.h>

#include "omrport.h"

#include "EnvironmentBase.hpp"
#include "Forge.hpp"
#include "GCExtensionsBase.hpp"
#include "ModronAssertions.h"
#include "AtomicOperations.hpp"


MM_LargeObjectAllocateStats *
MM_LargeObjectAllocateStats::newInstance(MM_EnvironmentBase *env, uint16_t maxAllocateSizes, uintptr_t largeObjectThreshold, uintptr_t veryLargeObjectThreshold, float sizeClassRatio, uintptr_t maxHeapSize, uintptr_t tlhMaximumSize, uintptr_t tlhMinimumSize,  uintptr_t factorVeryLargeEntryPool)
{
	MM_LargeObjectAllocateStats *largeObjectAllocateStats;

	largeObjectAllocateStats = (MM_LargeObjectAllocateStats *)env->getForge()->allocate(sizeof(MM_LargeObjectAllocateStats), OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());

	if(NULL != largeObjectAllocateStats) {
		new(largeObjectAllocateStats) MM_LargeObjectAllocateStats(env);

		if(!largeObjectAllocateStats->initialize(env, maxAllocateSizes, largeObjectThreshold, veryLargeObjectThreshold, sizeClassRatio, maxHeapSize, tlhMaximumSize, tlhMinimumSize, factorVeryLargeEntryPool)) {
			largeObjectAllocateStats->kill(env);
			return NULL;
		}
	}

	return largeObjectAllocateStats;
}

void
MM_LargeObjectAllocateStats::initializeFreeMemoryProfileMaxSizeClasses(MM_EnvironmentBase *env, uintptr_t veryLargeObjectThreshold, float sizeClassRatio, uintptr_t maxHeapSize)
{
	MM_GCExtensionsBase *extensions = env->getExtensions();
	float sizeClassRatioLog = logf(sizeClassRatio);

	/* ideally, freeMemoryProfileMaxSizeClasses should be initialized only once, way earlier;
	 * but at the point where most of extension fields are initialized, maxHeapSize is not known yet
	 */
	if (0 == extensions->freeMemoryProfileMaxSizeClasses) {
		uintptr_t largestClassSizeIndex = (uintptr_t)(logf((float)maxHeapSize)/sizeClassRatioLog);

		/* initialize largeObjectAllocationProfilingVeryLargeObjectThreshold and largeObjectAllocationProfilingVeryLargeObjectSizeClass */
		uintptr_t veryLargeEntrySizeClass;
		if (extensions->memoryMax > veryLargeObjectThreshold) {
			veryLargeEntrySizeClass = (uintptr_t)(logf((float)veryLargeObjectThreshold)/sizeClassRatioLog);
			extensions->largeObjectAllocationProfilingVeryLargeObjectThreshold = (uintptr_t)powf(sizeClassRatio, (float)veryLargeEntrySizeClass);
		} else {
			veryLargeEntrySizeClass = largestClassSizeIndex + 1;
			extensions->largeObjectAllocationProfilingVeryLargeObjectThreshold = UDATA_MAX;
		}
		extensions->largeObjectAllocationProfilingVeryLargeObjectSizeClass = veryLargeEntrySizeClass;

		/* to prevent thread race condition -- before largeObjectAllocationProfilingVeryLargeObjectThreshold has been initialized, another thread might use the value, multi-entries is not issue. */
		MM_AtomicOperations::writeBarrier();
		extensions->freeMemoryProfileMaxSizeClasses = largestClassSizeIndex + 1;
	}
}

bool
MM_LargeObjectAllocateStats::initialize(MM_EnvironmentBase *env, uint16_t maxAllocateSizes, uintptr_t largeObjectThreshold, uintptr_t veryLargeObjectThreshold, float sizeClassRatio, uintptr_t maxHeapSize, uintptr_t tlhMaximumSize, uintptr_t tlhMinimumSize, uintptr_t factorVeryLargeEntryPool)
{
	OMRPortLibrary *portLibrary = env->getPortLibrary();
#if defined(OMR_GC_THREAD_LOCAL_HEAP)
	_tlhMaximumSize = tlhMaximumSize;
	_tlhMinimumSize = tlhMinimumSize;
#endif	
	_maxAllocateSizes = maxAllocateSizes;
	_largeObjectThreshold = largeObjectThreshold;
	_sizeClassRatio = sizeClassRatio;
	_sizeClassRatioLog = logf(_sizeClassRatio);
	_maxHeapSize = maxHeapSize;

	/* To accurately maintain for stats for top _maxAllocateSizes different sizes,
	 * we'll actually maintain stats for 2x more, and discard info for lower 1/2 */
	if (NULL == (_spaceSavingSizes = spaceSavingNew(portLibrary, _maxAllocateSizes * 2))) {
		return false;
	}

	if (NULL == (_spaceSavingSizeClasses = spaceSavingNew(portLibrary, _maxAllocateSizes * 2))) {
		return false;
	}

	if (NULL == (_spaceSavingSizesAveragePercent = spaceSavingNew(portLibrary, _maxAllocateSizes * 2))) {
		return false;
	}

	if (NULL == (_spaceSavingSizeClassesAveragePercent = spaceSavingNew(portLibrary, _maxAllocateSizes * 2))) {
		return false;
	}

	if (NULL == (_spaceSavingTemp = spaceSavingNew(portLibrary, _maxAllocateSizes * 2))) {
		return false;
	}

	MM_LargeObjectAllocateStats::initializeFreeMemoryProfileMaxSizeClasses(env, veryLargeObjectThreshold, sizeClassRatio, maxHeapSize);

	if (!_freeEntrySizeClassStats.initialize(env, _maxAllocateSizes,  env->getExtensions()->freeMemoryProfileMaxSizeClasses, env->getExtensions()->largeObjectAllocationProfilingVeryLargeObjectThreshold, factorVeryLargeEntryPool)) {
		return false;
	}
	_veryLargeEntrySizeClass = env->getExtensions()->largeObjectAllocationProfilingVeryLargeObjectSizeClass; 

#if defined(OMR_GC_THREAD_LOCAL_HEAP)
	uintptr_t largestTLHClassSizeIndex = (uintptr_t)(logf((float)tlhMaximumSize)/_sizeClassRatioLog);
	uintptr_t maxTLHSizeClasses = largestTLHClassSizeIndex + 1;

	if (!_tlhAllocSizeClassStats.initialize(env, 0,  maxTLHSizeClasses, UDATA_MAX)) {
		return false;
	}
#endif

	_sizeClassSizes = (uintptr_t *)env->getForge()->allocate(sizeof(uintptr_t) * _freeEntrySizeClassStats._maxSizeClasses, OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());

	if (NULL == _sizeClassSizes) {
		return false;
	}

	for (uintptr_t sizeClassIndex = 0; sizeClassIndex < _freeEntrySizeClassStats._maxSizeClasses; sizeClassIndex++) {
		/* TODO: this is rather an approximation (at least due to insufficient precision of double math) */
		_sizeClassSizes[sizeClassIndex] = (uintptr_t)powf(_sizeClassRatio, (float)sizeClassIndex);
	}

	return true;
}

void
MM_LargeObjectAllocateStats::tearDown(MM_EnvironmentBase *env)
{
	if (NULL != _spaceSavingTemp){
		spaceSavingFree(_spaceSavingTemp);
		_spaceSavingTemp = NULL;
	}

	if (NULL != _spaceSavingSizesAveragePercent){
		spaceSavingFree(_spaceSavingSizesAveragePercent);
		_spaceSavingSizesAveragePercent = NULL;
	}

	if (NULL != _spaceSavingSizeClassesAveragePercent){
		spaceSavingFree(_spaceSavingSizeClassesAveragePercent);
		_spaceSavingSizeClassesAveragePercent = NULL;
	}

	if (NULL != _spaceSavingSizes){
		spaceSavingFree(_spaceSavingSizes);
		_spaceSavingSizes = NULL;
	}

	if (NULL != _spaceSavingSizeClasses){
		spaceSavingFree(_spaceSavingSizeClasses);
		_spaceSavingSizeClasses = NULL;
	}

	_freeEntrySizeClassStats.tearDown(env);
#if defined(OMR_GC_THREAD_LOCAL_HEAP)
	_tlhAllocSizeClassStats.tearDown(env);
#endif

	if (NULL != _sizeClassSizes) {
		env->getForge()->free(_sizeClassSizes);
		_sizeClassSizes = NULL;
	}
}



void
MM_LargeObjectAllocateStats::kill(MM_EnvironmentBase *env)
{
	tearDown(env);
	env->getForge()->free(this);
}

void
MM_LargeObjectAllocateStats::resetCurrent()
{
	spaceSavingClear(_spaceSavingSizes);
	spaceSavingClear(_spaceSavingSizeClasses);
}

void
MM_LargeObjectAllocateStats::resetAverage()
{
	spaceSavingClear(_spaceSavingSizesAveragePercent);
	spaceSavingClear(_spaceSavingSizeClassesAveragePercent);
}

void
MM_LargeObjectAllocateStats::allocateObject(uintptr_t allocateSize)
{
	/* update stats only for large enough objects */
	if (allocateSize >= _largeObjectThreshold) {

		/* update stats for the exact size;
		 * we want to put more weight on larger objects so we do not increment stats by 1,
		 * but by the allocation size itself
		 */
		spaceSavingUpdate(_spaceSavingSizes, (void *)allocateSize, allocateSize);

		/* find in which size class object belongs to and update the stats for the size class itself. */
		uintptr_t sizeClass = (uintptr_t)(powf(_sizeClassRatio, (float)ceil(logf((float)allocateSize) / _sizeClassRatioLog)));
		spaceSavingUpdate(_spaceSavingSizeClasses, (void *)sizeClass, sizeClass);
	}
}

void
MM_LargeObjectAllocateStats::mergeCurrent(MM_LargeObjectAllocateStats *statsToMerge)
{
	/* TODO: make sure we do not call merge more than necessary
	 * (in callers, separate merging from getting merged data (once merged, could be invoked several times) */
	uintptr_t i = 0;

	/* merge exact sizes - current  */
	OMRSpaceSaving* spaceSavingToMerge = statsToMerge->_spaceSavingSizes;

	for(i = 0; i < spaceSavingGetCurSize(spaceSavingToMerge); i++ ){
		spaceSavingUpdate(_spaceSavingSizes, spaceSavingGetKthMostFreq(spaceSavingToMerge, i + 1), spaceSavingGetKthMostFreqCount(spaceSavingToMerge, i + 1));
	}

	/* merge size classes - current */
	spaceSavingToMerge = statsToMerge->_spaceSavingSizeClasses;

	for(i = 0; i < spaceSavingGetCurSize(spaceSavingToMerge); i++ ){
		spaceSavingUpdate(_spaceSavingSizeClasses, spaceSavingGetKthMostFreq(spaceSavingToMerge, i + 1), spaceSavingGetKthMostFreqCount(spaceSavingToMerge, i + 1));
	}
}

void
MM_LargeObjectAllocateStats::mergeAverage(MM_LargeObjectAllocateStats *statsToMerge)
{
	uintptr_t i = 0;

	/* merge exact sizes - average */
	OMRSpaceSaving* spaceSavingToMerge = statsToMerge->_spaceSavingSizesAveragePercent;

	for(i = 0; i < spaceSavingGetCurSize(spaceSavingToMerge); i++ ){
		spaceSavingUpdate(_spaceSavingSizesAveragePercent, spaceSavingGetKthMostFreq(spaceSavingToMerge, i + 1), spaceSavingGetKthMostFreqCount(spaceSavingToMerge, i + 1));
	}

	/* merge size classes - average */
	spaceSavingToMerge = statsToMerge->_spaceSavingSizeClassesAveragePercent;

	for(i = 0; i < spaceSavingGetCurSize(spaceSavingToMerge); i++ ){
		spaceSavingUpdate(_spaceSavingSizeClassesAveragePercent, spaceSavingGetKthMostFreq(spaceSavingToMerge, i + 1), spaceSavingGetKthMostFreqCount(spaceSavingToMerge, i + 1));
	}
}

uintptr_t
MM_LargeObjectAllocateStats::upSampleAllocStats(MM_EnvironmentBase *env, uintptr_t allocSize, uintptr_t bytesAllocated)
{
	uintptr_t bytesAllocatedUpSampled = bytesAllocated;

#if defined(OMR_GC_THREAD_LOCAL_HEAP)
	if (allocSize < _tlhMaximumSize) {
		uintptr_t totalTlhBytesAllocated = 0;
		float thisSizeTlhBytesAllocated = 0;
		uintptr_t tlhMaximumSizeClassIndex = getSizeClassIndex(_tlhMaximumSize);
		uintptr_t tlhMinimumSizeClassIndex = getSizeClassIndex(_tlhMinimumSize);
		for (uintptr_t sizeClassIndex = tlhMinimumSizeClassIndex; sizeClassIndex <= tlhMaximumSizeClassIndex ; sizeClassIndex++) {
			uintptr_t freeEntrySize = getSizeClassSizes(sizeClassIndex);

			float probabilityAllocFits = 0;
			if (freeEntrySize >= allocSize) {
				probabilityAllocFits = ((float)freeEntrySize - (float)allocSize) / (float)freeEntrySize;
			}

			uintptr_t tlhAllocCount = getTlhAllocSizeClassCount(sizeClassIndex);
			uintptr_t tlhBytesAllocated = tlhAllocCount * freeEntrySize;
			totalTlhBytesAllocated += tlhBytesAllocated;
			thisSizeTlhBytesAllocated += probabilityAllocFits * tlhBytesAllocated;
		}
		Assert_MM_true(thisSizeTlhBytesAllocated <= (float)totalTlhBytesAllocated);
		float upSampleRatio = 1.0;
		if (0 != ((float)totalTlhBytesAllocated - thisSizeTlhBytesAllocated)) {
			upSampleRatio = ((float)totalTlhBytesAllocated / ((float)totalTlhBytesAllocated - thisSizeTlhBytesAllocated));
		}
		bytesAllocatedUpSampled = (uintptr_t)(bytesAllocatedUpSampled * upSampleRatio);
		Trc_MM_LargeObjectAllocateStats_upSample(env->getLanguageVMThread(), allocSize, bytesAllocated, (uintptr_t)thisSizeTlhBytesAllocated, totalTlhBytesAllocated, upSampleRatio, bytesAllocatedUpSampled);
	}
#endif

	return bytesAllocatedUpSampled;
}

uintptr_t
MM_LargeObjectAllocateStats::convertPercentFloatToUDATA(float percent)
{
	return (uintptr_t)(percent * (float)1000000.0);
}

float
MM_LargeObjectAllocateStats::convertPercentUDATAToFloat(uintptr_t percent)
{
	return (float)percent / (float)1000000.0;
}

uintptr_t
MM_LargeObjectAllocateStats::estimateFragmentation(MM_EnvironmentBase *env)
{
	_timeEstimateFragmentation = 0;
	_cpuTimeEstimateFragmentation = 0;
	_remainingFreeMemoryAfterEstimate = 0;
	_freeMemoryBeforeEstimate = 0;
	
	MM_GCExtensionsBase *ext = env->getExtensions();
	if (0 == spaceSavingGetCurSize(_spaceSavingSizesAveragePercent)) {
		/* no large allocation -> no (macro) fragmentation */
		return 0;
	}

	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
	uint64_t startTime = omrtime_hires_clock();
	int64_t startCPUTime = omrthread_get_self_cpu_time(env->getOmrVMThread()->_os_thread);
	/* calculate ratio of TLH allocated (as complement of large allocates) */
	float tlhPercent = 100.0;
	/* to speed up calculation, ignore objects with very low frequencies (threshold is set to 0.03% (not 3%)) */
	const float ignoreObjectPercentThreshold = (float)0.03;
	uintptr_t objectCount = 0;
	uintptr_t maxFrequentAllocateSizes = OMR_MIN(ext->freeEntrySizeClassStatsSimulated._maxFrequentAllocateSizes, spaceSavingGetCurSize(_spaceSavingSizesAveragePercent));

	for(uintptr_t i = 0; i < maxFrequentAllocateSizes; i++ ) {
		float percent = convertPercentUDATAToFloat(spaceSavingGetKthMostFreqCount(_spaceSavingSizesAveragePercent, i + 1));
		/* reset _fractionFrequentAllocation array */
		ext->freeEntrySizeClassStatsSimulated._fractionFrequentAllocation[i] = 0;
		if (percent >= ignoreObjectPercentThreshold) {
			tlhPercent -= percent;
			objectCount += 1;
		}
	}

	if (100.0 == tlhPercent) {
		/* large allocations are negligible */
		return 0;
	}

	/* TODO: sum of large allocate percentages should be <= 100 */
	if (tlhPercent < 0.0) {
		tlhPercent = 0.0;
	}

	/* take a snapshot of the free memory */
	uintptr_t initialFreeMemory = _freeEntrySizeClassStats.copyTo(&ext->freeEntrySizeClassStatsSimulated, _sizeClassSizes);
	_freeMemoryBeforeEstimate = initialFreeMemory;
	uintptr_t currentFreeMemory = initialFreeMemory;
	uintptr_t prevFreeMemory = UDATA_MAX;
	uintptr_t unsatisfiedAlloc = 0;

	Trc_MM_LargeObjectAllocateStats_estimateFragmentation_entry(env->getLanguageVMThread(), initialFreeMemory, initialFreeMemory >> 20, tlhPercent);

	const uintptr_t maxObjectStrides = 10;
	const uintptr_t maxTLHStrides = 100;
	float allocTLHStridesPerObject = (((float)maxTLHStrides) / maxObjectStrides) / objectCount;
	Assert_MM_true(allocTLHStridesPerObject >= 1.0);
//#if 1
//	allocTLHStridesPerObject =1.0;
//#endif	
	float allocTLHStridesFractional = 0.0;
	uintptr_t stride = 0;

	_TLHSizeClassIndex = 0;
	_TLHFrequentAllocationSize = 0;
	/* simulate the allocation in approximately maxObjectStrides strides.
	 * could be less, if fragmented. could be more if not fragmented (due to probablistic size decay of very large free entries).
	 * stop up until we cannot satisfy allocate or remaining free memory is less than 1% of initial free memory.
	 * if we do not make progress (strides are too small), we stop.
	 */
	for (; (0 == unsatisfiedAlloc) && (currentFreeMemory > initialFreeMemory / 100) && (prevFreeMemory > currentFreeMemory) ; stride++) {
		/* save currentFreeMemory for checking if make progress in one stride */
		prevFreeMemory = currentFreeMemory;
		/* walk over frequent allocates, which will drive the simulation. tlh allocate will interleave with them */
		for(uintptr_t i = 0; (i < maxFrequentAllocateSizes) && (0 == unsatisfiedAlloc); i++ ) {
			float objectPercent = convertPercentUDATAToFloat(spaceSavingGetKthMostFreqCount(_spaceSavingSizesAveragePercent, i + 1));

			if (objectPercent >= ignoreObjectPercentThreshold) {
				uintptr_t objectSize = (uintptr_t)spaceSavingGetKthMostFreq(_spaceSavingSizesAveragePercent, i + 1);

				uintptr_t objectAllocBytes = (uintptr_t)((objectPercent * initialFreeMemory) / 100);
				/* example: for a large object with 30% of all allocates and with 60% of total TLH allocates (10% of other large object allocations)
				 * in this stride we will simulate allocation of 30 / 10 = 3% for the large object and  30/(30+10) x 60 / 10 = 4.5% (tlhAllocBytesPerObjectPerObjectStride) for TLHs.
				 * TLHs allocations will be further divided into several strides (allocTLHStridesInteger), where each TLH allocation
				 * stride will allocate from different size class.
				 */
				uintptr_t tlhAllocBytesPerObject = (uintptr_t)((objectPercent / (100.0 - tlhPercent) * tlhPercent * initialFreeMemory) / 100);

				uintptr_t objectAllocBytesPerObjectStride = objectAllocBytes / maxObjectStrides;
				allocTLHStridesFractional += allocTLHStridesPerObject;
				uintptr_t allocTLHStridesInteger = (uintptr_t) allocTLHStridesFractional;
				allocTLHStridesFractional -= (float) allocTLHStridesInteger;
				uintptr_t tlhAllocBytesPerObjectPerObjectStride = tlhAllocBytesPerObject / maxObjectStrides;

				Trc_MM_LargeObjectAllocateStats_estimateFragmentation_stride(env->getLanguageVMThread(), stride, currentFreeMemory, currentFreeMemory >> 20, objectSize, objectAllocBytesPerObjectStride, objectAllocBytesPerObjectStride >> 20, tlhAllocBytesPerObjectPerObjectStride, tlhAllocBytesPerObjectPerObjectStride >> 20);

				if (0 != tlhAllocBytesPerObjectPerObjectStride) {
					unsatisfiedAlloc = simulateAllocateTLHs(env, tlhAllocBytesPerObjectPerObjectStride, &currentFreeMemory, allocTLHStridesInteger);
					if (0 != unsatisfiedAlloc) {
						break;
					}
				}
				
				/* only apply allocInteger(whole number of frequentAllocation) in simulateAllocateObjects, cumulate fraction part of frequentAllocation for later strides, when the cumulated fraction is bigger than 1, add the "integer" to allocInteger */
				float allocFractional = objectAllocBytesPerObjectStride / (float) objectSize;
				uintptr_t allocInteger = (uintptr_t)allocFractional;

				ext->freeEntrySizeClassStatsSimulated._fractionFrequentAllocation[i] += (allocFractional-(float)allocInteger);
				if (1.0 <= ext->freeEntrySizeClassStatsSimulated._fractionFrequentAllocation[i]) {
					uintptr_t fractionInteger = (uintptr_t) ext->freeEntrySizeClassStatsSimulated._fractionFrequentAllocation[i];
					allocInteger += fractionInteger;
					ext->freeEntrySizeClassStatsSimulated._fractionFrequentAllocation[i] -= (float) fractionInteger;
				}
				objectAllocBytesPerObjectStride = allocInteger * objectSize;

				if (objectAllocBytesPerObjectStride >= objectSize) {
					/* TODO: for very large infrequent objects (allocBytes lower (or larger but close to) size), we should also allocate with probability size/allocBytes */
					unsatisfiedAlloc = simulateAllocateObjects(env, objectAllocBytesPerObjectStride, objectSize, &currentFreeMemory);
				}

				/* Ensure we do not wrap around */
				Assert_MM_true(currentFreeMemory <= initialFreeMemory);
			}
		}
	}

	uintptr_t remainingFreeMemory = ext->freeEntrySizeClassStatsSimulated.getFreeMemory(_sizeClassSizes);

	Assert_MM_true(remainingFreeMemory == currentFreeMemory);

	Trc_MM_LargeObjectAllocateStats_estimateFragmentation_exit(env->getLanguageVMThread(), remainingFreeMemory, remainingFreeMemory >> 20, unsatisfiedAlloc, unsatisfiedAlloc >> 20);

	_timeEstimateFragmentation = omrtime_hires_clock() - startTime;
	_cpuTimeEstimateFragmentation = omrthread_get_self_cpu_time(env->getOmrVMThread()->_os_thread) - startCPUTime;
	/* convert cputime from NanoSeconds to MicroSeconds */
	_cpuTimeEstimateFragmentation /= 1000;

	_remainingFreeMemoryAfterEstimate = remainingFreeMemory;
	return _remainingFreeMemoryAfterEstimate;
}

uintptr_t
MM_LargeObjectAllocateStats::simulateAllocateObjects(MM_EnvironmentBase *env, uintptr_t allocBytes, uintptr_t objectSize, uintptr_t *currentFreeMemory)
{
	MM_GCExtensionsBase *ext = env->getExtensions();
	Assert_MM_true(NULL != ext->freeEntrySizeClassStatsSimulated._frequentAllocationHead);
	uintptr_t sizeClassIndex = getSizeClassIndex(objectSize);
	uintptr_t allocObjectCount = allocBytes / objectSize;
	Assert_MM_true(allocObjectCount > 0);

	Trc_MM_LargeObjectAllocateStats_simulateAllocateObjects_entry(env->getLanguageVMThread(), objectSize, allocBytes, allocBytes >> 20, *currentFreeMemory, *currentFreeMemory >> 20);

	/* keep allocating from this size class or any larger */
	while ((allocBytes > 0) && (sizeClassIndex < ext->freeEntrySizeClassStatsSimulated._maxSizeClasses)) {

		/* any available free entries of this size class? */
		/* TODO: find a faster way to find next non zero size class index */
		if ((0 != ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex]) && (objectSize <= _sizeClassSizes[sizeClassIndex])) {
			/* dealing with 'regular' size */

			Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_freeEntry_entry(env->getLanguageVMThread(), "Object", "regular", sizeClassIndex, _sizeClassSizes[sizeClassIndex] , ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex]);

			/* go for smallest to largest sizes within a class size; thus start with the 'regular' size and continue with frequent allocate sizes as ordered in the list (ascending) */

			/* find out which fraction of the memory is available (due to rounding) */
			uintptr_t freeEntryOverAllocSizeRatio = _sizeClassSizes[sizeClassIndex] / objectSize;
			uintptr_t usedPortionOfFreeEntry = freeEntryOverAllocSizeRatio * objectSize;
			uintptr_t availableBytes = ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex] * usedPortionOfFreeEntry;
			Trc_MM_LargeObjectAllocateStats_simulateAllocateObjects_availableBytes(env->getLanguageVMThread(), "regular", availableBytes, ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex], usedPortionOfFreeEntry, freeEntryOverAllocSizeRatio, objectSize);

			float freeEntriesUsed = 0.0;

			if (availableBytes <= allocBytes) {
				Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_satisfy(env->getLanguageVMThread(), "partial", "regular", _sizeClassSizes[sizeClassIndex], sizeClassIndex);
				Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_partial(env->getLanguageVMThread(), allocBytes - availableBytes, allocBytes, availableBytes);

				/* all available memory of this size will be used (but we my not satisfy all allocation) */
				freeEntriesUsed = (float)ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex];
				*currentFreeMemory -= ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex] * _sizeClassSizes[sizeClassIndex];
				ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex] = 0;
				allocBytes -= availableBytes;
				allocObjectCount = allocBytes / objectSize;
			} else {
				/* all allocation is satisfied (and there may be entries of this size left) */
				freeEntriesUsed = (float)allocObjectCount / (float)freeEntryOverAllocSizeRatio;
				Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_satisfy(env->getLanguageVMThread(), "full", "regular", _sizeClassSizes[sizeClassIndex], sizeClassIndex);
				Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_full(env->getLanguageVMThread(), ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex] - (uintptr_t)freeEntriesUsed, ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex], freeEntriesUsed);
				ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex] -= (uintptr_t)freeEntriesUsed;
				*currentFreeMemory -= (uintptr_t)freeEntriesUsed * _sizeClassSizes[sizeClassIndex];
				allocBytes = 0;
				allocObjectCount = 0;
			}

			/* there are two kind of remainders:
			 1) freeEntriesUsed of size: freeEntrySize - usedPortionOfFreeEntry
			 2) up to one of size: freeEntrySize - decimal-fraction-of-freeEntriesUsed-float * usedPortionOfFreeEntry
			*/

			/* we do not know the exact size of free entry(ies). we do know low and hi bounds, and will just approximate it (them) with a random size within the range */
			uintptr_t minFreeEntrySize = _sizeClassSizes[sizeClassIndex];
			uintptr_t maxFreeEntrySize = UDATA_MAX;

			if (NULL != ext->freeEntrySizeClassStatsSimulated._frequentAllocationHead[sizeClassIndex]) {
				/* since we deal with regular size, first larger size might be the first frequent alloc size, if any */
				maxFreeEntrySize = ext->freeEntrySizeClassStatsSimulated._frequentAllocationHead[sizeClassIndex]->_size;
			} else if (sizeClassIndex + 1 < ext->freeEntrySizeClassStatsSimulated._maxSizeClasses) {
				maxFreeEntrySize = _sizeClassSizes[sizeClassIndex + 1];
			}

			uintptr_t guessedFreeEntrySize = minFreeEntrySize + (uintptr_t) ((maxFreeEntrySize - minFreeEntrySize) * ((float)rand() / (float)RAND_MAX));

			uintptr_t freeEntriesUsedInteger = (uintptr_t)freeEntriesUsed;
			float freeEntriesUsedFractional = freeEntriesUsed - freeEntriesUsedInteger;

			uintptr_t remainderSize = guessedFreeEntrySize - usedPortionOfFreeEntry;
			if (remainderSize < _sizeClassSizes[sizeClassIndex]) {

				if (0 != freeEntriesUsedInteger) {
					/* no need to decrement anything - we already accounted it for */
					if (remainderSize >= _largeObjectThreshold) {
						uintptr_t updatedFreeEntrySize = incrementFreeEntrySizeClassStats(remainderSize, &ext->freeEntrySizeClassStatsSimulated, freeEntriesUsedInteger);
						Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_remainder(env->getLanguageVMThread(), freeEntriesUsedInteger, remainderSize, updatedFreeEntrySize);
						*currentFreeMemory += updatedFreeEntrySize * freeEntriesUsedInteger;
					}
				}

				/* possibly one more remainder of even larger size */
				remainderSize = guessedFreeEntrySize - (uintptr_t)(freeEntriesUsedFractional * usedPortionOfFreeEntry);

				/* often it will stay in the same size class, but if not, we have to update counters */
				if (remainderSize < _sizeClassSizes[sizeClassIndex]) {
					Assert_MM_true(ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex] > 0);
					Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_adjustement(env->getLanguageVMThread(), ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex] - 1, ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex]);
					ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex] -= 1;
					*currentFreeMemory -= _sizeClassSizes[sizeClassIndex];
					if (remainderSize >= _largeObjectThreshold) {
						uintptr_t updatedFreeEntrySize = incrementFreeEntrySizeClassStats(remainderSize, &ext->freeEntrySizeClassStatsSimulated);
						Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_remainder(env->getLanguageVMThread(), 1, remainderSize, updatedFreeEntrySize);
						*currentFreeMemory += updatedFreeEntrySize;
					}
				}
			}

			/* done with 'regular' size */
			Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_freeEntry_exit(env->getLanguageVMThread(), "Object", "regular", sizeClassIndex, ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex]);

		}
		if (0 != ext->freeEntrySizeClassStatsSimulated.getFrequentAllocCount(sizeClassIndex)) {
			/* for each size class, find frequent allocation sizes */
			MM_FreeEntrySizeClassStats::FrequentAllocation *curr = ext->freeEntrySizeClassStatsSimulated._frequentAllocationHead[sizeClassIndex];
			MM_FreeEntrySizeClassStats::FrequentAllocation *next = NULL;
			MM_FreeEntrySizeClassStats::FrequentAllocation *prev = NULL;

			/* find first available frequent-alloc free size that objectSize will fit */
			while ((NULL != curr) && ((objectSize > curr->_size) || (0 == curr->_count))) {
				prev = curr;
				curr = curr->_nextInSizeClass;
			}

			while ((allocBytes > 0) && (NULL != curr)) {
				uintptr_t currentSize = curr->_size;
				Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_freeEntry_entry(env->getLanguageVMThread(), "Object", "frequent", sizeClassIndex, currentSize, curr->_count);

				uintptr_t freeEntryOverAllocSizeRatio = currentSize / objectSize;
				uintptr_t usedPortionOfFreeEntry = freeEntryOverAllocSizeRatio * objectSize;
				uintptr_t availableBytes = curr->_count * usedPortionOfFreeEntry;
				Trc_MM_LargeObjectAllocateStats_simulateAllocateObjects_availableBytes(env->getLanguageVMThread(), "frequent", availableBytes, curr->_count, usedPortionOfFreeEntry, freeEntryOverAllocSizeRatio, objectSize);

				float freeEntriesUsed = 0;

				Assert_MM_true(objectSize <= currentSize);
				Assert_MM_true(0 != curr->_count);

				if (availableBytes <= allocBytes) {
					Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_satisfy(env->getLanguageVMThread(), "partial", "frequent", curr->_count, sizeClassIndex);
					Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_partial(env->getLanguageVMThread(), allocBytes - availableBytes, allocBytes, availableBytes);

					/* all available memory of this size will be used */
					freeEntriesUsed = (float)curr->_count;
					*currentFreeMemory -= curr->_count * currentSize;
					allocBytes -= availableBytes;
					allocObjectCount = allocBytes / objectSize;
					next = curr->_nextInSizeClass;

				} else {
					/* all allocation is satisfied */
					freeEntriesUsed = (float)allocObjectCount / (float)freeEntryOverAllocSizeRatio;
					Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_satisfy(env->getLanguageVMThread(), "full", "frequent", curr->_count, sizeClassIndex);
					Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_full(env->getLanguageVMThread(), curr->_count - (uintptr_t)freeEntriesUsed, curr->_count, freeEntriesUsed);
					*currentFreeMemory -= (uintptr_t)freeEntriesUsed * currentSize;

					allocBytes = 0;
					allocObjectCount = 0;
				}
				updateFreeEntrySizeClassStats(currentSize, &ext->freeEntrySizeClassStatsSimulated, -((intptr_t)freeEntriesUsed), sizeClassIndex, prev, curr);

				/* there are two kind of remainders:
				 1) freeEntriesUsed of size: freeEntrySize - usedPortionOfFreeEntry
				 2) up to one of size: freeEntrySize - decimal-fraction-of-freeEntriesUsed-float * usedPortionOfFreeEntry
				*/

				/* TODO: do we need random factor as for regular sizes? */
				uintptr_t freeEntrySize = currentSize;

				uintptr_t freeEntriesUsedInteger = (uintptr_t)freeEntriesUsed;
				float freeEntriesUsedFractional = freeEntriesUsed - freeEntriesUsedInteger;

				uintptr_t remainderSize = freeEntrySize - usedPortionOfFreeEntry;
				if (remainderSize < _sizeClassSizes[sizeClassIndex]) {

					if (0 != freeEntriesUsedInteger) {
						/* no need to decrement anything - we already accounted it for */
						if (remainderSize >= _largeObjectThreshold) {
							uintptr_t updatedFreeEntrySize = incrementFreeEntrySizeClassStats(remainderSize, &ext->freeEntrySizeClassStatsSimulated, freeEntriesUsedInteger);
							Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_remainder(env->getLanguageVMThread(), freeEntriesUsedInteger, remainderSize, updatedFreeEntrySize);
							*currentFreeMemory += updatedFreeEntrySize * freeEntriesUsedInteger;
						}
					}

					/* possibly one more remainder of even larger size */
					remainderSize = freeEntrySize - (uintptr_t)(freeEntriesUsedFractional * usedPortionOfFreeEntry);

					if (remainderSize < currentSize) {
						Assert_MM_true(((intptr_t)curr->_count) > 0);
						Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_adjustement(env->getLanguageVMThread(), curr->_count - 1, curr->_count);

						updateFreeEntrySizeClassStats(currentSize, &ext->freeEntrySizeClassStatsSimulated, -1, sizeClassIndex, prev, curr);

						*currentFreeMemory -= currentSize;
						if (remainderSize >= _largeObjectThreshold) {
							uintptr_t updatedFreeEntrySize = incrementFreeEntrySizeClassStats(remainderSize, &ext->freeEntrySizeClassStatsSimulated);
							Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_remainder(env->getLanguageVMThread(), 1, remainderSize, updatedFreeEntrySize);
							*currentFreeMemory += updatedFreeEntrySize;
						}
					}
				}

				Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_freeEntry_exit(env->getLanguageVMThread(), "Object", "frequent", sizeClassIndex, curr->_count);

				/* any allocation left? */
				if (allocBytes > 0) {
					curr = next;
					/* find next size that objectSize will fit */
					while ((NULL != curr) && (0 == curr->_count)) {
						prev = curr;
						curr = curr->_nextInSizeClass;
					}
				}
			}
		}

		/* any allocation left? */
		if (allocBytes > 0) {
			/* move to the next size class */
			sizeClassIndex += 1;
		}
	}

	Trc_MM_LargeObjectAllocateStats_simulateAllocateObjects_exit(env->getLanguageVMThread(), objectSize, allocBytes, allocBytes >> 20, *currentFreeMemory, *currentFreeMemory >> 20);

	/* return amount of unsatisfied allocate */
	return allocBytes;
}

MMINLINE uintptr_t
MM_LargeObjectAllocateStats::getNextSizeClass(uintptr_t sizeClassIndex, uintptr_t maxSizeClasses)
{
	return ((sizeClassIndex+1) % maxSizeClasses);
}

MMINLINE bool 
MM_LargeObjectAllocateStats::isFirstIterationCompleteForCurrentStride(uintptr_t sizeClassIndex, uintptr_t maxSizeClasses)
{
	return (sizeClassIndex == getNextSizeClass(_TLHSizeClassIndex, maxSizeClasses));
}

uintptr_t
MM_LargeObjectAllocateStats::simulateAllocateTLHs(MM_EnvironmentBase *env, uintptr_t allocBytes, uintptr_t *currentFreeMemory, uintptr_t strides)
{
	MM_GCExtensionsBase *ext = env->getExtensions();
	Assert_MM_true(NULL != ext->freeEntrySizeClassStatsSimulated._frequentAllocationHead);

	/* restore simulateTLH state */
	uintptr_t sizeClassIndex = _TLHSizeClassIndex;
	MM_FreeEntrySizeClassStats::FrequentAllocation *next = NULL;
	/* using to detect unsatisfied case */
	bool firstIterationForCurrentStride = true; 
	uintptr_t allocBytesPerStride = allocBytes/strides;
	uintptr_t allocBytesForCurrentStride = allocBytesPerStride;
	uintptr_t maxSizeClasses = ext->freeEntrySizeClassStatsSimulated._maxSizeClasses;
	
	Trc_MM_LargeObjectAllocateStats_simulateAllocateTLHs_entry(env->getLanguageVMThread(), allocBytes, allocBytes >> 20, *currentFreeMemory, *currentFreeMemory >> 20);
	/* keep allocating from this size class or any larger */
	while ((allocBytesForCurrentStride > 0) && (firstIterationForCurrentStride || !isFirstIterationCompleteForCurrentStride(sizeClassIndex, maxSizeClasses))) {
		if (firstIterationForCurrentStride && isFirstIterationCompleteForCurrentStride(sizeClassIndex, maxSizeClasses)) {
			firstIterationForCurrentStride = false;
		}
		/* any available free entries of this size class? */
		/* TODO: find a faster way to find next non zero size class index */
		if (0 == _TLHFrequentAllocationSize) {
			/* exhaused the current size class. move to next one */
			if (0 != ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex]) {
				Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_freeEntry_entry(env->getLanguageVMThread(), "TLH", "regular", sizeClassIndex, _sizeClassSizes[sizeClassIndex] , ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex]);

				/* go for smallest to largest sizes within a class size; thus start with the 'regular' size and continue with frequent allocate sizes as ordered in the list (ascending) */

				/* dealing with 'regular' size */
				uintptr_t availableBytes = ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex] * _sizeClassSizes[sizeClassIndex];
				float freeEntriesUsed = 0.0;

				Trc_MM_LargeObjectAllocateStats_simulateAllocateTLHs_availableBytes(env->getLanguageVMThread(), "regular", availableBytes, ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex], _sizeClassSizes[sizeClassIndex]);

				next = ext->freeEntrySizeClassStatsSimulated._frequentAllocationHead[sizeClassIndex];
				if (availableBytes <= allocBytesForCurrentStride) {
					Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_satisfy(env->getLanguageVMThread(), "partial", "regular", _sizeClassSizes[sizeClassIndex], sizeClassIndex);
					Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_partial(env->getLanguageVMThread(), allocBytesForCurrentStride - availableBytes, allocBytesForCurrentStride, availableBytes);

					/* all available memory of this size will be used (but we my not satisfy all allocation) */
					freeEntriesUsed = (float)ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex];
					*currentFreeMemory -= availableBytes;
					ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex] = 0;
					allocBytesForCurrentStride -= availableBytes;
				} else {
					/* all allocation is satisfied (and there may be entries of this size left) */
					freeEntriesUsed = (float)allocBytesForCurrentStride / (float)_sizeClassSizes[sizeClassIndex];
					Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_satisfy(env->getLanguageVMThread(), "full", "regular", _sizeClassSizes[sizeClassIndex], sizeClassIndex);
					Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_full(env->getLanguageVMThread(), ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex] - (uintptr_t)freeEntriesUsed, ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex], freeEntriesUsed);
					ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex] -= (uintptr_t)freeEntriesUsed;
					*currentFreeMemory -= (uintptr_t)freeEntriesUsed * _sizeClassSizes[sizeClassIndex];
					allocBytesForCurrentStride = 0;
				}

				/* there is one kind of remainder:
				 up to one of size: freeEntrySize - decimal-fraction-of-freeEntriesUsed-float * _sizeClassSizes[sizeClassIndex]
				*/

				/* we do not know the exact size of free entry(ies). we do know low and hi bounds, and will just approximate it (them) with a random size within the range */
				uintptr_t minFreeEntrySize = _sizeClassSizes[sizeClassIndex];
				uintptr_t maxFreeEntrySize = UDATA_MAX;

				if (NULL != ext->freeEntrySizeClassStatsSimulated._frequentAllocationHead[sizeClassIndex]) {
					/* since we deal with regular size, first larger size might be the first frequent alloc size, if any */
					maxFreeEntrySize = ext->freeEntrySizeClassStatsSimulated._frequentAllocationHead[sizeClassIndex]->_size;
				} else if (sizeClassIndex + 1 < maxSizeClasses) {
					maxFreeEntrySize = _sizeClassSizes[sizeClassIndex + 1];
				}

				uintptr_t freeEntrySize = minFreeEntrySize + (uintptr_t) ((maxFreeEntrySize - minFreeEntrySize) * ((float)rand() / (float)RAND_MAX));

				uintptr_t freeEntriesUsedInteger = (uintptr_t)freeEntriesUsed;
				float freeEntriesUsedFractional = freeEntriesUsed - freeEntriesUsedInteger;

				uintptr_t remainderSize = freeEntrySize - (uintptr_t)(freeEntriesUsedFractional * _sizeClassSizes[sizeClassIndex]);

				/* often it will stay in the same size class, but if not, we have to update counters */
				if (remainderSize < _sizeClassSizes[sizeClassIndex]) {
					Assert_MM_true(((intptr_t)ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex]) > 0);
					Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_adjustement(env->getLanguageVMThread(), ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex] - 1, ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex]);
					ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex] -= 1;
					*currentFreeMemory -= _sizeClassSizes[sizeClassIndex];
					if (remainderSize >= _largeObjectThreshold) {
						uintptr_t updatedFreeEntrySize = incrementFreeEntrySizeClassStats(remainderSize, &ext->freeEntrySizeClassStatsSimulated);
						Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_remainder(env->getLanguageVMThread(), 1, remainderSize, updatedFreeEntrySize);
						*currentFreeMemory += updatedFreeEntrySize;
					}
				}

				/* done with 'regular' size; now deal with frequent alloc sizes */
				Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_freeEntry_exit(env->getLanguageVMThread(), "TLH", "regular", sizeClassIndex, ext->freeEntrySizeClassStatsSimulated._count[sizeClassIndex]);
				if (0 == allocBytesForCurrentStride) {
					if (strides > 1) {
						strides -= 1;
						allocBytesForCurrentStride = allocBytesPerStride;
						firstIterationForCurrentStride = true;
						_TLHSizeClassIndex = getNextSizeClass(sizeClassIndex, maxSizeClasses);
					}
				}
			}
		}

		if ((allocBytesForCurrentStride > 0) && (0 != ext->freeEntrySizeClassStatsSimulated.getFrequentAllocCount(sizeClassIndex))) {
			/* for each size class, try to find frequent allocation sizes equal or larger than preserved size */
			MM_FreeEntrySizeClassStats::FrequentAllocation *curr = ext->freeEntrySizeClassStatsSimulated._frequentAllocationHead[sizeClassIndex];
			MM_FreeEntrySizeClassStats::FrequentAllocation *prev = NULL;
			if (0 != _TLHFrequentAllocationSize) {
				while ((NULL != curr) && (curr->_size < _TLHFrequentAllocationSize)) {
					prev = curr;
					curr = curr->_nextInSizeClass;
				}
				if (NULL == curr) {
					prev = NULL;
					Assert_MM_true(sizeClassIndex >= _veryLargeEntrySizeClass);
				}
				_TLHFrequentAllocationSize = 0;
			}
			next = NULL;

			/* find first non-empty frequent-alloc size */
			while ((NULL != curr) && (0 == curr->_count)) {
				prev = curr;
				curr = curr->_nextInSizeClass;
			}

			while ((allocBytesForCurrentStride > 0) && (NULL != curr)) {
				uintptr_t currentSize = curr->_size;
				Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_freeEntry_entry(env->getLanguageVMThread(), "TLH", "frequent", sizeClassIndex, currentSize, curr->_count);

				uintptr_t availableBytes = curr->_count * currentSize;
				float freeEntriesUsed = 0;

				Trc_MM_LargeObjectAllocateStats_simulateAllocateTLHs_availableBytes(env->getLanguageVMThread(), "frequent", availableBytes, curr->_count, currentSize);

				Assert_MM_true(0 != curr->_count);

				next = curr->_nextInSizeClass;
				if (availableBytes <= allocBytesForCurrentStride) {
					Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_satisfy(env->getLanguageVMThread(), "partial", "frequent", curr->_count, sizeClassIndex);
					Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_partial(env->getLanguageVMThread(), allocBytesForCurrentStride - availableBytes, allocBytesForCurrentStride, availableBytes);

					/* all available memory of this size will be used */
					freeEntriesUsed = (float)curr->_count;
					*currentFreeMemory -= availableBytes;
					allocBytesForCurrentStride -= availableBytes;
				} else {
					/* all allocation is satisfied */
					freeEntriesUsed = (float)allocBytesForCurrentStride / (float)currentSize;
					Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_satisfy(env->getLanguageVMThread(), "full", "frequent", curr->_count, sizeClassIndex);
					Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_full(env->getLanguageVMThread(), curr->_count - (uintptr_t)freeEntriesUsed, curr->_count, freeEntriesUsed);

					*currentFreeMemory -= (uintptr_t)freeEntriesUsed * currentSize;
					allocBytesForCurrentStride = 0;
				}
				updateFreeEntrySizeClassStats(currentSize, &ext->freeEntrySizeClassStatsSimulated, -((intptr_t)freeEntriesUsed), sizeClassIndex, prev, curr);

				/* there is one kind of remainder:
				 up to one of size: freeEntrySize - decimal-fraction-of-freeEntriesUsed-float * curr->_size
				*/

				uintptr_t freeEntrySize = currentSize;

				uintptr_t freeEntriesUsedInteger = (uintptr_t)freeEntriesUsed;
				float freeEntriesUsedFractional = freeEntriesUsed - freeEntriesUsedInteger;

				uintptr_t remainderSize = freeEntrySize - (uintptr_t)(freeEntriesUsedFractional * currentSize);

				if (remainderSize < currentSize) {
					Assert_MM_true(((intptr_t)curr->_count) > 0);
					Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_adjustement(env->getLanguageVMThread(), curr->_count - 1, curr->_count);

					updateFreeEntrySizeClassStats(currentSize, &ext->freeEntrySizeClassStatsSimulated, -1, sizeClassIndex, prev, curr);

					*currentFreeMemory -= currentSize;
					if (remainderSize >= _largeObjectThreshold) {
						uintptr_t updatedFreeEntrySize = incrementFreeEntrySizeClassStats(remainderSize, &ext->freeEntrySizeClassStatsSimulated);
						Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_remainder(env->getLanguageVMThread(), 1, remainderSize, updatedFreeEntrySize);
						*currentFreeMemory += updatedFreeEntrySize;
					}
				}

				Trc_MM_LargeObjectAllocateStats_simulateAllocateCommon_freeEntry_exit(env->getLanguageVMThread(), "TLH", "frequent", sizeClassIndex, curr->_count);

				if (0 == allocBytesForCurrentStride) {
					/* exhausted all allocation for current stride, move to next stride */
					if (strides > 1) {
						strides -= 1;
						allocBytesForCurrentStride = allocBytesPerStride;
						firstIterationForCurrentStride = true;
						_TLHSizeClassIndex = getNextSizeClass(sizeClassIndex, maxSizeClasses);
						if (curr->_nextInSizeClass == next) {
							prev = curr;
						}
					}
				}
				
				if (allocBytesForCurrentStride > 0) {
					/* find next non-empty frequent-alloc size */
					curr = next;
					while ((NULL != curr) && (0 == curr->_count)) {
						prev = curr;
						curr = curr->_nextInSizeClass;
					}
				}
			}
		}

		if (allocBytesForCurrentStride > 0) {
			/* move to the next size class (round robin)*/
			sizeClassIndex = getNextSizeClass(sizeClassIndex, maxSizeClasses);
			_TLHFrequentAllocationSize = 0;
		}
	}

	/* increment and preserve simulateTLH state */
	if (0 == allocBytesForCurrentStride) {

		if (NULL == next) {
			/* move to the next size class (round robin)*/
			sizeClassIndex = getNextSizeClass(sizeClassIndex, maxSizeClasses);
			next = NULL;
		}

		_TLHSizeClassIndex = sizeClassIndex;
		if (NULL != next) {
			_TLHFrequentAllocationSize = next->_size;
		}
		allocBytes = 0;
	} else {
		allocBytes = allocBytesForCurrentStride + (strides - 1) * allocBytesPerStride;
	}
	Trc_MM_LargeObjectAllocateStats_simulateAllocateTLHs_exit(env->getLanguageVMThread(), allocBytesForCurrentStride, allocBytes >> 20, *currentFreeMemory, *currentFreeMemory >> 20);
	/* return amount of unsatisfied allocate */
	return allocBytes;
}
void
MM_LargeObjectAllocateStats::averageForSpaceSaving(MM_EnvironmentBase *env, OMRSpaceSaving* spaceSavingToAverageWith, OMRSpaceSaving** spaceSavingAveragePercent, uintptr_t bytesAllocatedThisRound)
{
	/* no updates, if no allocation this round */
	if (0 == bytesAllocatedThisRound) {
		return;
	}

	/* if bytesAllocatedThisRound == _averageBytesAllocated, the historic average will carry 9/10 weight (new weight is 1/10)
	 * if bytesAllocatedThisRound = 9 x _averageBytesAllocated, the historic average will will carry 1/2 weight (new weight is 1/2)
	 * as bytesAllocatedThisRound / _averageBytesAllocated ratio approaches infinity, the historic average weight will approach 0 (new weight approaches 1)
	 * if _averageBytesAllocated = 9 x bytesAllocatedThisRound, the historic average weight will carry 99/100 weigth (new weight is 1/100)
	 * as bytesAllocatedThisRound / _averageBytesAllocated approaches 0, the historic average weight will approach 1 (new weight approaches 0)
	 */
	float avgWeightBase = (float)0.9;
	float newWeight = 1 - (10 * avgWeightBase * _averageBytesAllocated) / (bytesAllocatedThisRound + 10 * avgWeightBase * _averageBytesAllocated);
	Assert_MM_true((0.0 <= newWeight ) && (newWeight <= 1.0));

	spaceSavingClear(_spaceSavingTemp);

	uintptr_t i = 0;
	/* walk averagePercent values, apply weight and store to temp */
	for(i = 0; i < spaceSavingGetCurSize(*spaceSavingAveragePercent); i++ ) {
		void *key = spaceSavingGetKthMostFreq(*spaceSavingAveragePercent, i + 1);
		uintptr_t percentsEncoded = spaceSavingGetKthMostFreqCount(*spaceSavingAveragePercent, i + 1);
		uintptr_t percentsEncodedWeighted = (uintptr_t)(percentsEncoded * (1 - newWeight));
		spaceSavingUpdate(_spaceSavingTemp, key, percentsEncodedWeighted);
	}

	/* walk new values, upsample, apply newWeight, encode and add to temp */
	for(i = 0; i < spaceSavingGetCurSize(spaceSavingToAverageWith); i++ ) {
		void *key = spaceSavingGetKthMostFreq(spaceSavingToAverageWith, i + 1);
		uintptr_t bytesAllocated = spaceSavingGetKthMostFreqCount(spaceSavingToAverageWith, i + 1);

		/* TODO: enable upSampleAllocStats, currently do not count the allocation in TLH, due to inaccurate upSample in some cases */
		uintptr_t bytesAllocatedUpSampled = bytesAllocated;
		/*	uintptr_t bytesAllocatedUpSampled = upSampleAllocStats(env, (uintptr_t)key, bytesAllocated); */
		float bytesAllocatedWeighted = bytesAllocatedUpSampled * newWeight;
		float percentsWeighted = bytesAllocatedWeighted * (float)100.0 / bytesAllocatedThisRound;
		uintptr_t percentsEncodedWeighted = convertPercentFloatToUDATA(percentsWeighted);
		spaceSavingUpdate(_spaceSavingTemp, key, percentsEncodedWeighted);
	}

	/* The final result in _spaceSavingTemp. Swap spaceSavingAveragePercent and _spaceSavingTemp. */
	OMRSpaceSaving *temp = *spaceSavingAveragePercent;
	*spaceSavingAveragePercent = _spaceSavingTemp;
	_spaceSavingTemp = temp;

}

void
MM_LargeObjectAllocateStats::average(MM_EnvironmentBase *env, uintptr_t bytesAllocatedThisRound)
{
	averageForSpaceSaving(env, _spaceSavingSizes, &_spaceSavingSizesAveragePercent, bytesAllocatedThisRound);
	averageForSpaceSaving(env, _spaceSavingSizeClasses, &_spaceSavingSizeClassesAveragePercent, bytesAllocatedThisRound);

	const float avgWeight = (float)0.9;

	_averageBytesAllocated = (uintptr_t)((1 - avgWeight) * bytesAllocatedThisRound + avgWeight * _averageBytesAllocated);
}

MMINLINE uintptr_t
MM_LargeObjectAllocateStats::updateFreeEntrySizeClassStats(uintptr_t freeEntrySize, MM_FreeEntrySizeClassStats *freeEntrySizeClassStats, intptr_t count, uintptr_t sizeClassIndex, MM_FreeEntrySizeClassStats::FrequentAllocation* prev, MM_FreeEntrySizeClassStats::FrequentAllocation* curr)
{
	uintptr_t returnSize = 0;
	if (sizeClassIndex >= _veryLargeEntrySizeClass) {
		if ((NULL != curr) && (freeEntrySize == curr->_size)) {
			curr->_count += count;

			if (0 == curr->_count) {
				if (NULL == prev) {
					freeEntrySizeClassStats->_frequentAllocationHead[sizeClassIndex] = curr->_nextInSizeClass;
				} else {
					prev->_nextInSizeClass = curr->_nextInSizeClass;
				}
				curr->_nextInSizeClass = freeEntrySizeClassStats->_freeHeadVeryLargeEntry;
				freeEntrySizeClassStats->_freeHeadVeryLargeEntry = curr;
			}
			returnSize = freeEntrySize;
		} else {
			if (NULL != freeEntrySizeClassStats->_freeHeadVeryLargeEntry) {
				MM_FreeEntrySizeClassStats::FrequentAllocation *newVeryLargeEntry = freeEntrySizeClassStats->_freeHeadVeryLargeEntry;
				freeEntrySizeClassStats->_freeHeadVeryLargeEntry = newVeryLargeEntry->_nextInSizeClass;

				newVeryLargeEntry->_size = freeEntrySize;
				newVeryLargeEntry->_count = count;
				newVeryLargeEntry->_nextInSizeClass = curr;
				if (NULL != prev) {
					prev->_nextInSizeClass = newVeryLargeEntry;
				} else {
					freeEntrySizeClassStats->_frequentAllocationHead[sizeClassIndex] = newVeryLargeEntry;
				}
				returnSize = freeEntrySize;
			} else {
				Assert_MM_false(freeEntrySizeClassStats->guarantyEnoughPoolSizeForVeryLargeEntry);
				/* lack of VeryLargeEntryPool */
				freeEntrySizeClassStats->_count[sizeClassIndex] += count;
				returnSize = _sizeClassSizes[sizeClassIndex];
			}
		}
	} else {
		if (NULL != curr && freeEntrySize == curr->_size) {
			curr->_count += count;
			returnSize = curr->_size;
		} else if (NULL != prev) {
			prev->_count += count;
			returnSize = prev->_size;
		} else {
			freeEntrySizeClassStats->_count[sizeClassIndex] += count;
			returnSize = _sizeClassSizes[sizeClassIndex];
		}
	}

	return returnSize;
}

uintptr_t
MM_LargeObjectAllocateStats::incrementFreeEntrySizeClassStats(uintptr_t freeEntrySize, MM_FreeEntrySizeClassStats *freeEntrySizeClassStats, uintptr_t count)
{
	uintptr_t sizeClassIndex = getSizeClassIndex(freeEntrySize);
	
	Assert_MM_true(0 != freeEntrySizeClassStats->getMaxSizeClasses());
	/* for this sizeClass, walk the (ascending) list of frequent allocations, and find a match, if any */
	MM_FreeEntrySizeClassStats::FrequentAllocation *frequentAllocation = freeEntrySizeClassStats->_frequentAllocationHead[sizeClassIndex];
	MM_FreeEntrySizeClassStats::FrequentAllocation *prevFrequentAllocation = NULL;
	while ((NULL != frequentAllocation) && (freeEntrySize > frequentAllocation->_size)) {
		prevFrequentAllocation = frequentAllocation;
		frequentAllocation = frequentAllocation->_nextInSizeClass;
	}

	return updateFreeEntrySizeClassStats(freeEntrySize, freeEntrySizeClassStats, count, sizeClassIndex, prevFrequentAllocation, frequentAllocation);
}

uintptr_t
MM_LargeObjectAllocateStats::incrementFreeEntrySizeClassStats(uintptr_t freeEntrySize)
{
	return incrementFreeEntrySizeClassStats(freeEntrySize, &_freeEntrySizeClassStats, 1);
}

void
MM_LargeObjectAllocateStats::incrementTlhAllocSizeClassStats(uintptr_t freeEntrySize)
{
#if defined(OMR_GC_THREAD_LOCAL_HEAP)
	uintptr_t sizeClassIndex = getSizeClassIndex(freeEntrySize);
	Assert_MM_true(sizeClassIndex < _tlhAllocSizeClassStats._maxSizeClasses);
	_tlhAllocSizeClassStats._count[sizeClassIndex] += 1;
#endif
}

void
MM_LargeObjectAllocateStats::decrementFreeEntrySizeClassStats(uintptr_t freeEntrySize)
{
	return decrementFreeEntrySizeClassStats(freeEntrySize, &_freeEntrySizeClassStats, 1);
}

void
MM_LargeObjectAllocateStats::decrementFreeEntrySizeClassStats(uintptr_t freeEntrySize, MM_FreeEntrySizeClassStats *freeEntrySizeClassStats, uintptr_t count)
{
	uintptr_t sizeClassIndex = getSizeClassIndex(freeEntrySize);

	/* for this sizeClass, walk the (ascending) list of frequent allocations, and find a match, if any */
	MM_FreeEntrySizeClassStats::FrequentAllocation *frequentAllocation = freeEntrySizeClassStats->_frequentAllocationHead[sizeClassIndex];
	MM_FreeEntrySizeClassStats::FrequentAllocation *prevFrequentAllocation = NULL;

	while ((NULL != frequentAllocation) && (freeEntrySize > frequentAllocation->_size)) {
		prevFrequentAllocation = frequentAllocation;
		frequentAllocation = frequentAllocation->_nextInSizeClass;
	}

	updateFreeEntrySizeClassStats(freeEntrySize, freeEntrySizeClassStats, -((intptr_t)count), sizeClassIndex, prevFrequentAllocation, frequentAllocation);
}

uintptr_t
MM_LargeObjectAllocateStats::getSizeClassIndex(uintptr_t size)
{
	float logValue = logf((float)size);
	/*
	 * We discovered a data corruption in this function
	 * The reason for it is a corruption of float point registers at time of switching of context
	 * for a few RT Linux kernels. If one of this assertions is triggered call for IT Support to check/update
	 * a Linux kernel on problematic machine
	 */

	/* the logarithm can not be negative! */
	Assert_GC_true_with_message2(_env, (logValue >= 0.0), "Error calculation logf(), passed %zu, returned %f\n", size, logValue);
	Assert_GC_true_with_message(_env, (_sizeClassRatioLog > 0.0), "_sizeClassRatioLog is %f but must be larger then zero\n", _sizeClassRatioLog);

	uintptr_t result = (uintptr_t)(logValue / _sizeClassRatioLog);

	/* the logarithm value is larger then we can accept - probably larger then logf(UDATA_MAX) */
	Assert_GC_true_with_message2(_env, ((_freeEntrySizeClassStats._maxSizeClasses == 0) || (result < _freeEntrySizeClassStats._maxSizeClasses)),
			"Calculated value of getSizeClassIndex() %zu can not be larger then maximum %zu\n", result, _freeEntrySizeClassStats._maxSizeClasses);

	return result;
}

void
MM_LargeObjectAllocateStats::verifyFreeEntryCount(uintptr_t actualFreeEntryCount)
{
	uintptr_t totalCount = 0;
	for (intptr_t sizeClassIndex = 0; sizeClassIndex < (intptr_t)getMaxSizeClasses(); sizeClassIndex++) {
		uintptr_t count = getFreeEntrySizeClassStats()->getCount(sizeClassIndex);
		uintptr_t frequentAllocCount = getFreeEntrySizeClassStats()->getFrequentAllocCount(sizeClassIndex);
		count += frequentAllocCount;
		if (0 != count) {
			totalCount += count;

			Assert_MM_true(frequentAllocCount <= count);
		}
	}
	Assert_MM_true(totalCount == actualFreeEntryCount);
}
