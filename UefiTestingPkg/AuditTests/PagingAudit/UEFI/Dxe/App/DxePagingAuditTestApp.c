/** @file -- DxePagingAuditTestApp.c
This Shell App tests the page table or writes page table and
memory map information to SFS

Copyright (c) Microsoft Corporation.
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "../../PagingAuditCommon.h"

#include <Protocol/ShellParameters.h>
#include <Protocol/Shell.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/MemoryProtectionSpecialRegionProtocol.h>
#include <Protocol/MemoryProtectionDebug.h>
#include <Protocol/MemoryAttribute.h>

#include <Library/FileHandleLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FlatPageTableLib.h>
#include <Library/UnitTestLib.h>
#include <Library/HobLib.h>
#include <Library/SafeIntLib.h>

#define UNIT_TEST_APP_NAME     "Paging Audit Test"
#define UNIT_TEST_APP_VERSION  "2"
#define MAX_CHARS_TO_READ      4

// TRUE if A interval subsumes B interval
#define CHECK_SUBSUMPTION(AStart, AEnd, BStart, BEnd) \
  ((AStart <= BStart) && (AEnd >= BEnd))

// TRUE if A and B have overlapping intervals
#define CHECK_OVERLAP(AStart, AEnd, BStart, BEnd)   \
  ((AEnd > AStart) && (BEnd > BStart) &&            \
  ((AStart <= BStart && AEnd > BStart) ||           \
  (BStart <= AStart && BEnd > AStart)))

// Aligns the input address down to the nearest page boundary
#define ALIGN_ADDRESS(Address)  ((Address / EFI_PAGE_SIZE) * EFI_PAGE_SIZE)

// Globals for memory protection special regions
MEMORY_PROTECTION_SPECIAL_REGION  *mSpecialRegions    = NULL;
UINTN                             mSpecialRegionCount = 0;

// Global for the non-protected image list
IMAGE_RANGE_DESCRIPTOR  *mNonProtectedImageList = NULL;

// Globals for the memory space map
EFI_GCD_MEMORY_SPACE_DESCRIPTOR  *mMemorySpaceMap     = NULL;
UINTN                            mMemorySpaceMapCount = 0;

// Globals for the EFI memory map
UINTN                  mEfiMemoryMapSize           = 0;
EFI_MEMORY_DESCRIPTOR  *mEfiMemoryMap              = NULL;
UINTN                  mEfiMemoryMapDescriptorSize = 0;

// Global for the flat page table
PAGE_MAP  mMap = { 0 };

// -------------------------------------------------
//    GLOBALS SUPPORT FUNCTIONS
// -------------------------------------------------

/**
  Return if the PE image section is aligned. This function must
  only be called using a loaded image's code type or EfiReservedMemoryType.
  Calling with a different type will ASSERT.

  @param[in]  SectionAlignment    PE/COFF section alignment
  @param[in]  MemoryType          PE/COFF image memory type

  @retval TRUE  The PE image section is aligned.
  @retval FALSE The PE image section is not aligned.
**/
BOOLEAN
IsLoadedImageSectionAligned (
  IN UINT32           SectionAlignment,
  IN EFI_MEMORY_TYPE  MemoryType
  )
{
  UINT32  PageAlignment;

  switch (MemoryType) {
    case EfiRuntimeServicesCode:
    case EfiACPIMemoryNVS:
      PageAlignment = RUNTIME_PAGE_ALLOCATION_GRANULARITY;
      break;
    case EfiRuntimeServicesData:
    case EfiACPIReclaimMemory:
      ASSERT (FALSE);
      PageAlignment = RUNTIME_PAGE_ALLOCATION_GRANULARITY;
      break;
    case EfiBootServicesCode:
    case EfiLoaderCode:
    case EfiReservedMemoryType:
      PageAlignment = EFI_PAGE_SIZE;
      break;
    default:
      ASSERT (FALSE);
      PageAlignment = EFI_PAGE_SIZE;
      break;
  }

  if ((SectionAlignment & (PageAlignment - 1)) != 0) {
    return FALSE;
  } else {
    return TRUE;
  }
}

/**
  Frees the entries in the mMap global.
**/
STATIC
VOID
FreePageTableMap (
  VOID
  )
{
  if (mMap.Entries != NULL) {
    FreePages (mMap.Entries, mMap.EntryPagesAllocated);
    mMap.Entries = NULL;
  }

  mMap.ArchSignature       = 0;
  mMap.EntryCount          = 0;
  mMap.EntryPagesAllocated = 0;
}

