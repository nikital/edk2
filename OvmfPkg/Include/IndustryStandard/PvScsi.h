/** @file

  VMware PVSCSI Device specific type and macro definitions.

  Copyright (C) 2020, Oracle and/or its affiliates.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef __PVSCSI_H_
#define __PVSCSI_H_

//
// Device offsets and constants
//

#define PCI_VENDOR_ID_VMWARE            (0x15ad)
#define PCI_DEVICE_ID_VMWARE_PVSCSI     (0x07c0)

//
// CDB (Command Descriptor Block) with size above this constant
// should be considered out-of-band
//
#define PVSCSI_CDB_MAX_SIZE         (16)

enum PVSCSI_BAR0_OFFSETS {
  PVSCSI_REG_OFFSET_COMMAND        =    0x0,
  PVSCSI_REG_OFFSET_COMMAND_DATA   =    0x4,
  PVSCSI_REG_OFFSET_COMMAND_STATUS =    0x8,
  PVSCSI_REG_OFFSET_LAST_STS_0     =  0x100,
  PVSCSI_REG_OFFSET_LAST_STS_1     =  0x104,
  PVSCSI_REG_OFFSET_LAST_STS_2     =  0x108,
  PVSCSI_REG_OFFSET_LAST_STS_3     =  0x10c,
  PVSCSI_REG_OFFSET_INTR_STATUS    = 0x100c,
  PVSCSI_REG_OFFSET_INTR_MASK      = 0x2010,
  PVSCSI_REG_OFFSET_KICK_NON_RW_IO = 0x3014,
  PVSCSI_REG_OFFSET_DEBUG          = 0x3018,
  PVSCSI_REG_OFFSET_KICK_RW_IO     = 0x4018,
};

//
// Define Interrupt-Status register flags
//
#define PVSCSI_INTR_CMPL_0      (1 << 0)
#define PVSCSI_INTR_CMPL_1      (1 << 1)
#define PVSCSI_INTR_CMPL_MASK   (PVSCSI_INTR_CMPL_0 | PVSCSI_INTR_CMPL_1)

enum PVSCSI_COMMANDS {
  PVSCSI_CMD_FIRST             = 0,
  PVSCSI_CMD_ADAPTER_RESET     = 1,
  PVSCSI_CMD_ISSUE_SCSI        = 2,
  PVSCSI_CMD_SETUP_RINGS       = 3,
  PVSCSI_CMD_RESET_BUS         = 4,
  PVSCSI_CMD_RESET_DEVICE      = 5,
  PVSCSI_CMD_ABORT_CMD         = 6,
  PVSCSI_CMD_CONFIG            = 7,
  PVSCSI_CMD_SETUP_MSG_RING    = 8,
  PVSCSI_CMD_DEVICE_UNPLUG     = 9,
  PVSCSI_CMD_LAST              = 10
};

#define PVSCSI_SETUP_RINGS_MAX_NUM_PAGES    (32)

#pragma pack (1)
typedef struct {
  UINT32 ReqRingNumPages;
  UINT32 CmpRingNumPages;
  UINT64 RingsStatePPN;
  UINT64 ReqRingPPNs[PVSCSI_SETUP_RINGS_MAX_NUM_PAGES];
  UINT64 CmpRingPPNs[PVSCSI_SETUP_RINGS_MAX_NUM_PAGES];
} PVSCSI_CMD_DESC_SETUP_RINGS;
#pragma pack ()

#define PVSCSI_MAX_CMD_DATA_WORDS   \
  (sizeof (PVSCSI_CMD_DESC_SETUP_RINGS) / sizeof (UINT32))

#pragma pack (1)
typedef struct {
  UINT32 ReqProdIdx;
  UINT32 ReqConsIdx;
  UINT32 ReqNumEntriesLog2;

  UINT32 CmpProdIdx;
  UINT32 CmpConsIdx;
  UINT32 CmpNumEntriesLog2;

  UINT8  Pad[104];

  UINT32 MsgProdIdx;
  UINT32 MsgConsIdx;
  UINT32 MsgNumEntriesLog2;
} PVSCSI_RINGS_STATE;
#pragma pack ()

//
// Define PVSCSI request descriptor tags
//
#define PVSCSI_SIMPLE_QUEUE_TAG            (0x20)

//
// Define PVSCSI request descriptor flags
//
#define PVSCSI_FLAG_CMD_WITH_SG_LIST       (1 << 0)
#define PVSCSI_FLAG_CMD_OUT_OF_BAND_CDB    (1 << 1)
#define PVSCSI_FLAG_CMD_DIR_NONE           (1 << 2)
#define PVSCSI_FLAG_CMD_DIR_TOHOST         (1 << 3)
#define PVSCSI_FLAG_CMD_DIR_TODEVICE       (1 << 4)

#pragma pack (1)
typedef struct {
  UINT64 Context;
  UINT64 DataAddr;
  UINT64 DataLen;
  UINT64 SenseAddr;
  UINT32 SenseLen;
  UINT32 Flags;
  UINT8  Cdb[16];
  UINT8  CdbLen;
  UINT8  Lun[8];
  UINT8  Tag;
  UINT8  Bus;
  UINT8  Target;
  UINT8  vCPUHint;
  UINT8  Unused[59];
} PVSCSI_RING_REQ_DESC;
#pragma pack ()

//
// Host adapter status/error codes
//
enum PVSCSI_HOST_BUS_ADAPTER_STATUS {
   BTSTAT_SUCCESS       = 0x00,  // CCB complete normally with no errors
   BTSTAT_LINKED_COMMAND_COMPLETED           = 0x0a,
   BTSTAT_LINKED_COMMAND_COMPLETED_WITH_FLAG = 0x0b,
   BTSTAT_DATA_UNDERRUN = 0x0c,
   BTSTAT_SELTIMEO      = 0x11,  // SCSI selection timeout
   BTSTAT_DATARUN       = 0x12,  // Data overrun/underrun
   BTSTAT_BUSFREE       = 0x13,  // Unexpected bus free
   BTSTAT_INVPHASE      = 0x14,  //
                                 // Invalid bus phase or sequence requested by
                                 // target
                                 //
   BTSTAT_LUNMISMATCH   = 0x17,  // Linked CCB has different LUN from first CCB
   BTSTAT_SENSFAILED    = 0x1b,  // Auto request sense failed
   BTSTAT_TAGREJECT     = 0x1c,  //
                                 // SCSI II tagged queueing message rejected by
                                 // target
                                 //
   BTSTAT_BADMSG        = 0x1d,  //
                                 // Unsupported message received by the host
                                 // adapter
                                 //
   BTSTAT_HAHARDWARE    = 0x20,  // Host adapter hardware failed
   BTSTAT_NORESPONSE    = 0x21,  //
                                 // Target did not respond to SCSI ATN sent a
                                 // SCSI RST
                                 //
   BTSTAT_SENTRST       = 0x22,  // Host adapter asserted a SCSI RST
   BTSTAT_RECVRST       = 0x23,  // Other SCSI devices asserted a SCSI RST
   BTSTAT_DISCONNECT    = 0x24,  //
                                 // Target device reconnected improperly
                                 // (w/o tag)
                                 //
   BTSTAT_BUSRESET      = 0x25,  // Host adapter issued BUS device reset
   BTSTAT_ABORTQUEUE    = 0x26,  // Abort queue generated
   BTSTAT_HASOFTWARE    = 0x27,  // Host adapter software error
   BTSTAT_HATIMEOUT     = 0x30,  // Host adapter hardware timeout error
   BTSTAT_SCSIPARITY    = 0x34,  // SCSI parity error detected
};

#pragma pack (1)
typedef struct {
  UINT64 Context;
  UINT64 DataLen;
  UINT32 SenseLen;
  UINT16 HostStatus;
  UINT16 ScsiStatus;
  UINT32 Pad[2];
} PVSCSI_RING_CMP_DESC;
#pragma pack ()

#endif // __PVSCSI_H_
