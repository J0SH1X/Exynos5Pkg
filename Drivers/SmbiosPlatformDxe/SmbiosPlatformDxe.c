/** @file
  SMBIOS records for v1awifi (Galaxy Note Pro 12.2 Wi-Fi, SM-P900):
  BIOS (0), system (1), board (2), chassis (3), processor (4) with its
  caches (7), memory (16/17/19) and boot status (32).

  The processor record describes what Windows runs on: the Cortex-A15
  cluster of the Exynos5420 (4 cores; the A7 cluster stays off).  The
  processor ID is the MIDR, ProcessorFamily2 ARMv7, as ArmPkg's
  ProcessorSubClassDxe does it - that driver is not used because it probes
  SMCCC with an SMC, which Samsung's secure monitor is not known to answer.

  Records go in at EndOfDxe, after CpuFreqDxe has set the clock, so the
  current speed is the one the OS gets.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>
#include <IndustryStandard/SmBios.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/Smbios.h>

#define CPU_CORES          4
#define CPU_MAX_MHZ        1900          // Exynos5420 A15 cluster rating
#define CPU_EXT_CLOCK_MHZ  24            // XXTI
#define RAM_MB             3072          // 3 GB LPDDR3 (PoP)

//
// CMU_CPU, for the current A15 clock (CpuFreqDxe).
//
#define APLL_CON0          0x10010100
#define CLK_MUX_STAT_CPU   0x10010400
#define CLK_DIV_CPU0       0x10010500

STATIC EFI_SMBIOS_PROTOCOL  *mSmbios;

/**
  Add one record: the formatted area (its Hdr.Length = Length), then the
  string set (1-based in the order given; NULL entries are skipped and must
  be referenced as 0).
**/
STATIC
EFI_SMBIOS_HANDLE
AddRecord (
  IN VOID          *Record,
  IN UINTN         Length,
  IN CONST CHAR8   **Strings,
  IN UINTN         Count
  )
{
  SMBIOS_STRUCTURE   *Hdr;
  UINT8              *Buffer;
  UINTN              Size;
  UINTN              Index;
  UINTN              Offset;
  EFI_SMBIOS_HANDLE  Handle;
  EFI_STATUS         Status;

  Size = Length + 1;
  for (Index = 0; Index < Count; Index++) {
    if (Strings[Index] != NULL) {
      Size += AsciiStrSize (Strings[Index]);
    }
  }
  if (Size == Length + 1) {
    Size++;                   // no strings: two NULs
  }

  Buffer = AllocateZeroPool (Size);
  if (Buffer == NULL) {
    return SMBIOS_HANDLE_PI_RESERVED;
  }
  CopyMem (Buffer, Record, Length);
  Hdr         = (SMBIOS_STRUCTURE *)Buffer;
  Hdr->Length = (UINT8)Length;
  Hdr->Handle = SMBIOS_HANDLE_PI_RESERVED;

  Offset = Length;
  for (Index = 0; Index < Count; Index++) {
    if (Strings[Index] != NULL) {
      CopyMem (Buffer + Offset, Strings[Index], AsciiStrSize (Strings[Index]));
      Offset += AsciiStrSize (Strings[Index]);
    }
  }

  Handle = SMBIOS_HANDLE_PI_RESERVED;
  Status = mSmbios->Add (mSmbios, NULL, &Handle, (EFI_SMBIOS_TABLE_HEADER *)Buffer);
  FreePool (Buffer);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smbios: type %u not added - %r\n", Hdr->Type, Status));
    return SMBIOS_HANDLE_PI_RESERVED;
  }
  return Handle;
}

STATIC
UINT16
CurrentArmMhz (
  VOID
  )
{
  UINT32  Con;
  UINT32  Div;

  if (((MmioRead32 (CLK_MUX_STAT_CPU) >> 16) & 0x7) != 1) {
    return 0;                 // not on the APLL: unknown
  }
  Con = MmioRead32 (APLL_CON0);
  Div = MmioRead32 (CLK_DIV_CPU0);
  if (((Con >> 8) & 0x3F) == 0) {
    return 0;
  }
  return (UINT16)((24u * ((Con >> 16) & 0x3FF)) / (((Con >> 8) & 0x3F) << (Con & 0x7))
                  / ((Div & 0x7) + 1) / (((Div >> 28) & 0x7) + 1));
}