/**
  Populates the Page Table Map

  @retval EFI_SUCCESS   The page table map is fetched successfully.
  @retval other         An error occurred while fetching the page table map.
**/
STATIC
EFI_STATUS
PopulatePageTableMap (
  VOID
  )
{
  EFI_STATUS  Status;

  if ((mMap.Entries == NULL) || (mMap.EntryCount == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (mMap.Entries, mMap.EntryPagesAllocated * EFI_PAGE_SIZE);
  mMap.EntryCount = (mMap.EntryPagesAllocated * EFI_PAGE_SIZE) / sizeof (PAGE_MAP_ENTRY);
  Status          = CreateFlatPageTable (&mMap);

  return Status;
}

/**
  Checks the allocation size of the global page map buffer and reallocates it to be 20% larger
  if it is too small to hold the flat page table.

  @retval EFI_SUCCESS   Global buffer is large enough or was reallocated.
  @retval other         An error occurred while validating the global buffer.
**/
STATIC
EFI_STATUS
ValidatePageTableMapSize (
  VOID
  )
{
  PAGE_MAP    Map;
  EFI_STATUS  Status;

  Map.Entries             = NULL;
  Map.EntryCount          = 0;
  Map.EntryPagesAllocated = 0;

  Status = CreateFlatPageTable (&Map);

  if (Status != EFI_BUFFER_TOO_SMALL) {
    UT_LOG_ERROR ("Failed to get the required page table map size!\n");
    return EFI_ABORTED;
  }

  Map.EntryPagesAllocated = EFI_SIZE_TO_PAGES (Map.EntryCount * sizeof (PAGE_MAP_ENTRY));
  if (Map.EntryPagesAllocated >= mMap.EntryPagesAllocated) {
    FreePageTableMap ();
    mMap.EntryCount          = Map.EntryCount + (Map.EntryCount / 5); // Increase size by 20%
    mMap.EntryPagesAllocated = EFI_SIZE_TO_PAGES (mMap.EntryCount * sizeof (PAGE_MAP_ENTRY));
    mMap.Entries             = AllocatePages (mMap.EntryPagesAllocated);
    if (mMap.Entries == NULL) {
      UT_LOG_ERROR ("Failed to allocate %d pages for page table map!\n", mMap.EntryPagesAllocated);
      return EFI_OUT_OF_RESOURCES;
    }
  }

  return EFI_SUCCESS;
}

/**
  Frees the mNonProtectedImageList global
**/
STATIC
VOID
FreeNonProtectedImageList (
  VOID
  )
{
  LIST_ENTRY              *ImageRecordLink;
  IMAGE_RANGE_DESCRIPTOR  *CurrentImageRangeDescriptor;

  if (mNonProtectedImageList != NULL) {
    ImageRecordLink = &mNonProtectedImageList->Link;
    while (!IsListEmpty (ImageRecordLink)) {
      CurrentImageRangeDescriptor = CR (
                                      ImageRecordLink->ForwardLink,
                                      IMAGE_RANGE_DESCRIPTOR,
                                      Link,
                                      IMAGE_RANGE_DESCRIPTOR_SIGNATURE
                                      );

      RemoveEntryList (&CurrentImageRangeDescriptor->Link);
      FreePool (CurrentImageRangeDescriptor);
    }

    FreePool (mNonProtectedImageList);
    mNonProtectedImageList = NULL;
  }
}

/**
  Populates the mNonProtectedImageList global

  @retval EFI_SUCCESS   The non-protected image list is populated successfully.
  @retval other         An error occurred while populating the non-protected image list.
**/
STATIC
EFI_STATUS
PopulateNonProtectedImageList (
  VOID
  )
{
  EFI_STATUS                        Status;
  MEMORY_PROTECTION_DEBUG_PROTOCOL  *MemoryProtectionProtocol;

  MemoryProtectionProtocol = NULL;

  if (mNonProtectedImageList != NULL) {
    return EFI_SUCCESS;
  }

  Status = gBS->LocateProtocol (
                  &gMemoryProtectionDebugProtocolGuid,
                  NULL,
                  (VOID **)&MemoryProtectionProtocol
                  );

  if (!EFI_ERROR (Status)) {
    Status = MemoryProtectionProtocol->GetImageList (
                                         &mNonProtectedImageList,
                                         NonProtected
                                         );
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a:%d - Unable to fetch non-protected image list\n", __FUNCTION__, __LINE__));
    mNonProtectedImageList = NULL;
  }

  return Status;
}

/**
  Frees the mSpecialRegions global
**/
STATIC
VOID
FreeSpecialRegions (
  VOID
  )
{
  if (mSpecialRegions != NULL) {
    FreePool (mSpecialRegions);
    mSpecialRegions = NULL;
  }

  mSpecialRegionCount = 0;
}

/**
  Populates the special region array global

  @retval EFI_SUCCESS   The special region array is populated successfully.
  @retval other         An error occurred while populating the special region array.
**/
STATIC
EFI_STATUS
PopulateSpecialRegions (
  VOID
  )
{
  EFI_STATUS                                 Status;
  MEMORY_PROTECTION_SPECIAL_REGION_PROTOCOL  *SpecialRegionProtocol;

  SpecialRegionProtocol = NULL;

  if (mSpecialRegions != NULL) {
    return EFI_SUCCESS;
  }

  Status = gBS->LocateProtocol (
                  &gMemoryProtectionSpecialRegionProtocolGuid,
                  NULL,
                  (VOID **)&SpecialRegionProtocol
                  );

  if (!EFI_ERROR (Status)) {
    Status = SpecialRegionProtocol->GetSpecialRegions (
                                      &mSpecialRegions,
                                      &mSpecialRegionCount
                                      );
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a:%d - Unable to fetch special region list\n", __FUNCTION__, __LINE__));
    mSpecialRegions = NULL;
  }

  return Status;
}

/**
  Frees the memory space map global
**/
STATIC
VOID
FreeMemorySpaceMap (
  VOID
  )
{
  if (mMemorySpaceMap != NULL) {
    FreePool (mMemorySpaceMap);
    mMemorySpaceMap = NULL;
  }

  mMemorySpaceMapCount = 0;
}

/**
  Populates the memory space map global

  @retval EFI_SUCCESS   The memory space map is populated successfully.
  @retval other         An error occurred while populating the memory space map.
**/
STATIC
EFI_STATUS
PopulateMemorySpaceMap (
  VOID
  )
{
  EFI_STATUS  Status;

  if (mMemorySpaceMap != NULL) {
    return EFI_SUCCESS;
  }

  Status = gDS->GetMemorySpaceMap (&mMemorySpaceMapCount, &mMemorySpaceMap);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a:%d - Unable to fetch memory space map\n", __FUNCTION__, __LINE__));
    mMemorySpaceMap = NULL;
  }

  SortMemorySpaceMap (mMemorySpaceMap, mMemorySpaceMapCount, sizeof (EFI_GCD_MEMORY_SPACE_DESCRIPTOR));

  return Status;
}

/**
  Frees the EFI memory map global
**/
STATIC
VOID
FreeEfiMemoryMap (
  VOID
  )
{
  if (mEfiMemoryMap != NULL) {
    FreePool (mEfiMemoryMap);
    mEfiMemoryMap = NULL;
  }

  mEfiMemoryMapSize           = 0;
  mEfiMemoryMapDescriptorSize = 0;
}

/**
  Populates the EFI memory map global.

  @retval EFI_SUCCESS   The EFI memory map is populated successfully.
  @retval other         An error occurred while populating the EFI memory map.
**/
STATIC
EFI_STATUS
PopulateEfiMemoryMap (
  VOID
  )
{
  UINTN       EfiMapKey;
  UINT32      EfiDescriptorVersion;
  EFI_STATUS  Status;

  if ((mEfiMemoryMap == NULL) || (mEfiMemoryMapSize == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  mEfiMemoryMapDescriptorSize = 0;

  Status = gBS->GetMemoryMap (
                  &mEfiMemoryMapSize,
                  mEfiMemoryMap,
                  &EfiMapKey,
                  &mEfiMemoryMapDescriptorSize,
                  &EfiDescriptorVersion
                  );

  SortMemoryMap (mEfiMemoryMap, mEfiMemoryMapSize, mEfiMemoryMapDescriptorSize);

  return Status;
}

/**
  Checks the allocation size of the global EFI memory map buffer and reallocates it to be 20%
  larger if it is too small to hold the EFI memory map.

  @retval EFI_SUCCESS   Global buffer is large enough or was reallocated.
  @retval other         An error occurred while validating the global buffer.
**/
STATIC
EFI_STATUS
ValidateEfiMemoryMapSize (
  VOID
  )
{
  EFI_MEMORY_DESCRIPTOR  *EfiMemoryMap;
  UINTN                  EfiMemoryMapSize;
  UINTN                  EfiMemoryMapDescriptorSize;
  UINTN                  EfiMapKey;
  UINT32                 EfiDescriptorVersion;
  EFI_STATUS             Status;

  EfiMemoryMapSize           = 0;
  EfiMemoryMap               = NULL;
  EfiMemoryMapDescriptorSize = 0;
  EfiMapKey                  = 0;
  EfiDescriptorVersion       = 0;

  Status = gBS->GetMemoryMap (
                  &EfiMemoryMapSize,
                  EfiMemoryMap,
                  &EfiMapKey,
                  &EfiMemoryMapDescriptorSize,
                  &EfiDescriptorVersion
                  );

  if (Status != EFI_BUFFER_TOO_SMALL) {
    UT_LOG_ERROR ("Failed to get the required EFI memory map size!\n");
    return EFI_ABORTED;
  }

  if (EfiMemoryMapSize >= mEfiMemoryMapSize) {
    FreeEfiMemoryMap ();
    mEfiMemoryMapSize = EfiMemoryMapSize + (EfiMemoryMapSize / 5); // Increase size by 20%
    mEfiMemoryMap     = AllocatePool (mEfiMemoryMapSize);
    if (mEfiMemoryMap == NULL) {
      UT_LOG_ERROR ("Failed to allocate %d bytes for EFI memory map!\n", mEfiMemoryMapSize);
      return EFI_OUT_OF_RESOURCES;
    }
  }

  return EFI_SUCCESS;
}

/**
  Checks the input flat page/translation table for the input region and validates
  the attributes match the input attributes.

  @param[in]  Map                   Pointer to the PAGE_MAP struct to be parsed
  @param[in]  Address               Start address of the region
  @param[in]  Length                Length of the region
  @param[in]  RequiredAttributes    The required EFI Attributes of the region
  @param[in]  MatchAnyAttribute     If TRUE, the region must contain at least one of the
                                    required attributes. If FALSE, the region must contain
                                    all of the required attributes.
  @param[in]  AllowUnmappedRegions  If TRUE, unmapped regions are excepted from the check.
  @param[in]  LogAttributeMismatch  If TRUE, log the attribute mismatch via DEBUG().

  @retval TRUE                   The region has the required attributes
  @retval FALSE                  The region does not have the required attributes
**/
STATIC
BOOLEAN
ValidateRegionAttributes (
  IN PAGE_MAP  *Map,
  IN UINT64    Address,
  IN UINT64    Length,
  IN UINT64    RequiredAttributes,
  IN BOOLEAN   MatchAnyAttribute,
  IN BOOLEAN   AllowUnmappedRegions,
  IN BOOLEAN   LogAttributeMismatch
  )
{
  UINT64      RegionAttributes;
  UINT64      CheckedLength;
  EFI_STATUS  Status;
  BOOLEAN     AttributesMatch;

  AttributesMatch = TRUE;

  do {
    RegionAttributes = 0;
    CheckedLength    = 0;
    Status           = GetRegionAccessAttributes (
                         Map,
                         Address,
                         Length,
                         &RegionAttributes,
                         &CheckedLength
                         );

    // If the region was completely or partially matched, check the returned attributes against the
    // expected attributes
    if ((Status == EFI_SUCCESS) || (Status == EFI_NOT_FOUND)) {
      if (((!MatchAnyAttribute && ((RegionAttributes & RequiredAttributes) != RequiredAttributes)) ||
           (MatchAnyAttribute && ((RegionAttributes & RequiredAttributes) == 0))))
      {
        if (LogAttributeMismatch) {
          UT_LOG_ERROR (
            "Region 0x%llx-0x%llx does not %a%a%a%a\n",
            Address,
            Address + CheckedLength,
            MatchAnyAttribute ? "contain a superset of the following attribute(s): " : "match exactly the following attribute(s): ",
            ((RequiredAttributes & EFI_MEMORY_RP) != 0) ? "EFI_MEMORY_RP " : "",
            ((RequiredAttributes & EFI_MEMORY_RO) != 0) ? "EFI_MEMORY_RO " : "",
            ((RequiredAttributes & EFI_MEMORY_XP) != 0) ? "EFI_MEMORY_XP " : ""
            );
        }

        AttributesMatch = FALSE;
      }
    }
    // If the region was not found, check if unmapped regions are OK
    else if (Status == EFI_NO_MAPPING) {
      if (!AllowUnmappedRegions) {
        if (LogAttributeMismatch) {
          UT_LOG_ERROR (
            "Region 0x%llx-0x%llx is not mapped\n",
            Address,
            Address + CheckedLength
            );
        }

        AttributesMatch = FALSE;
      }
    }
    // If an unexpected status was returned, break out of the loop and return failure
    else {
      UT_LOG_INFO (
        "Failed to get attributes for Address: 0x%llx, Length: 0x%llx. Status: %r\n",
        Address,
        Length,
        Status
        );
      AttributesMatch = FALSE;
      break;
    }

    if (CheckedLength == 0) {
      UT_LOG_INFO (
        "Unexpected error occurred when parsing the page table for 0x%llx-0x%llx!\n",
        Address,
        Address + Length
        );

      AttributesMatch = FALSE;
      break;
    }

    if (EFI_ERROR (SafeUint64Add (Address, CheckedLength, &Address))) {
      break;
    }

    Length -= CheckedLength;
  } while (Length > 0);

  return AttributesMatch;
}

// ----------------------
//    CLEANUP FUNCTION
// ----------------------

/**
  Cleanup function for the unit test.

  @param[in] Context        Unit test context

  @retval UNIT_TEST_PASSED  Always passes
**/
STATIC
VOID
EFIAPI
GeneralTestCleanup (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  if (mSpecialRegions != NULL) {
    FreeSpecialRegions ();
  }

  if (mNonProtectedImageList != NULL) {
    FreeNonProtectedImageList ();
  }

  if (mMemorySpaceMap != NULL) {
    FreeMemorySpaceMap ();
  }
}

// ---------------------------------
//    UNIT TEST SUPPORT FUNCTIONS
// ---------------------------------

/**
  Checks if a region is allowed to be read/write/execute based on the special region array
  and non protected image list

  @param[in] Address            Start address of the region
  @param[in] Length             Length of the region

  @retval TRUE                  The region is allowed to be read/write/execute
  @retval FALSE                 The region is not allowed to be read/write/execute
**/
STATIC
BOOLEAN
CanRegionBeRWX (
  IN UINT64  Address,
  IN UINT64  Length
  )
{
  LIST_ENTRY              *NonProtectedImageLink;
  IMAGE_RANGE_DESCRIPTOR  *NonProtectedImage;
  UINTN                   SpecialRegionIndex, MemorySpaceMapIndex;

  if ((mNonProtectedImageList == NULL) && (mSpecialRegions == NULL)) {
    return FALSE;
  }

  if (mSpecialRegions != NULL) {
    for (SpecialRegionIndex = 0; SpecialRegionIndex < mSpecialRegionCount; SpecialRegionIndex++) {
      if (CHECK_SUBSUMPTION (
            mSpecialRegions[SpecialRegionIndex].Start,
            mSpecialRegions[SpecialRegionIndex].Start + mSpecialRegions[SpecialRegionIndex].Length,
            Address,
            Address + Length
            ) &&
          (mSpecialRegions[SpecialRegionIndex].EfiAttributes == 0))
      {
        return TRUE;
      }
    }
  }

  if (mNonProtectedImageList != NULL) {
    for (NonProtectedImageLink = mNonProtectedImageList->Link.ForwardLink;
         NonProtectedImageLink != &mNonProtectedImageList->Link;
         NonProtectedImageLink = NonProtectedImageLink->ForwardLink)
    {
      NonProtectedImage = CR (NonProtectedImageLink, IMAGE_RANGE_DESCRIPTOR, Link, IMAGE_RANGE_DESCRIPTOR_SIGNATURE);
      if (CHECK_SUBSUMPTION (
            NonProtectedImage->Base,
            NonProtectedImage->Base + NonProtectedImage->Length,
            Address,
            Address + Length
            ))
      {
        return TRUE;
      }
    }
  }

  if (mMemorySpaceMap != NULL) {
    for (MemorySpaceMapIndex = 0; MemorySpaceMapIndex < mMemorySpaceMapCount; MemorySpaceMapIndex++) {
      if (CHECK_SUBSUMPTION (
            mMemorySpaceMap[MemorySpaceMapIndex].BaseAddress,
            mMemorySpaceMap[MemorySpaceMapIndex].BaseAddress + mMemorySpaceMap[MemorySpaceMapIndex].Length,
            Address,
            Address + Length
            ) &&
          (mMemorySpaceMap[MemorySpaceMapIndex].GcdMemoryType == EfiGcdMemoryTypeNonExistent))
      {
        return TRUE;
      }
    }
  }

  return FALSE;
}

/**
 Locates and opens the SFS volume containing the application and, if successful, returns an
 FS handle to the opened volume.

  @param    mFs_Handle       Handle to the opened volume.

  @retval   EFI_SUCCESS     The FS volume was opened successfully.
  @retval   Others          The operation failed.

**/
STATIC
EFI_STATUS
OpenAppSFS (
  OUT EFI_FILE  **Fs_Handle
  )
{
  EFI_DEVICE_PATH_PROTOCOL         *DevicePath;
  BOOLEAN                          Found;
  EFI_HANDLE                       Handle;
  EFI_HANDLE                       *HandleBuffer;
  UINTN                            Index;
  UINTN                            NumHandles;
  EFI_STRING                       PathNameStr;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *SfProtocol;
  EFI_STATUS                       Status;
  EFI_FILE_PROTOCOL                *FileHandle;
  EFI_FILE_PROTOCOL                *FileHandle2;

  Status       = EFI_SUCCESS;
  SfProtocol   = NULL;
  NumHandles   = 0;
  HandleBuffer = NULL;

  //
  // Locate all handles that are using the SFS protocol.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &NumHandles,
                  &HandleBuffer
                  );

  if (EFI_ERROR (Status) != FALSE) {
    DEBUG ((DEBUG_ERROR, "%a: failed to locate all handles using the Simple FS protocol (%r)\n", __FUNCTION__, Status));
    goto CleanUp;
  }

  //
  // Search the handles to find one that is on a GPT partition on a hard drive.
  //
  Found = FALSE;
  for (Index = 0; (Index < NumHandles) && (Found == FALSE); Index += 1) {
    DevicePath = DevicePathFromHandle (HandleBuffer[Index]);
    if (DevicePath == NULL) {
      continue;
    }

    //
    // Convert the device path to a string to print it.
    //
    PathNameStr = ConvertDevicePathToText (DevicePath, TRUE, TRUE);
    DEBUG ((DEBUG_ERROR, "%a: device path %d -> %s\n", __FUNCTION__, Index, PathNameStr));

    //
    // Check if this is a block IO device path. If it is not, keep searching.
    // This changes our locate device path variable, so we'll have to restore
    // it afterwards.
    //
    Status = gBS->LocateDevicePath (
                    &gEfiBlockIoProtocolGuid,
                    &DevicePath,
                    &Handle
                    );

    if (EFI_ERROR (Status) != FALSE) {
      DEBUG ((DEBUG_ERROR, "%a: not a block IO device path\n", __FUNCTION__));
      continue;
    }

    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&SfProtocol
                    );

    if (EFI_ERROR (Status) != FALSE) {
      DEBUG ((DEBUG_ERROR, "%a: Failed to locate Simple FS protocol using the handle to fs0: %r \n", __FUNCTION__, Status));
      goto CleanUp;
    }

    //
    // Open the volume/partition.
    //
    Status = SfProtocol->OpenVolume (SfProtocol, &FileHandle);
    if (EFI_ERROR (Status) != FALSE) {
      DEBUG ((DEBUG_ERROR, "%a: Failed to open Simple FS volume fs0: %r \n", __FUNCTION__, Status));
      goto CleanUp;
    }

    //
    // Ensure the PktName file is present
    //
    Status = FileHandle->Open (FileHandle, &FileHandle2, L"DxePagingAuditTestApp.efi", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_INFO, "%a: Unable to locate %s. Status: %r\n", __FUNCTION__, L"DxePagingAuditTestApp.efi", Status));
      Status = FileHandleClose (FileHandle);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "%a: Error closing Vol Handle. Code = %r\n", __FUNCTION__, Status));
      }

      Status = EFI_NOT_FOUND;
      continue;
    } else {
      DEBUG ((DEBUG_ERROR, "%a: Located app device path\n", __FUNCTION__));
      Status     = FileHandleClose (FileHandle2);
      *Fs_Handle = (EFI_FILE *)FileHandle;
      break;
    }
  }

