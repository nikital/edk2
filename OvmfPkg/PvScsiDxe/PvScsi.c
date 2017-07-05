/** @file

  This driver produces Extended SCSI Pass Thru Protocol instances for
  pvscsi devices.

  Copyright (C) 2020, Oracle and/or its affiliates.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <IndustryStandard/Pci.h>
#include <IndustryStandard/PvScsi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/PciIo.h>

//
// Higher versions will be used before lower, 0x10-0xffffffef is the version
// range for IVH (Indie Hardware Vendors)
//
#define PVSCSI_BINDING_VERSION      0x10

//
// Driver Binding
//

STATIC
EFI_STATUS
EFIAPI
PvScsiDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                  ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath OPTIONAL
  )
{
  EFI_STATUS          Status;
  EFI_PCI_IO_PROTOCOL *PciIo;
  PCI_TYPE00          Pci;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PciIo->Pci.Read (
                        PciIo,
                        EfiPciIoWidthUint32,
                        0,
                        sizeof(Pci) / sizeof(UINT32),
                        &Pci
                        );
  if (EFI_ERROR (Status)) {
      goto Done;
  }

  if ((Pci.Hdr.VendorId != PCI_VENDOR_ID_VMWARE) ||
      (Pci.Hdr.DeviceId != PCI_DEVICE_ID_VMWARE_PVSCSI)) {
      Status = EFI_UNSUPPORTED;
      goto Done;
  }

  Status = EFI_SUCCESS;

Done:
  gBS->CloseProtocol (
         ControllerHandle,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
PvScsiDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                  ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath OPTIONAL
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
PvScsiDriverBindingStop (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                  ControllerHandle,
  IN UINTN                       NumberOfChildren,
  IN EFI_HANDLE                  *ChildHandleBuffer
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_DRIVER_BINDING_PROTOCOL mPvScsiDriverBinding = {
  &PvScsiDriverBindingSupported,
  &PvScsiDriverBindingStart,
  &PvScsiDriverBindingStop,
  PVSCSI_BINDING_VERSION,
  NULL, // ImageHandle, filled by EfiLibInstallDriverBindingComponentName2()
  NULL  // DriverBindingHandle, filled as well
};

//
// Component Name
//

STATIC
EFI_UNICODE_STRING_TABLE mDriverNameTable[] = {
  { "eng;en", L"PVSCSI Host Driver" },
  { NULL,     NULL                  }
};

STATIC
EFI_COMPONENT_NAME_PROTOCOL mComponentName;

STATIC
EFI_STATUS
EFIAPI
PvScsiGetDriverName (
  IN  EFI_COMPONENT_NAME_PROTOCOL *This,
  IN  CHAR8                       *Language,
  OUT CHAR16                      **DriverName
  )
{
  return LookupUnicodeString2 (
           Language,
           This->SupportedLanguages,
           mDriverNameTable,
           DriverName,
           (BOOLEAN)(This == &mComponentName) // Iso639Language
           );
}

STATIC
EFI_STATUS
EFIAPI
PvScsiGetDeviceName (
  IN  EFI_COMPONENT_NAME_PROTOCOL *This,
  IN  EFI_HANDLE                  DeviceHandle,
  IN  EFI_HANDLE                  ChildHandle,
  IN  CHAR8                       *Language,
  OUT CHAR16                      **ControllerName
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_COMPONENT_NAME_PROTOCOL mComponentName = {
  &PvScsiGetDriverName,
  &PvScsiGetDeviceName,
  "eng" // SupportedLanguages, ISO 639-2 language codes
};

STATIC
EFI_COMPONENT_NAME2_PROTOCOL mComponentName2 = {
  (EFI_COMPONENT_NAME2_GET_DRIVER_NAME)     &PvScsiGetDriverName,
  (EFI_COMPONENT_NAME2_GET_CONTROLLER_NAME) &PvScsiGetDeviceName,
  "en" // SupportedLanguages, RFC 4646 language codes
};

//
// Entry Point
//

EFI_STATUS
EFIAPI
PvScsiEntryPoint (
  IN EFI_HANDLE       ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  return EfiLibInstallDriverBindingComponentName2 (
           ImageHandle,
           SystemTable,
           &mPvScsiDriverBinding,
           ImageHandle,
           &mComponentName,
           &mComponentName2
           );
}