/**
  __DATE__ ("Mmm dd yyyy") -> "mm/dd/yyyy" for the BIOS release date.
**/
STATIC
VOID
BuildDate (
  OUT CHAR8  Date[11]
  )
{
  STATIC CONST CHAR8  Months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  CONST CHAR8         *Build   = __DATE__;
  UINTN               Month;

  for (Month = 0; Month < 12; Month++) {
    if (CompareMem (Build, &Months[Month * 3], 3) == 0) {
      break;
    }
  }
  Month++;
  Date[0]  = (CHAR8)('0' + Month / 10);
  Date[1]  = (CHAR8)('0' + Month % 10);
  Date[2]  = '/';
  Date[3]  = (Build[4] == ' ') ? '0' : Build[4];
  Date[4]  = Build[5];
  Date[5]  = '/';
  CopyMem (&Date[6], &Build[7], 4);
  Date[10] = '\0';
}

STATIC
VOID
AddBiosSystemChassis (
  VOID
  )
{
  SMBIOS_TABLE_TYPE0  T0;
  SMBIOS_TABLE_TYPE1  T1;
  SMBIOS_TABLE_TYPE2  T2;
  SMBIOS_TABLE_TYPE3  T3;
  CHAR8               Version[32];
  CHAR8               Date[11];
  CONST CHAR8         *S[5];

  UnicodeStrToAsciiStrS ((CHAR16 *)PcdGetPtr (PcdFirmwareVersionString), Version, sizeof (Version));
  BuildDate (Date);

  ZeroMem (&T0, sizeof (T0));
  T0.Hdr.Type                = EFI_SMBIOS_TYPE_BIOS_INFORMATION;
  T0.Vendor                  = 1;
  T0.BiosVersion             = 2;
  T0.BiosReleaseDate         = 3;
  T0.BiosSize                = 0x0F;                 // 1 MB
  *(UINT64 *)&T0.BiosCharacteristics = BIT3;         // characteristics not supported
  T0.BIOSCharacteristicsExtensionBytes[1] = BIT3;    // UEFI
  T0.SystemBiosMajorRelease  = 0;
  T0.SystemBiosMinorRelease  = 1;
  T0.EmbeddedControllerFirmwareMajorRelease = 0xFF;
  T0.EmbeddedControllerFirmwareMinorRelease = 0xFF;
  S[0] = "Exynos5420Pkg (EDK2)";
  S[1] = Version;
  S[2] = Date;
  AddRecord (&T0, OFFSET_OF (SMBIOS_TABLE_TYPE0, ExtendedBiosSize), S, 3);

  ZeroMem (&T1, sizeof (T1));
  T1.Hdr.Type     = EFI_SMBIOS_TYPE_SYSTEM_INFORMATION;
  T1.Manufacturer = 1;
  T1.ProductName  = 2;
  T1.Version      = 3;
  T1.SKUNumber    = 4;
  T1.Family       = 5;
  T1.WakeUpType   = SystemWakeupTypePowerSwitch;
  S[0] = "Samsung Electronics";
  S[1] = "Galaxy Note Pro 12.2 Wi-Fi";
  S[2] = "SM-P900";
  S[3] = "SM-P900";
  S[4] = "Galaxy Note Pro";
  AddRecord (&T1, sizeof (T1), S, 5);

  ZeroMem (&T2, sizeof (T2));
  T2.Hdr.Type     = EFI_SMBIOS_TYPE_BASEBOARD_INFORMATION;
  T2.Manufacturer = 1;
  T2.ProductName  = 2;
  T2.FeatureFlag.Motherboard = 1;
  T2.ChassisHandle = 0xFFFF;
  T2.BoardType    = BaseBoardTypeMotherBoard;
  S[0] = "Samsung Electronics";
  S[1] = "v1awifi";
  AddRecord (&T2, OFFSET_OF (SMBIOS_TABLE_TYPE2, ContainedObjectHandles), S, 2);

  ZeroMem (&T3, sizeof (T3));
  T3.Hdr.Type         = EFI_SMBIOS_TYPE_SYSTEM_ENCLOSURE;
  T3.Manufacturer     = 1;
  T3.Type             = MiscChassisTablet;
  T3.BootupState      = ChassisStateSafe;
  T3.PowerSupplyState = ChassisStateSafe;
  T3.ThermalState     = ChassisStateSafe;
  T3.SecurityStatus   = ChassisSecurityStatusNone;
  S[0] = "Samsung Electronics";
  AddRecord (&T3, OFFSET_OF (SMBIOS_TABLE_TYPE3, ContainedElements), S, 1);
}