CleanUp:
  if (HandleBuffer != NULL) {
    FreePool (HandleBuffer);
  }

  return Status;
}

// -------------------------
//    UNIT TEST FUNCTIONS
// -------------------------

/**
  Checks if the page/translation table has any read/write/execute
  regions.

  @param[in] Context            Unit test context

  @retval UNIT_TEST_PASSED      The unit test passed
  @retval other                 The unit test failed

**/
UNIT_TEST_STATUS
EFIAPI
NoReadWriteExecute (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  UINTN    Index;
  BOOLEAN  TestFailure;

  DEBUG ((DEBUG_INFO, "%a Enter...\n", __FUNCTION__));

  PopulateSpecialRegions ();
  PopulateNonProtectedImageList ();
  UT_ASSERT_NOT_EFI_ERROR (ValidatePageTableMapSize ());
  UT_ASSERT_NOT_EFI_ERROR (PopulateMemorySpaceMap ());
  UT_ASSERT_NOT_NULL (mMemorySpaceMap);
  UT_ASSERT_NOT_EFI_ERROR (PopulatePageTableMap ());

  Index       = 0;
  TestFailure = FALSE;

  for ( ; Index < mMap.EntryCount; Index++) {
    if (IsPageExecutable (mMap.Entries[Index].PageEntry) &&
        IsPageReadable (mMap.Entries[Index].PageEntry) &&
        IsPageWritable (mMap.Entries[Index].PageEntry))
    {
      if (!CanRegionBeRWX (mMap.Entries[Index].LinearAddress, mMap.Entries[Index].Length)) {
        UT_LOG_ERROR (
          "Memory Range 0x%llx-0x%llx is Read/Write/Execute\n",
          mMap.Entries[Index].LinearAddress,
          mMap.Entries[Index].LinearAddress + mMap.Entries[Index].Length
          );
        TestFailure = TRUE;
      }
    }
  }

  UT_ASSERT_FALSE (TestFailure);

  return UNIT_TEST_PASSED;
}

