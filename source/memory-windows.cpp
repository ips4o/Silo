/*****************************************************************************
 * Silo
 *   Multi-platform topology-aware memory management library.
 *   Supports multiple styles of NUMA-aware memory allocation.
 *****************************************************************************
 * Authored by Samuel Grossman
 * Department of Electrical Engineering, Stanford University
 * Copyright (c) 2016
 *************************************************************************//**
 * @file memory-windows.cpp
 *   Implementation of functions that (de)allocate memory. 
 *   This file contains Windows-specific functions.
 *****************************************************************************/

#include "../silo.h"
#include "memory.h"
#include "pointermap.h"

#include <cstdint>
#include <cstdlib>
#include <topo.h>
#include <Windows.h>


// -------- INTERNAL FUNCTIONS --------------------------------------------- //

/// Allocates virtual memory at the specified starting address, optionally using large-page support.
/// This is a Windows-specific helper function.
/// @param [in] size Number of bytes to allocate.
/// @param [in] numaNode OS-specific index of the NUMA node on which to allocate the memory.
/// @param [in] startPtr Pointer that holds the desired starting address of the allocated region.
/// @param [in] shouldCommit Indicates that the virtual memory region should be committed, rather than just reserved in the process' virtual address space.
/// @param [in] useLargePageSupport `true` to indicate that large-page support should be requested, `false` otherwise.
/// @return Pointer to the start of the allocated buffer, or NULL on allocation failure.
static void* siloWindowsMemoryAllocAtNUMA(size_t size, uint32_t numaNode, void* startPtr, bool shouldCommit, bool useLargePageSupport)
{
    return VirtualAllocExNuma(GetCurrentProcess(), startPtr, size, MEM_RESERVE | (shouldCommit ? MEM_COMMIT : 0) | (useLargePageSupport ? MEM_LARGE_PAGES : 0), PAGE_READWRITE, numaNode);
}

/// Determines the allocation unit size, with or without considering large page support.
/// This is a Windows-specific helper function.
/// @param [in] useLargePageSupport `true` to indicate that large-page size should be considered, `false` otherwise.
/// @return Allocation unit size, the minimum size of each distinct piece of a multi-node array.
static DWORD siloWindowsGetAllocationUnitSize(bool useLargePageSupport)
{
    SYSTEM_INFO systemInfo;
    DWORD allocationUnitSize = 0, largePageSize = 0;

    GetSystemInfo(&systemInfo);

    if (useLargePageSupport)
        largePageSize = (DWORD)GetLargePageMinimum();

    // Return the largest of the system-reported allocation granularity, page size, and (if applicable) large page size.
    allocationUnitSize = systemInfo.dwAllocationGranularity;

    if (systemInfo.dwPageSize > allocationUnitSize)
        allocationUnitSize = systemInfo.dwPageSize;

    if (largePageSize > allocationUnitSize)
        allocationUnitSize = largePageSize;

    return allocationUnitSize;
}

/// Rounds the requested allocation size to one of a multiple of the allocation granularity.
/// This is a Windows-specific helper function.
/// @param [in] unroundedSize Unrounded size, in bytes.
/// @param [in] useLargePageSupport `true` to indicate that large-page size should be considered, `false` otherwise.
/// @return `unroundedSize`, rounded to the nearest multiple of the allocation granularity.
static size_t siloWindowsRoundRequestedAllocationSize(size_t unroundedSize, bool useLargePageSupport)
{
    const size_t allocationUnitSize = siloWindowsGetAllocationUnitSize(useLargePageSupport);
    
    const size_t quotient = unroundedSize / allocationUnitSize;
    const size_t remainder = unroundedSize % allocationUnitSize;
    
    if (remainder >= (allocationUnitSize / 2))
        return allocationUnitSize * (quotient + 1);
    else
        return allocationUnitSize * quotient;
}


// -------- FUNCTIONS ------------------------------------------------------ //
// See "memory.h" and "silo.h" for documentation.

void* siloMemoryAllocNUMA(size_t size, uint32_t numaNode)
{
    return siloWindowsMemoryAllocAtNUMA(size, numaNode, NULL, true, false);
}

// --------

void siloMemoryFreeNUMA(void* ptr, size_t size)
{
    VirtualFreeEx(GetCurrentProcess(), ptr, 0, MEM_RELEASE);
}

// --------

void* siloMultinodeArrayAlloc(uint32_t count, SSiloMemorySpec* spec)
{
    // Get the minimum allocation unit size.
    const size_t allocationUnitSize = siloWindowsGetAllocationUnitSize(false);
    
    // Compute the total number of bytes requested and granted, and simultaneously verify the passed NUMA node indices.
    size_t totalRequestedBytes = 0;
    size_t totalActualBytes = 0;
    
    for (uint32_t i = 0; i < count; ++i)
    {
        if (0 > topoGetNUMANodeOSIndex(spec[i].numaNode))
            return NULL;
        
        totalRequestedBytes += spec[i].size;
        spec[i].size = siloWindowsRoundRequestedAllocationSize(spec[i].size, false);
        totalActualBytes += spec[i].size;
    }
    
    // Verify that sufficient space was actually allocated on each node to justify even using this function.
    if (0 == totalActualBytes)
        return NULL;
    
    // Add sufficient additional space to allocate to the last NUMA node to ensure coverage of the total requested size.
    while (totalActualBytes < totalRequestedBytes)
    {
        totalActualBytes += allocationUnitSize;
        spec[count].size += allocationUnitSize;
    }

    // Reserve the entire virtual address space, as a way of checking for sufficient virtual address space and getting a base address.
    void* allocatedBuffer = siloWindowsMemoryAllocAtNUMA(totalActualBytes, 0, NULL, false, false);
    if (NULL == allocatedBuffer)
        return NULL;

    // Create an array of allocation specs into which to store information about each array piece.
    SSiloAllocationSpec* allocationSpecs = new SSiloAllocationSpec[count];

    // Free the reserved virtual address space, for future piece-wise allocation.
    siloMemoryFreeNUMA(allocatedBuffer, totalActualBytes);

    // Allocate each piece of the multi-node array.
    uint32_t numAllocated = 0;
    bool allocationSuccessful = true;
    for (; numAllocated < count; ++numAllocated)
    {
        // Attempt to allocate a piece of the array and bail if the attempt results in failure.
        void* allocationResult = siloWindowsMemoryAllocAtNUMA(spec[numAllocated].size, topoGetNUMANodeOSIndex(spec[numAllocated].numaNode), allocatedBuffer, true, false);
        if (NULL == allocationResult)
        {
            allocationSuccessful = false;
            break;
        }
        
        // Record the piece that was allocated.
        allocationSpecs[numAllocated].ptr = allocatedBuffer;
        allocationSpecs[numAllocated].size = spec[numAllocated].size;
        
        // Advance to the next piece.
        allocatedBuffer = (void*)((size_t)allocatedBuffer + spec[numAllocated].size);
    }
    
    // Check for success or failure.
    if (false == allocationSuccessful)
    {
        // If failed, free the pieces that actually were allocated.
        for (uint32_t i = 0; i < numAllocated; ++i)
            siloMemoryFreeNUMA(allocationSpecs[i].ptr, allocationSpecs[i].size);

        allocatedBuffer = NULL;
    }
    else
    {
        // If succeeded, return the base address of the allocated multi-node array and store its metadata.
        allocatedBuffer = allocationSpecs[0].ptr;
        siloPointerMapSubmit(numAllocated, allocationSpecs);
    }
    
    delete[] allocationSpecs;
    return allocatedBuffer;
}
