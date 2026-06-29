from __future__ import annotations
from enum import IntEnum

# USB identifiers for kvasir's CDC device, set in
# firmware/cubemx/USB_DEVICE/App/usbd_desc.c USER CODE BEGIN PRIVATE_DEFINES.
USB_VID = 0x0483
USB_PID = 0x4B56  # 'KV'

MAGIC = 0xAD


class FrameType(IntEnum):
    # Device → host
    DEVICE_INFO = 0x00
    STREAM_DATA = 0x01
    ACK         = 0x10
    # Host → device
    CONFIG      = 0x80
    START       = 0x81
    STOP        = 0x82


class ChannelType(IntEnum):
    STREAM = 0x00


class AckStatus(IntEnum):
    OK            = 0x00
    ERR_INVALID   = 0x01
    ERR_BAD_STATE = 0x02
