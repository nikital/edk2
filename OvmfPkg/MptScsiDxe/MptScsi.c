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

#include <IndustryStandard/FusionMptScsi.h>
#include <IndustryStandard/Pci.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/PciIo.h>
#include <Protocol/PciRootBridgeIo.h>
#include <Protocol/ScsiPassThruExt.h>

//
// Higher versions will be used before lower, 0x10-0xffffffef is the version
// range for IVH (Indie Hardware Vendors)
//
#define MPT_SCSI_BINDING_VERSION 0x10

//
// Runtime Structures
//

typedef struct {
  MPT_SCSI_IO_ERROR_REPLY         IoErrorReply;
  MPT_SCSI_REQUEST_WITH_SG        IoRequest;
  UINT8                           Sense[MAX_UINT8];
  UINT8                           Data[0x2000];
} MPT_SCSI_DMA_BUFFER;

#define MPT_SCSI_DEV_SIGNATURE SIGNATURE_32 ('M','P','T','S')
typedef struct {
  UINT32                          Signature;
  EFI_EXT_SCSI_PASS_THRU_PROTOCOL PassThru;
  EFI_EXT_SCSI_PASS_THRU_MODE     PassThruMode;
  EFI_PCI_IO_PROTOCOL             *PciIo;
  UINT64                          OriginalPciAttributes;
  UINT32                          StallPerPollUsec;
  MPT_SCSI_DMA_BUFFER             *Dma;
  EFI_PHYSICAL_ADDRESS            DmaPhysical;
  VOID                            *DmaMapping;
} MPT_SCSI_DEV;

#define MPT_SCSI_FROM_PASS_THRU(PassThruPtr) \
  CR (PassThruPtr, MPT_SCSI_DEV, PassThru, MPT_SCSI_DEV_SIGNATURE)

#define MPT_SCSI_DMA_ADDR(Dev, MemberName) \
  (Dev->DmaPhysical + OFFSET_OF(MPT_SCSI_DMA_BUFFER, MemberName))

//
// Hardware functions
//

STATIC
EFI_STATUS
Out32 (
  IN MPT_SCSI_DEV       *Dev,
  IN UINT32             Addr,
  IN UINT32             Data
  )
{
  return Dev->PciIo->Io.Write (
                          Dev->PciIo,
                          EfiPciIoWidthUint32,
                          0, // BAR0
                          Addr,
                          1,
                          &Data
                          );
}

STATIC
EFI_STATUS
In32 (
  IN  MPT_SCSI_DEV       *Dev,
  IN  UINT32             Addr,
  OUT UINT32             *Data
  )
{
  return Dev->PciIo->Io.Read (
                          Dev->PciIo,
                          EfiPciIoWidthUint32,
                          0, // BAR0
                          Addr,
                          1,
                          Data
                          );
}

STATIC
EFI_STATUS
MptDoorbell (
  IN MPT_SCSI_DEV       *Dev,
  IN UINT8              DoorbellFunc,
  IN UINT8              DoorbellArg
  )
{
  return Out32 (
           Dev,
           MPT_REG_DOORBELL,
           (((UINT32)DoorbellFunc) << 24) | (DoorbellArg << 16)
           );
}