/**
  Checks that EfiConventionalMemory is EFI_MEMORY_RP or
  is not mapped.

  @param[in] Context            Unit test context

  @retval UNIT_TEST_PASSED      The unit test passed
  @retval other                 The unit test failed
**/
UNIT_TEST_STATUS
EFIAPI
UnallocatedMemoryIsRP (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  BOOLEAN                TestFailure;
  EFI_MEMORY_DESCRIPTOR  *EfiMemoryMapEntry;
  EFI_MEMORY_DESCRIPTOR  *EfiMemoryMapEnd;

  DEBUG ((DEBUG_INFO, "%a Enter...\n", __FUNCTION__));

  UT_ASSERT_NOT_EFI_ERROR (ValidatePageTableMapSize ());
  UT_ASSERT_NOT_EFI_ERROR (ValidateEfiMemoryMapSize ());
  UT_ASSERT_NOT_EFI_ERROR (PopulateEfiMemoryMap ());
  UT_ASSERT_NOT_EFI_ERROR (PopulatePageTableMap ());

  TestFailure = FALSE;

  EfiMemoryMapEntry = mEfiMemoryMap;
  EfiMemoryMapEnd   = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mEfiMemoryMap + mEfiMemoryMapSize);

  while (EfiMemoryMapEntry < EfiMemoryMapEnd) {
    if (EfiMemoryMapEntry->Type == EfiConventionalMemory) {
      if (!ValidateRegionAttributes (
             &mMap,
             EfiMemoryMapEntry->PhysicalStart,
             (EfiMemoryMapEntry->NumberOfPages * EFI_PAGE_SIZE),
             EFI_MEMORY_RP,
             TRUE,
             TRUE,
             TRUE
             ))
      {
        TestFailure = TRUE;
      }
    }

    EfiMemoryMapEntry = NEXT_MEMORY_DESCRIPTOR (EfiMemoryMapEntry, mEfiMemoryMapDescriptorSize);
  }

  UT_ASSERT_FALSE (TestFailure);

  return UNIT_TEST_PASSED;
}

