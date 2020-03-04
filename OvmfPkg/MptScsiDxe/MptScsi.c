/** @file

  This driver produces Extended SCSI Pass Thru Protocol instances for
  LSI Fusion MPT SCSI devices.

  Copyright (C) 2020, Oracle and/or its affiliates.

  This program and the accompanying materials are licensed and made available
  under the terms and conditions of the BSD License which accompanies this
  distribution. The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS, WITHOUT
  WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <Library/UefiLib.h>

//
// Higher versions will be used before lower, 0x10-0xffffffef is the version
// range for IVH (Indie Hardware Vendors)
//
#define MPT_SCSI_BINDING_VERSION 0x10

//
// Driver Binding
//

STATIC
EFI_STATUS
EFIAPI
MptScsiControllerSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL            *This,
  IN EFI_HANDLE                             ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL               *RemainingDevicePath OPTIONAL
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
MptScsiControllerStart (
  IN EFI_DRIVER_BINDING_PROTOCOL            *This,
  IN EFI_HANDLE                             ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL               *RemainingDevicePath OPTIONAL
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
MptScsiControllerStop (
  IN EFI_DRIVER_BINDING_PROTOCOL            *This,
  IN  EFI_HANDLE                            ControllerHandle,
  IN  UINTN                                 NumberOfChildren,
  IN  EFI_HANDLE                            *ChildHandleBuffer
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_DRIVER_BINDING_PROTOCOL mMptScsiDriverBinding = {
  &MptScsiControllerSupported,
  &MptScsiControllerStart,
  &MptScsiControllerStop,
  MPT_SCSI_BINDING_VERSION,
  NULL, // ImageHandle, filled by EfiLibInstallDriverBindingComponentName2
  NULL, // DriverBindingHandle, filled as well
};

//
// Entry Point
//

EFI_STATUS
EFIAPI
MptScsiEntryPoint (
  IN EFI_HANDLE       ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  return EfiLibInstallDriverBindingComponentName2 (
           ImageHandle,
           SystemTable,
           &mMptScsiDriverBinding,
           ImageHandle, // The handle to install onto
           NULL, // TODO Component name
           NULL // TODO Component name
           );
}