STATIC
EFI_STATUS
MptScsiReset (
  IN MPT_SCSI_DEV       *Dev
  )
{
  EFI_STATUS Status;

  //
  // Reset hardware
  //
  Status = MptDoorbell (Dev, MPT_DOORBELL_RESET, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  //
  // Mask interrupts
  //
  Status = Out32 (Dev, MPT_REG_IMASK, MPT_IMASK_DOORBELL|MPT_IMASK_REPLY);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  //
  // Clear interrupt status
  //
  Status = Out32 (Dev, MPT_REG_ISTATUS, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MptScsiInit (
  IN MPT_SCSI_DEV       *Dev
  )
{
  EFI_STATUS                     Status;
  MPT_IO_CONTROLLER_INIT_REQUEST Req;
  MPT_IO_CONTROLLER_INIT_REPLY   Reply;
  UINT8                          *ReplyBytes;
  UINT32                         Reply32;

  Dev->StallPerPollUsec = PcdGet32 (PcdMptScsiStallPerPollUsec);

  Status = MptScsiReset (Dev);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Req, sizeof (Req));
  ZeroMem (&Reply, sizeof (Reply));
  Req.Data.WhoInit = MPT_IOC_WHOINIT_ROM_BIOS;
  Req.Data.Function = MPT_MESSAGE_HDR_FUNCTION_IOC_INIT;
  Req.Data.MaxDevices = 1;
  Req.Data.MaxBuses = 1;
  Req.Data.ReplyFrameSize = sizeof (MPT_SCSI_IO_ERROR_REPLY);

  //
  // Send controller init through doorbell
  //
  Status = MptDoorbell (
             Dev,
             MPT_DOORBELL_HANDSHAKE,
             sizeof (Req) / sizeof (UINT32)
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Dev->PciIo->Io.Write (
                            Dev->PciIo,
                            EfiPciIoWidthFifoUint32,
                            0,
                            MPT_REG_DOORBELL,
                            sizeof (Req) / sizeof (UINT32),
                            &Req
                            );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Read reply through doorbell
  // Each 32bit read produces 16bit of data
  //
  ReplyBytes = (UINT8 *)&Reply;
  while (ReplyBytes != (UINT8 *)(&Reply + 1)) {
    Status = In32 (Dev, MPT_REG_DOORBELL, &Reply32);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    CopyMem (ReplyBytes, &Reply32, sizeof (UINT16));
    ReplyBytes += sizeof (UINT16);
  }

  //
  // Clear interrupts generated by doorbell reply
  //
  Status = Out32 (Dev, MPT_REG_ISTATUS, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Put one free reply frame on the reply queue, the hardware may use it to
  // report an error to us.
  //
  Status = Out32 (Dev, MPT_REG_REP_Q, MPT_SCSI_DMA_ADDR (Dev, IoErrorReply));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MptScsiPopulateRequest (
  IN MPT_SCSI_DEV                                   *Dev,
  IN UINT8                                          Target,
  IN UINT64                                         Lun,
  IN OUT EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET *Packet
  )
{
  MPT_SCSI_REQUEST_WITH_SG *Request;

  Request = &Dev->Dma->IoRequest;

  if (Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_BIDIRECTIONAL ||
      Packet->CdbLength > sizeof (Request->Data.Header.CDB)) {
    return EFI_UNSUPPORTED;
  }

  if (Target > 0 || Lun > 0) {
    return EFI_INVALID_PARAMETER;
  }

  if (Packet->InTransferLength > sizeof (Dev->Dma->Data)) {
    Packet->InTransferLength = sizeof (Dev->Dma->Data);
    return EFI_BAD_BUFFER_SIZE;
  }
  if (Packet->OutTransferLength > sizeof (Dev->Dma->Data)) {
    Packet->OutTransferLength = sizeof (Dev->Dma->Data);
    return EFI_BAD_BUFFER_SIZE;
  }

  ZeroMem (Request, sizeof (*Request));
  Request->Data.Header.TargetID = Target;
  //
  // It's 1 and not 0, for some reason...
  //
  Request->Data.Header.LUN[1] = Lun;
  Request->Data.Header.Function = MPT_MESSAGE_HDR_FUNCTION_SCSI_IO_REQUEST;
  Request->Data.Header.MessageContext = 1; // We handle one request at a time

  Request->Data.Header.CDBLength = Packet->CdbLength;
  CopyMem (Request->Data.Header.CDB, Packet->Cdb, Packet->CdbLength);

  //
  // SenseDataLength is UINT8, Sense[] is MAX_UINT8, so we can't overflow
  //
  ZeroMem (&Dev->Dma->Sense, Packet->SenseDataLength);
  Request->Data.Header.SenseBufferLength = Packet->SenseDataLength;
  Request->Data.Header.SenseBufferLowAddress = MPT_SCSI_DMA_ADDR (Dev, Sense);

  Request->Data.Sg.EndOfList = 1;
  Request->Data.Sg.EndOfBuffer = 1;
  Request->Data.Sg.LastElement = 1;
  Request->Data.Sg.ElementType = MPT_SG_ENTRY_TYPE_SIMPLE;
  Request->Data.Sg.DataBufferAddress = MPT_SCSI_DMA_ADDR (Dev, Data);

  Request->Data.Header.Control = MPT_SCSIIO_REQUEST_CONTROL_TXDIR_NONE;
  switch (Packet->DataDirection)
  {
  case EFI_EXT_SCSI_DATA_DIRECTION_READ:
    if (Packet->InTransferLength == 0) {
      break;
    }
    Request->Data.Header.DataLength = Packet->InTransferLength;
    Request->Data.Sg.Length = Packet->InTransferLength;
    Request->Data.Header.Control = MPT_SCSIIO_REQUEST_CONTROL_TXDIR_READ;
    break;
  case EFI_EXT_SCSI_DATA_DIRECTION_WRITE:
    if (Packet->OutTransferLength == 0) {
      break;
    }
    Request->Data.Header.DataLength = Packet->OutTransferLength;
    Request->Data.Sg.Length = Packet->OutTransferLength;
    Request->Data.Header.Control = MPT_SCSIIO_REQUEST_CONTROL_TXDIR_WRITE;

    CopyMem (Dev->Dma->Data, Packet->OutDataBuffer, Packet->OutTransferLength);
    Request->Data.Sg.BufferContainsData = 1;
    break;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MptScsiSendRequest (
  IN MPT_SCSI_DEV                                   *Dev,
  IN OUT EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET *Packet
  )
{
  EFI_STATUS Status;

  //
  // Make sure Request is fully written
  //
  MemoryFence ();

  Status = Out32 (Dev, MPT_REG_REQ_Q, MPT_SCSI_DMA_ADDR (Dev, IoRequest));
  if (EFI_ERROR (Status)) {
    //
    // We couldn't enqueue the request, report it as an adapter error
    //
    Packet->InTransferLength  = 0;
    Packet->OutTransferLength = 0;
    Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OTHER;
    Packet->TargetStatus      = EFI_EXT_SCSI_STATUS_TARGET_GOOD;
    Packet->SenseDataLength   = 0;
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MptScsiGetReply (
  IN MPT_SCSI_DEV                                   *Dev,
  OUT UINT32                                        *Reply
  )
{
  EFI_STATUS Status;
  UINT32     Istatus;
  UINT32     EmptyReply;

  for (;;) {
    Status = In32 (Dev, MPT_REG_ISTATUS, &Istatus);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    //
    // Interrupt raised
    //
    if (Istatus & MPT_IMASK_REPLY) {
      break;
    }

    gBS->Stall (Dev->StallPerPollUsec);
  }

  Status = In32 (Dev, MPT_REG_REP_Q, Reply);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // The driver is supposed to fetch replies until 0xffffffff is returned, which
  // will reset the interrupt status. We put only one request, so we expect the
  // next read reply to be the last.
  //
  Status = In32 (Dev, MPT_REG_REP_Q, &EmptyReply);
  if (EFI_ERROR (Status) || EmptyReply != MAX_UINT32) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MptScsiHandleReply (
  IN MPT_SCSI_DEV                                   *Dev,
  IN UINT32                                         Reply,
  OUT EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET    *Packet
  )
{
  EFI_STATUS Status;

  CopyMem (Packet->SenseData, Dev->Dma->Sense, Packet->SenseDataLength);
  if (Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_READ) {
    CopyMem (Packet->InDataBuffer, Dev->Dma->Data, Packet->InTransferLength);
  }

  if (Reply == Dev->Dma->IoRequest.Data.Header.MessageContext) {
    //
    // Everything is good
    //
    Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK;
    Packet->TargetStatus = EFI_EXT_SCSI_STATUS_TARGET_GOOD;

  } else if (Reply & (1 << 31)) {
    DEBUG ((DEBUG_ERROR, "%a: request failed\n", __FUNCTION__));
    //
    // When reply MSB is set, it's an error frame.
    //

    switch (Dev->Dma->IoErrorReply.Data.IOCStatus) {
    case MPT_SCSI_IO_ERROR_IOCSTATUS_DEVICE_NOT_THERE:
      Packet->HostAdapterStatus =
        EFI_EXT_SCSI_STATUS_HOST_ADAPTER_SELECTION_TIMEOUT;
      break;
    default:
      Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OTHER;
      break;
    }

    //
    // Resubmit the reply frame to the reply queue
    //
    Status = Out32 (Dev, MPT_REG_REP_Q, MPT_SCSI_DMA_ADDR (Dev, IoErrorReply));
    if (EFI_ERROR (Status)) {
      return Status;
    }

  } else {
    DEBUG ((DEBUG_ERROR, "%a: unexpected reply\n", __FUNCTION__));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

//
// Ext SCSI Pass Thru
//

STATIC
EFI_STATUS
EFIAPI
MptScsiPassThru (
  IN EFI_EXT_SCSI_PASS_THRU_PROTOCOL                *This,
  IN UINT8                                          *Target,
  IN UINT64                                         Lun,
  IN OUT EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET *Packet,
  IN EFI_EVENT                                      Event     OPTIONAL
  )
{
  EFI_STATUS   Status;
  MPT_SCSI_DEV *Dev;
  UINT32       Reply;

  Dev = MPT_SCSI_FROM_PASS_THRU (This);
  Status = MptScsiPopulateRequest (Dev, *Target, Lun, Packet);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = MptScsiSendRequest (Dev, Packet);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK;

  Status = MptScsiGetReply (Dev, &Reply);
  if (EFI_ERROR (Status)) {
    goto Fatal;
  }

  Status = MptScsiHandleReply (Dev, Reply, Packet);
  if (EFI_ERROR (Status)) {
    goto Fatal;
  }

  return Status;

Fatal:
  //
  // We erred in the middle of a transaction, a very serious problem has occured
  // and it's not clear if it's possible to recover without leaving the hardware
  // in an inconsistent state. Perhaps we would want to reset the device...
  //
  DEBUG ((DEBUG_ERROR, "%a: fatal error in scsi request\n", __FUNCTION__));
  Packet->InTransferLength  = 0;
  Packet->OutTransferLength = 0;
  if (Packet->HostAdapterStatus == EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK) {
    Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OTHER;
  }
  Packet->TargetStatus      = EFI_EXT_SCSI_STATUS_TARGET_TASK_ABORTED;
  Packet->SenseDataLength   = 0;
  return EFI_DEVICE_ERROR;
}

STATIC
BOOLEAN
IsTargetInitialized (
  IN UINT8                                          *Target
  )
{
  UINTN Idx;

  for (Idx = 0; Idx < TARGET_MAX_BYTES; ++Idx) {
    if (Target[Idx] != 0xFF) {
      return TRUE;
    }
  }
  return FALSE;
}

STATIC
EFI_STATUS
EFIAPI
MptScsiGetNextTargetLun (
  IN EFI_EXT_SCSI_PASS_THRU_PROTOCOL                *This,
  IN OUT UINT8                                      **Target,
  IN OUT UINT64                                     *Lun
  )
{
  //
  // Currently support only target 0 LUN 0, so hardcode it
  //
  if (!IsTargetInitialized (*Target)) {
    ZeroMem (*Target, TARGET_MAX_BYTES);
    *Lun = 0;
    return EFI_SUCCESS;
  } else {
    return EFI_NOT_FOUND;
  }
}

STATIC
EFI_STATUS
EFIAPI
MptScsiGetNextTarget (
  IN EFI_EXT_SCSI_PASS_THRU_PROTOCOL               *This,
  IN OUT UINT8                                     **Target
  )
{
  //
  // Currently support only target 0 LUN 0, so hardcode it
  //
  if (!IsTargetInitialized (*Target)) {
    ZeroMem (*Target, TARGET_MAX_BYTES);
    return EFI_SUCCESS;
  } else {
    return EFI_NOT_FOUND;
  }
}

STATIC
EFI_STATUS
EFIAPI
MptScsiBuildDevicePath (
  IN EFI_EXT_SCSI_PASS_THRU_PROTOCOL               *This,
  IN UINT8                                         *Target,
  IN UINT64                                        Lun,
  IN OUT EFI_DEVICE_PATH_PROTOCOL                  **DevicePath
  )
{
  SCSI_DEVICE_PATH *ScsiDevicePath;

  if (DevicePath == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // This device support 256 targets only, so it's enough to dereference
  // the LSB of Target.
  //
  if (*Target > 0 || Lun > 0) {
    return EFI_NOT_FOUND;
  }

  ScsiDevicePath = AllocateZeroPool (sizeof (*ScsiDevicePath));
  if (ScsiDevicePath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ScsiDevicePath->Header.Type      = MESSAGING_DEVICE_PATH;
  ScsiDevicePath->Header.SubType   = MSG_SCSI_DP;
  ScsiDevicePath->Header.Length[0] = (UINT8)sizeof (*ScsiDevicePath);
  ScsiDevicePath->Header.Length[1] = (UINT8)(sizeof (*ScsiDevicePath) >> 8);
  ScsiDevicePath->Pun              = *Target;
  ScsiDevicePath->Lun              = (UINT16)Lun;

  *DevicePath = &ScsiDevicePath->Header;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MptScsiGetTargetLun (
  IN EFI_EXT_SCSI_PASS_THRU_PROTOCOL               *This,
  IN EFI_DEVICE_PATH_PROTOCOL                      *DevicePath,
  OUT UINT8                                        **Target,
  OUT UINT64                                       *Lun
  )
{
  SCSI_DEVICE_PATH *ScsiDevicePath;

  if (DevicePath == NULL ||
      Target == NULL || *Target == NULL || Lun == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (DevicePath->Type    != MESSAGING_DEVICE_PATH ||
      DevicePath->SubType != MSG_SCSI_DP) {
    return EFI_UNSUPPORTED;
  }

  ScsiDevicePath = (SCSI_DEVICE_PATH *)DevicePath;
  if (ScsiDevicePath->Pun > 0 ||
      ScsiDevicePath->Lun > 0) {
    return EFI_NOT_FOUND;
  }

  ZeroMem (*Target, TARGET_MAX_BYTES);
  //
  // This device support 256 targets only, so it's enough to set the LSB
  // of Target.
  //
  **Target = (UINT8)ScsiDevicePath->Pun;
  *Lun = ScsiDevicePath->Lun;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MptScsiResetChannel (
  IN EFI_EXT_SCSI_PASS_THRU_PROTOCOL               *This
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
MptScsiResetTargetLun (
  IN EFI_EXT_SCSI_PASS_THRU_PROTOCOL               *This,
  IN UINT8                                         *Target,
  IN UINT64                                        Lun
  )
{
  return EFI_UNSUPPORTED;
}

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
                        sizeof (Pci) / sizeof (UINT32),
                        &Pci
                        );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (Pci.Hdr.VendorId == LSI_LOGIC_PCI_VENDOR_ID &&
      (Pci.Hdr.DeviceId == LSI_53C1030_PCI_DEVICE_ID ||
       Pci.Hdr.DeviceId == LSI_SAS1068_PCI_DEVICE_ID ||
       Pci.Hdr.DeviceId == LSI_SAS1068E_PCI_DEVICE_ID)) {
    Status = EFI_SUCCESS;
  } else {
    Status = EFI_UNSUPPORTED;
  }

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
MptScsiControllerStart (
  IN EFI_DRIVER_BINDING_PROTOCOL            *This,
  IN EFI_HANDLE                             ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL               *RemainingDevicePath OPTIONAL
  )
{
  EFI_STATUS           Status;
  MPT_SCSI_DEV         *Dev;
  UINTN                BytesMapped;

  Dev = AllocateZeroPool (sizeof (*Dev));
  if (Dev == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Dev->Signature = MPT_SCSI_DEV_SIGNATURE;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&Dev->PciIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    goto FreePool;
  }

  Status = Dev->PciIo->Attributes (
                         Dev->PciIo,
                         EfiPciIoAttributeOperationGet,
                         0,
                         &Dev->OriginalPciAttributes
                         );
  if (EFI_ERROR (Status)) {
    goto CloseProtocol;
  }

  //
  // Enable I/O Space & Bus-Mastering
  //
  Status = Dev->PciIo->Attributes (
                         Dev->PciIo,
                         EfiPciIoAttributeOperationEnable,
                         (EFI_PCI_IO_ATTRIBUTE_IO |
                          EFI_PCI_IO_ATTRIBUTE_BUS_MASTER),
                         NULL
                         );
  if (EFI_ERROR (Status)) {
    goto CloseProtocol;
  }

  //
  // Create buffers for data transfer
  //
  Status = Dev->PciIo->AllocateBuffer (
                         Dev->PciIo,
                         AllocateAnyPages,
                         EfiBootServicesData,
                         EFI_SIZE_TO_PAGES (sizeof (*Dev->Dma)),
                         (VOID **)&Dev->Dma,
                         EFI_PCI_ATTRIBUTE_MEMORY_CACHED
                         );
  if (EFI_ERROR (Status)) {
    goto RestoreAttributes;
  }

  BytesMapped = sizeof (*Dev->Dma);
  Status = Dev->PciIo->Map (
                         Dev->PciIo,
                         EfiPciIoOperationBusMasterCommonBuffer,
                         Dev->Dma,
                         &BytesMapped,
                         &Dev->DmaPhysical,
                         &Dev->DmaMapping
                         );
  if (EFI_ERROR (Status)) {
    goto FreeBuffer;
  }

  if (BytesMapped != sizeof (*Dev->Dma)) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Unmap;
  }

  Status = MptScsiInit (Dev);
  if (EFI_ERROR (Status)) {
    goto Unmap;
  }

  //
  // Host adapter channel, doesn't exist
  //
  Dev->PassThruMode.AdapterId = MAX_UINT32;
  Dev->PassThruMode.Attributes =
    EFI_EXT_SCSI_PASS_THRU_ATTRIBUTES_PHYSICAL
    | EFI_EXT_SCSI_PASS_THRU_ATTRIBUTES_LOGICAL;

  Dev->PassThru.Mode = &Dev->PassThruMode;
  Dev->PassThru.PassThru = &MptScsiPassThru;
  Dev->PassThru.GetNextTargetLun = &MptScsiGetNextTargetLun;
  Dev->PassThru.BuildDevicePath = &MptScsiBuildDevicePath;
  Dev->PassThru.GetTargetLun = &MptScsiGetTargetLun;
  Dev->PassThru.ResetChannel = &MptScsiResetChannel;
  Dev->PassThru.ResetTargetLun = &MptScsiResetTargetLun;
  Dev->PassThru.GetNextTarget = &MptScsiGetNextTarget;

  Status = gBS->InstallProtocolInterface (
                  &ControllerHandle,
                  &gEfiExtScsiPassThruProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &Dev->PassThru
                  );
  if (EFI_ERROR (Status)) {
    goto Unmap;
  }

  return EFI_SUCCESS;

Unmap:
    Dev->PciIo->Unmap (
                  Dev->PciIo,
                  Dev->DmaMapping
                  );

FreeBuffer:
    Dev->PciIo->FreeBuffer (
                    Dev->PciIo,
                    EFI_SIZE_TO_PAGES (sizeof (*Dev->Dma)),
                    Dev->Dma
      );
RestoreAttributes:
  Dev->PciIo->Attributes (
                Dev->PciIo,
                EfiPciIoAttributeOperationEnable,
                Dev->OriginalPciAttributes,
                NULL
                );

CloseProtocol:
  gBS->CloseProtocol (
         ControllerHandle,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

FreePool:
  FreePool (Dev);

  return Status;
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
  EFI_STATUS                      Status;
  EFI_EXT_SCSI_PASS_THRU_PROTOCOL *PassThru;
  MPT_SCSI_DEV                    *Dev;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiExtScsiPassThruProtocolGuid,
                  (VOID **)&PassThru,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL // Lookup only
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Dev = MPT_SCSI_FROM_PASS_THRU (PassThru);

  Status = gBS->UninstallProtocolInterface (
                  ControllerHandle,
                  &gEfiExtScsiPassThruProtocolGuid,
                  &Dev->PassThru
                  );
  ASSERT_EFI_ERROR (Status);

  MptScsiReset (Dev);

  Dev->PciIo->Unmap (
                Dev->PciIo,
                Dev->DmaMapping
                );

  Dev->PciIo->FreeBuffer (
                Dev->PciIo,
                EFI_SIZE_TO_PAGES (sizeof (*Dev->Dma)),
                Dev->Dma
                );

  Dev->PciIo->Attributes (
                Dev->PciIo,
                EfiPciIoAttributeOperationEnable,
                Dev->OriginalPciAttributes,
                NULL
                );

  gBS->CloseProtocol (
         ControllerHandle,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

  FreePool (Dev);

  return Status;
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
// Component Name
//

STATIC
EFI_UNICODE_STRING_TABLE mDriverNameTable[] = {
  { "eng;en", L"LSI Fusion MPT SCSI Driver" },
  { NULL,     NULL                   }
};

STATIC
EFI_COMPONENT_NAME_PROTOCOL mComponentName;

EFI_STATUS
EFIAPI
MptScsiGetDriverName (
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

EFI_STATUS
EFIAPI
MptScsiGetDeviceName (
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
  &MptScsiGetDriverName,
  &MptScsiGetDeviceName,
  "eng" // SupportedLanguages, ISO 639-2 language codes
};

STATIC
EFI_COMPONENT_NAME2_PROTOCOL mComponentName2 = {
  (EFI_COMPONENT_NAME2_GET_DRIVER_NAME)     &MptScsiGetDriverName,
  (EFI_COMPONENT_NAME2_GET_CONTROLLER_NAME) &MptScsiGetDeviceName,
  "en" // SupportedLanguages, RFC 4646 language codes
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
           &mComponentName,
           &mComponentName2
           );
}