/**
  Checks if the EFI Memory Attribute Protocol is Present.

  @param[in] Context            Unit test context

  @retval UNIT_TEST_PASSED      The unit test passed
  @retval other                 The unit test failed
**/
UNIT_TEST_STATUS
EFIAPI
IsMemoryAttributeProtocolPresent (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  EFI_STATUS                     Status;
  EFI_MEMORY_ATTRIBUTE_PROTOCOL  *MemoryAttribute;

  DEBUG ((DEBUG_INFO, "%a Enter...\n", __FUNCTION__));

  Status = gBS->LocateProtocol (
                  &gEfiMemoryAttributeProtocolGuid,
                  NULL,
                  (VOID **)&MemoryAttribute
                  );

  UT_ASSERT_NOT_EFI_ERROR (Status);

  return UNIT_TEST_PASSED;
}

/**
  Checks that the NULL page is not mapped or is
  EFI_MEMORY_RP.

  @param[in] Context            Unit test context

  @retval UNIT_TEST_PASSED      The unit test passed
  @retval other                 The unit test failed
**/
STATIC
UNIT_TEST_STATUS
EFIAPI
NullPageIsRp (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  DEBUG ((DEBUG_INFO, "%a Enter...\n", __FUNCTION__));
  UT_ASSERT_NOT_EFI_ERROR (ValidatePageTableMapSize ());
  UT_ASSERT_NOT_EFI_ERROR (PopulatePageTableMap ());

  UT_ASSERT_TRUE (
    ValidateRegionAttributes (
      &mMap,
      0,
      EFI_PAGE_SIZE,
      EFI_MEMORY_RP,
      TRUE,
      TRUE,
      TRUE
      )
    );

  return UNIT_TEST_PASSED;
}

/**
  Checks that MMIO regions in the EFI memory map are
  EFI_MEMORY_XP.

  @param[in] Context            Unit test context

  @retval UNIT_TEST_PASSED      The unit test passed
  @retval other                 The unit test failed
**/
STATIC
UNIT_TEST_STATUS
EFIAPI
MmioIsXp (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  EFI_MEMORY_DESCRIPTOR  *EfiMemoryMapEntry;
  EFI_MEMORY_DESCRIPTOR  *EfiMemoryMapEnd;
  BOOLEAN                TestFailure;
  UINTN                  Index;

  DEBUG ((DEBUG_INFO, "%a Enter...\n", __FUNCTION__));
  UT_ASSERT_NOT_EFI_ERROR (ValidatePageTableMapSize ());
  UT_ASSERT_NOT_EFI_ERROR (ValidateEfiMemoryMapSize ());
  UT_ASSERT_NOT_EFI_ERROR (PopulateMemorySpaceMap ());
  UT_ASSERT_NOT_NULL (mMemorySpaceMap);
  UT_ASSERT_NOT_EFI_ERROR (PopulateEfiMemoryMap ());
  UT_ASSERT_NOT_EFI_ERROR (PopulatePageTableMap ());

  TestFailure = FALSE;

  EfiMemoryMapEntry = mEfiMemoryMap;
  EfiMemoryMapEnd   = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mEfiMemoryMap + mEfiMemoryMapSize);

  while (EfiMemoryMapEntry < EfiMemoryMapEnd) {
    if (EfiMemoryMapEntry->Type == EfiMemoryMappedIO) {
      if (!ValidateRegionAttributes (
             &mMap,
             EfiMemoryMapEntry->PhysicalStart,
             (EfiMemoryMapEntry->NumberOfPages * EFI_PAGE_SIZE),
             EFI_MEMORY_XP | EFI_MEMORY_RP,
             TRUE,
             TRUE,
             TRUE
             ))
      {
        TestFailure = TRUE;
      }
    }

    EfiMemoryMapEntry = NEXT_MEMORY_DESCRIPTOR (EfiMemoryMapEntry, mEfiMemoryMapDescriptorSize);
  }

  for (Index = 0; Index < mMemorySpaceMapCount; Index++) {
    if (mMemorySpaceMap[Index].GcdMemoryType == EfiGcdMemoryTypeMemoryMappedIo) {
      if (!ValidateRegionAttributes (
             &mMap,
             mMemorySpaceMap[Index].BaseAddress,
             mMemorySpaceMap[Index].Length,
             EFI_MEMORY_XP | EFI_MEMORY_RP,
             TRUE,
             TRUE,
             TRUE
             ))
      {
        TestFailure = TRUE;
      }
    }
  }

  UT_ASSERT_FALSE (TestFailure);

  return UNIT_TEST_PASSED;
}