STATIC
EFI_SMBIOS_HANDLE
AddCache (
  IN CONST CHAR8  *Name,
  IN UINT8        Level,
  IN UINT16       SizeKb,
  IN UINT8        Type,
  IN UINT8        Associativity
  )
{
  SMBIOS_TABLE_TYPE7  T7;

  ZeroMem (&T7, sizeof (T7));
  T7.Hdr.Type            = EFI_SMBIOS_TYPE_CACHE_INFORMATION;
  T7.SocketDesignation   = 1;
  //
  // Level - 1, internal, enabled, write-back.
  //
  T7.CacheConfiguration  = (UINT16)((Level - 1) | BIT7 | (1u << 8));
  T7.MaximumCacheSize    = SizeKb;
  T7.InstalledSize       = SizeKb;
  T7.SupportedSRAMType.Synchronous = 1;
  T7.CurrentSRAMType.Synchronous   = 1;
  T7.ErrorCorrectionType = CacheErrorUnknown;
  T7.SystemCacheType     = Type;
  T7.Associativity       = Associativity;
  return AddRecord (&T7, OFFSET_OF (SMBIOS_TABLE_TYPE7, MaximumCacheSize2), &Name, 1);
}

STATIC
VOID
AddProcessor (
  VOID
  )
{
  SMBIOS_TABLE_TYPE4  T4;
  EFI_SMBIOS_HANDLE   L1d;
  EFI_SMBIOS_HANDLE   L2;
  CONST CHAR8         *S[4];
  UINT32              Midr;

  //
  // Cortex-A15: 32 KB 2-way L1 I and D per core, 2 MB 16-way L2 per
  // cluster.  Sizes are totals for the 4 cores.
  //
  AddCache ("L1 Instruction", 1, 32 * CPU_CORES, CacheTypeInstruction, CacheAssociativity2Way);
  L1d = AddCache ("L1 Data", 1, 32 * CPU_CORES, CacheTypeData, CacheAssociativity2Way);
  L2  = AddCache ("L2", 2, 2048, CacheTypeUnified, CacheAssociativity16Way);

  Midr = (UINT32)ArmReadMidr ();

  ZeroMem (&T4, sizeof (T4));
  T4.Hdr.Type              = EFI_SMBIOS_TYPE_PROCESSOR_INFORMATION;
  T4.Socket                = 1;
  T4.ProcessorType         = CentralProcessor;
  T4.ProcessorFamily       = ProcessorFamilyIndicatorFamily2;
  T4.ProcessorFamily2      = ProcessorFamilyARMv7;
  T4.ProcessorManufacturer = 2;
  CopyMem (&T4.ProcessorId, &Midr, sizeof (Midr));
  T4.ProcessorVersion      = 3;
  T4.PartNumber            = 4;
  T4.ExternalClock         = CPU_EXT_CLOCK_MHZ;
  T4.MaxSpeed              = CPU_MAX_MHZ;
  T4.CurrentSpeed          = CurrentArmMhz ();
  T4.Status                = 0x41;                  // populated, enabled
  T4.ProcessorUpgrade      = ProcessorUpgradeNone;
  T4.L1CacheHandle         = L1d;
  T4.L2CacheHandle         = L2;
  T4.L3CacheHandle         = 0xFFFF;
  T4.CoreCount             = CPU_CORES;
  T4.EnabledCoreCount      = CPU_CORES;
  T4.ThreadCount           = CPU_CORES;
  T4.CoreCount2            = CPU_CORES;
  T4.EnabledCoreCount2     = CPU_CORES;
  T4.ThreadCount2          = CPU_CORES;
  T4.ProcessorCharacteristics = BIT3 | BIT5;        // multi-core, execute protection
  S[0] = "SoC";
  S[1] = "Samsung";
  S[2] = "Samsung Exynos 5 Octa 5420 (Cortex-A15)";
  S[3] = "Exynos5420";
  AddRecord (&T4, OFFSET_OF (SMBIOS_TABLE_TYPE4, ThreadEnabled), S, 4);

  DEBUG ((DEBUG_INFO, "Smbios: processor MIDR 0x%08x, %u MHz now, %u cores\n",
          Midr, T4.CurrentSpeed, CPU_CORES));
}