/**
  Checks that loaded image sections containing code
  are EFI_MEMORY_RO and sections containing data
  are EFI_MEMORY_XP.

  @param[in] Context            Unit test context

  @retval UNIT_TEST_PASSED      The unit test passed
  @retval other                 The unit test failed
**/
STATIC
UNIT_TEST_STATUS
EFIAPI
ImageCodeSectionsRoDataSectionsXp (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  EFI_STATUS                           Status;
  UINTN                                NoHandles;
  EFI_HANDLE                           *HandleBuffer;
  UINTN                                Index;
  UINTN                                Index2;
  EFI_LOADED_IMAGE_PROTOCOL            *LoadedImage;
  EFI_IMAGE_DOS_HEADER                 *DosHdr;
  UINT32                               PeCoffHeaderOffset;
  EFI_IMAGE_OPTIONAL_HEADER_PTR_UNION  Hdr;
  UINT32                               SectionAlignment;
  EFI_IMAGE_SECTION_HEADER             *Section;
  BOOLEAN                              TestFailure;
  UINT64                               SectionStart;
  UINT64                               SectionEnd;
  CHAR8                                *PdbFileName;

  DEBUG ((DEBUG_INFO, "%a Enter...\n", __FUNCTION__));

  UT_ASSERT_NOT_EFI_ERROR (ValidatePageTableMapSize ());
  UT_ASSERT_NOT_EFI_ERROR (PopulatePageTableMap ());

  TestFailure = FALSE;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiLoadedImageProtocolGuid,
                  NULL,
                  &NoHandles,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status) && (NoHandles == 0)) {
    UT_LOG_ERROR ("Unable to query EFI Loaded Image Protocol\n");
    UT_ASSERT_NOT_EFI_ERROR (Status);
    UT_ASSERT_NOT_EQUAL (NoHandles, 0);
  }

  for (Index = 0; Index < NoHandles; Index++) {
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiLoadedImageProtocolGuid,
                    (VOID **)&LoadedImage
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    // Check PE/COFF image
    PdbFileName = PeCoffLoaderGetPdbPointer (LoadedImage->ImageBase);
    if (PdbFileName == NULL) {
      DEBUG ((
        DEBUG_WARN,
        "%a Could not get name of image loaded at 0x%llx - 0x%llx...\n",
        __func__,
        (UINTN)LoadedImage->ImageBase,
        (UINTN)LoadedImage->ImageBase + LoadedImage->ImageSize
        ));
    }

    DosHdr             = (EFI_IMAGE_DOS_HEADER *)(UINTN)LoadedImage->ImageBase;
    PeCoffHeaderOffset = 0;
    if (DosHdr->e_magic == EFI_IMAGE_DOS_SIGNATURE) {
      PeCoffHeaderOffset = DosHdr->e_lfanew;
    }

    Hdr.Pe32 = (EFI_IMAGE_NT_HEADERS32 *)((UINT8 *)(UINTN)LoadedImage->ImageBase + PeCoffHeaderOffset);

    // Get SectionAlignment
    if (Hdr.Pe32->OptionalHeader.Magic == EFI_IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
      SectionAlignment = Hdr.Pe32->OptionalHeader.SectionAlignment;
    } else {
      SectionAlignment = Hdr.Pe32Plus->OptionalHeader.SectionAlignment;
    }

    if (!IsLoadedImageSectionAligned (SectionAlignment, LoadedImage->ImageCodeType)) {
      UT_LOG_ERROR (
        "Image %a: 0x%llx - 0x%llx is not aligned\n",
        PdbFileName,
        (UINTN)LoadedImage->ImageBase,
        (UINTN)LoadedImage->ImageBase + LoadedImage->ImageSize
        );
      TestFailure = TRUE;
      continue;
    }

    Section = (EFI_IMAGE_SECTION_HEADER *)(
                                           (UINT8 *)(UINTN)LoadedImage->ImageBase +
                                           PeCoffHeaderOffset +
                                           sizeof (UINT32) +
                                           sizeof (EFI_IMAGE_FILE_HEADER) +
                                           Hdr.Pe32->FileHeader.SizeOfOptionalHeader
                                           );

    for (Index2 = 0; Index2 < Hdr.Pe32->FileHeader.NumberOfSections; Index2++) {
      SectionStart = (UINT64)LoadedImage->ImageBase + Section[Index2].VirtualAddress;
      SectionEnd   = SectionStart + ALIGN_VALUE (Section[Index2].SizeOfRawData, SectionAlignment);

      // Check if the section contains code and data
      if (((Section[Index2].Characteristics & EFI_IMAGE_SCN_CNT_CODE) != 0) &&
          ((Section[Index2].Characteristics &
            (EFI_IMAGE_SCN_CNT_INITIALIZED_DATA | EFI_IMAGE_SCN_CNT_UNINITIALIZED_DATA)) != 0))
      {
        UT_LOG_ERROR (
          "Image %a: Section 0x%llx-0x%llx contains code and data\n",
          PdbFileName,
          SectionStart,
          SectionEnd
          );
        TestFailure = TRUE;
      } else if ((Section[Index2].Characteristics &
                  (EFI_IMAGE_SCN_MEM_WRITE | EFI_IMAGE_SCN_MEM_EXECUTE)) == EFI_IMAGE_SCN_MEM_EXECUTE)
      {
        if (!ValidateRegionAttributes (
               &mMap,
               SectionStart,
               SectionEnd - SectionStart,
               EFI_MEMORY_RO,
               FALSE,
               FALSE,
               FALSE
               ))
        {
          UT_LOG_ERROR (
            "Image %a: Section 0x%llx-0x%llx is not EFI_MEMORY_RO\n",
            PdbFileName,
            SectionStart,
            SectionEnd
            );
          TestFailure = TRUE;
        }
      } else {
        if (!ValidateRegionAttributes (
               &mMap,
               SectionStart,
               SectionEnd - SectionStart,
               EFI_MEMORY_XP,
               FALSE,
               FALSE,
               FALSE
               ))
        {
          UT_LOG_ERROR (
            "Image %a: Section 0x%llx-0x%llx is not EFI_MEMORY_XP\n",
            PdbFileName,
            SectionStart,
            SectionEnd
            );
          TestFailure = TRUE;
        }
      }
    }
  }

  FreePool (HandleBuffer);

  UT_ASSERT_FALSE (TestFailure);

  return UNIT_TEST_PASSED;
}