STATIC
VOID
AddMemory (
  VOID
  )
{
  SMBIOS_TABLE_TYPE16  T16;
  SMBIOS_TABLE_TYPE17  T17;
  SMBIOS_TABLE_TYPE19  T19;
  EFI_SMBIOS_HANDLE    Array;
  EFI_PEI_HOB_POINTERS Hob;
  UINT64               Start;
  UINT64               End;
  CONST CHAR8          *S[2];

  Start = MAX_UINT64;
  End   = 0;
  for (Hob.Raw = GetHobList (); !END_OF_HOB_LIST (Hob); Hob.Raw = GET_NEXT_HOB (Hob)) {
    if ((GET_HOB_TYPE (Hob) == EFI_HOB_TYPE_RESOURCE_DESCRIPTOR) &&
        (Hob.ResourceDescriptor->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY)) {
      Start = MIN (Start, Hob.ResourceDescriptor->PhysicalStart);
      End   = MAX (End, Hob.ResourceDescriptor->PhysicalStart + Hob.ResourceDescriptor->ResourceLength);
    }
  }
  if (End == 0) {
    return;
  }

  ZeroMem (&T16, sizeof (T16));
  T16.Hdr.Type              = EFI_SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY;
  T16.Location              = MemoryArrayLocationSystemBoard;
  T16.Use                   = MemoryArrayUseSystemMemory;
  T16.MemoryErrorCorrection = MemoryErrorCorrectionNone;
  T16.MaximumCapacity       = RAM_MB * 1024;           // KB
  T16.MemoryErrorInformationHandle = 0xFFFE;
  T16.NumberOfMemoryDevices = 1;
  Array = AddRecord (&T16, sizeof (T16), NULL, 0);

  ZeroMem (&T17, sizeof (T17));
  T17.Hdr.Type             = EFI_SMBIOS_TYPE_MEMORY_DEVICE;
  T17.MemoryArrayHandle    = Array;
  T17.MemoryErrorInformationHandle = 0xFFFE;
  T17.TotalWidth           = 64;
  T17.DataWidth            = 64;
  T17.Size                 = RAM_MB;                   // MB
  T17.FormFactor           = MemoryFormFactorOther;
  T17.DeviceLocator        = 1;
  T17.MemoryType           = MemoryTypeLpddr3;
  T17.TypeDetail.Synchronous = 1;
  T17.ConfiguredVoltage    = 1200;
  S[0] = "LPDDR3 (PoP)";
  AddRecord (&T17, OFFSET_OF (SMBIOS_TABLE_TYPE17, MemoryTechnology), S, 1);

  ZeroMem (&T19, sizeof (T19));
  T19.Hdr.Type          = EFI_SMBIOS_TYPE_MEMORY_ARRAY_MAPPED_ADDRESS;
  T19.StartingAddress   = (UINT32)(Start / SIZE_1KB);
  T19.EndingAddress     = (UINT32)((End - 1) / SIZE_1KB);
  T19.MemoryArrayHandle = Array;
  T19.PartitionWidth    = 1;
  AddRecord (&T19, sizeof (T19), NULL, 0);
}

STATIC
VOID
AddBootStatus (
  VOID
  )
{
  SMBIOS_TABLE_TYPE32  T32;

  ZeroMem (&T32, sizeof (T32));
  T32.Hdr.Type   = EFI_SMBIOS_TYPE_SYSTEM_BOOT_INFORMATION;
  T32.BootStatus = BootInformationStatusNoError;
  AddRecord (&T32, sizeof (T32), NULL, 0);
}

STATIC
VOID
EFIAPI
OnEndOfDxe (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  gBS->CloseEvent (Event);

  AddBiosSystemChassis ();
  AddProcessor ();
  AddMemory ();
  AddBootStatus ();
}

EFI_STATUS
EFIAPI
SmbiosPlatformDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT   Event;
  EFI_STATUS  Status;

  Status = gBS->LocateProtocol (&gEfiSmbiosProtocolGuid, NULL, (VOID **)&mSmbios);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnEndOfDxe, NULL,
                             &gEfiEndOfDxeEventGroupGuid, &Event);
}