/**
  Checks that the stack is EFI_MEMORY_XP and has
  an EFI_MEMORY_RP page to catch overflow.

  @param[in] Context            Unit test context

  @retval UNIT_TEST_PASSED      The unit test passed
  @retval other                 The unit test failed
**/
STATIC
UNIT_TEST_STATUS
EFIAPI
BspStackIsXpAndHasGuardPage (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  EFI_PEI_HOB_POINTERS       Hob;
  EFI_HOB_MEMORY_ALLOCATION  *MemoryHob;
  BOOLEAN                    TestFailure;
  EFI_PHYSICAL_ADDRESS       StackBase;
  UINT64                     StackLength;

  DEBUG ((DEBUG_INFO, "%a Enter...\n", __FUNCTION__));

  UT_ASSERT_NOT_EFI_ERROR (ValidatePageTableMapSize ());
  UT_ASSERT_NOT_EFI_ERROR (PopulatePageTableMap ());

  Hob.Raw     = GetHobList ();
  TestFailure = FALSE;

  while ((Hob.Raw = GetNextHob (EFI_HOB_TYPE_MEMORY_ALLOCATION, Hob.Raw)) != NULL) {
    MemoryHob = Hob.MemoryAllocation;
    if (CompareGuid (&gEfiHobMemoryAllocStackGuid, &MemoryHob->AllocDescriptor.Name)) {
      StackBase   = (EFI_PHYSICAL_ADDRESS)((MemoryHob->AllocDescriptor.MemoryBaseAddress / EFI_PAGE_SIZE) * EFI_PAGE_SIZE);
      StackLength = (EFI_PHYSICAL_ADDRESS)(EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (MemoryHob->AllocDescriptor.MemoryLength)));

      UT_LOG_INFO ("BSP stack located at 0x%llx - 0x%llx\n", StackBase, StackBase + StackLength);

      if (!ValidateRegionAttributes (
             &mMap,
             StackBase,
             EFI_PAGE_SIZE,
             EFI_MEMORY_RP,
             TRUE,
             TRUE,
             FALSE
             ))
      {
        UT_LOG_ERROR (
          "Stack 0x%llx-0x%llx does not have an EFI_MEMORY_RP page to catch overflow\n",
          StackBase,
          StackBase + EFI_PAGE_SIZE
          );
        TestFailure = TRUE;
      }

      if (!ValidateRegionAttributes (
             &mMap,
             StackBase + EFI_PAGE_SIZE,
             StackLength - EFI_PAGE_SIZE,
             EFI_MEMORY_XP,
             TRUE,
             FALSE,
             FALSE
             ))
      {
        UT_LOG_ERROR (
          "Stack 0x%llx-0x%llx is not EFI_MEMORY_XP\n",
          StackBase + EFI_PAGE_SIZE,
          StackBase + StackLength
          );
        TestFailure = TRUE;
      }

      break;
    }

    Hob.Raw = GET_NEXT_HOB (Hob);
  }

  UT_ASSERT_FALSE (TestFailure);

  return UNIT_TEST_PASSED;
}

/**
  Checks that memory ranges not in the EFI
  memory map will cause a CPU fault if accessed.

  @param[in] Context            Unit test context

  @retval UNIT_TEST_PASSED      The unit test passed
  @retval other                 The unit test failed
**/
STATIC
UNIT_TEST_STATUS
EFIAPI
MemoryOutsideEfiMemoryMapIsInaccessible (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  UINT64                 StartOfAddressSpace;
  UINT64                 EndOfAddressSpace;
  EFI_MEMORY_DESCRIPTOR  *EndOfEfiMemoryMap;
  EFI_MEMORY_DESCRIPTOR  *CurrentEfiMemoryMapEntry;
  BOOLEAN                TestFailure;
  EFI_PHYSICAL_ADDRESS   LastMemoryMapEntryEnd;

  DEBUG ((DEBUG_INFO, "%a Enter...\n", __FUNCTION__));

  UT_ASSERT_NOT_EFI_ERROR (ValidatePageTableMapSize ());
  UT_ASSERT_NOT_EFI_ERROR (ValidateEfiMemoryMapSize ());
  UT_ASSERT_NOT_EFI_ERROR (PopulateMemorySpaceMap ());
  UT_ASSERT_NOT_NULL (mMemorySpaceMap);
  UT_ASSERT_NOT_EFI_ERROR (PopulateEfiMemoryMap ());
  UT_ASSERT_NOT_EFI_ERROR (PopulatePageTableMap ());

  StartOfAddressSpace = mMemorySpaceMap[0].BaseAddress;
  EndOfAddressSpace   = mMemorySpaceMap[mMemorySpaceMapCount - 1].BaseAddress +
                        mMemorySpaceMap[mMemorySpaceMapCount - 1].Length;
  TestFailure              = FALSE;
  EndOfEfiMemoryMap        = (EFI_MEMORY_DESCRIPTOR *)(((UINT8 *)mEfiMemoryMap + mEfiMemoryMapSize));
  CurrentEfiMemoryMapEntry = mEfiMemoryMap;

  if (CurrentEfiMemoryMapEntry->PhysicalStart > StartOfAddressSpace) {
    if (!ValidateRegionAttributes (
           &mMap,
           StartOfAddressSpace,
           CurrentEfiMemoryMapEntry->PhysicalStart - StartOfAddressSpace,
           EFI_MEMORY_RP,
           TRUE,
           TRUE,
           TRUE
           ))
    {
      TestFailure = TRUE;
    }
  }

  LastMemoryMapEntryEnd = CurrentEfiMemoryMapEntry->PhysicalStart +
                          (CurrentEfiMemoryMapEntry->NumberOfPages * EFI_PAGE_SIZE);
  CurrentEfiMemoryMapEntry = NEXT_MEMORY_DESCRIPTOR (CurrentEfiMemoryMapEntry, mEfiMemoryMapDescriptorSize);

  while ((UINTN)CurrentEfiMemoryMapEntry < (UINTN)EndOfEfiMemoryMap) {
    if (CurrentEfiMemoryMapEntry->PhysicalStart > LastMemoryMapEntryEnd) {
      if (!ValidateRegionAttributes (
             &mMap,
             LastMemoryMapEntryEnd,
             CurrentEfiMemoryMapEntry->PhysicalStart - LastMemoryMapEntryEnd,
             EFI_MEMORY_RP,
             TRUE,
             TRUE,
             TRUE
             ))
      {
        TestFailure = TRUE;
      }
    }

    LastMemoryMapEntryEnd = CurrentEfiMemoryMapEntry->PhysicalStart +
                            (CurrentEfiMemoryMapEntry->NumberOfPages * EFI_PAGE_SIZE);
    CurrentEfiMemoryMapEntry = NEXT_MEMORY_DESCRIPTOR (CurrentEfiMemoryMapEntry, mEfiMemoryMapDescriptorSize);
  }

  if (LastMemoryMapEntryEnd < EndOfAddressSpace) {
    if (!ValidateRegionAttributes (
           &mMap,
           LastMemoryMapEntryEnd,
           EndOfAddressSpace - LastMemoryMapEntryEnd,
           EFI_MEMORY_RP,
           TRUE,
           TRUE,
           TRUE
           ))
    {
      TestFailure = TRUE;
    }
  }

  UT_ASSERT_FALSE (TestFailure);

  return UNIT_TEST_PASSED;
}

/**
  Entry Point of the shell app.

  @param[in] ImageHandle  The firmware allocated handle for the EFI image.
  @param[in] SystemTable  A pointer to the EFI System Table.

  @retval EFI_SUCCESS     The entry point executed successfully.
  @retval other           Some error occurred when executing this entry point.

**/
EFI_STATUS
EFIAPI
DxePagingAuditTestAppEntryPoint (
  IN     EFI_HANDLE        ImageHandle,
  IN     EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                     Status;
  UNIT_TEST_FRAMEWORK_HANDLE     Fw       = NULL;
  UNIT_TEST_SUITE_HANDLE         Misc     = NULL;
  BOOLEAN                        RunTests = TRUE;
  EFI_SHELL_PARAMETERS_PROTOCOL  *ShellParams;
  EFI_FILE                       *Fs_Handle;

  DEBUG ((DEBUG_ERROR, "%a()\n", __FUNCTION__));
  DEBUG ((DEBUG_ERROR, "%a v%a\n", UNIT_TEST_APP_NAME, UNIT_TEST_APP_VERSION));

  Status = gBS->HandleProtocol (
                  gImageHandle,
                  &gEfiShellParametersProtocolGuid,
                  (VOID **)&ShellParams
                  );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a Could not retrieve command line args!\n", __FUNCTION__));
    return EFI_PROTOCOL_ERROR;
  }

  if (ShellParams->Argc > 1) {
    RunTests = FALSE;
    if (StrnCmp (ShellParams->Argv[1], L"-r", MAX_CHARS_TO_READ) == 0) {
      RunTests = TRUE;
    } else if (StrnCmp (ShellParams->Argv[1], L"-d", MAX_CHARS_TO_READ) == 0) {
      Status = OpenAppSFS (&Fs_Handle);

      if (!EFI_ERROR ((Status))) {
        DumpPagingInfo (Fs_Handle);
      } else {
        DumpPagingInfo (NULL);
      }
    } else {
      if (StrnCmp (ShellParams->Argv[1], L"-h", MAX_CHARS_TO_READ) != 0) {
        DEBUG ((DEBUG_ERROR, "Invalid argument!\n"));
      }

      Print (L"-h : Print available flags\n");
      Print (L"-d : Dump the page table files\n");
      Print (L"-r : Run the application tests\n");
      Print (L"NOTE: Combined flags (i.e. -rd) is not supported\n");
    }
  }

  if (RunTests) {
    //
    // Start setting up the test framework for running the tests.
    //
    Status = InitUnitTestFramework (&Fw, UNIT_TEST_APP_NAME, gEfiCallerBaseName, UNIT_TEST_APP_VERSION);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failed in InitUnitTestFramework. Status = %r\n", Status));
      goto EXIT;
    }

    //
    // Create test suite
    //
    CreateUnitTestSuite (&Misc, Fw, "Miscellaneous tests", "Security.Misc", NULL, NULL);

    if (Misc == NULL) {
      DEBUG ((DEBUG_ERROR, "Failed in CreateUnitTestSuite for TestSuite\n"));
      goto EXIT;
    }

    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a - Unable to fetch the GCD memory map. Test results may be inaccurate. Status: %r\n", __FUNCTION__, Status));
    }

    AddTestCase (Misc, "No pages are  readable, writable, and executable", "Security.Misc.NoReadWriteExecute", NoReadWriteExecute, NULL, GeneralTestCleanup, NULL);
    AddTestCase (Misc, "Unallocated memory is EFI_MEMORY_RP", "Security.Misc.UnallocatedMemoryIsRP", UnallocatedMemoryIsRP, NULL, GeneralTestCleanup, NULL);
    AddTestCase (Misc, "Memory Attribute Protocol is present", "Security.Misc.IsMemoryAttributeProtocolPresent", IsMemoryAttributeProtocolPresent, NULL, NULL, NULL);
    AddTestCase (Misc, "NULL page is EFI_MEMORY_RP", "Security.Misc.NullPageIsRp", NullPageIsRp, NULL, GeneralTestCleanup, NULL);
    AddTestCase (Misc, "MMIO Regions are EFI_MEMORY_XP", "Security.Misc.MmioIsXp", MmioIsXp, NULL, GeneralTestCleanup, NULL);
    AddTestCase (Misc, "Image code sections are EFI_MEMORY_RO and and data sections are EFI_MEMORY_XP", "Security.Misc.ImageCodeSectionsRoDataSectionsXp", ImageCodeSectionsRoDataSectionsXp, NULL, GeneralTestCleanup, NULL);
    AddTestCase (Misc, "BSP stack is EFI_MEMORY_XP and has EFI_MEMORY_RP guard page", "Security.Misc.BspStackIsXpAndHasGuardPage", BspStackIsXpAndHasGuardPage, NULL, GeneralTestCleanup, NULL);
    AddTestCase (Misc, "Memory outside of the EFI Memory Map is inaccessible", "Security.Misc.MemoryOutsideEfiMemoryMapIsInaccessible", MemoryOutsideEfiMemoryMapIsInaccessible, NULL, GeneralTestCleanup, NULL);

    //
    // Execute the tests.
    //
    Status = RunAllTestSuites (Fw);
  }

EXIT:
  if (Fw != NULL) {
    FreeUnitTestFramework (Fw);
  }

  if (mMap.Entries != NULL) {
    FreePageTableMap ();
  }

  if (mEfiMemoryMap != NULL) {
    FreeEfiMemoryMap ();
  }

  return EFI_SUCCESS;
}
