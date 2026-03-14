#pragma once

#ifdef _IS_KERNEL_MODE_COMPONENT
    #include <ntddk.h>
#else
    #include <windows.h>
    #include <winioctl.h>
#endif

// ============================================================================
// Global Macros & Constants
// ============================================================================

// Defines the maximum size of the data payload we will copy from a URB.
// Limits non-paged pool memory allocations to prevent system resource exhaustion.
#define MAX_URB_PAYLOAD_SIZE 4096

// Maximum number of USB endpoints (pipes) the driver tracks per device.
#define MAX_SUPPORTED_PIPES 32

// A magic number used to verify that a given WDM PDEVICE_OBJECT belongs to our 
// specific child filter driver before we attempt to cast its device extension.
// 'MDWc' in little-endian memory translates to "cWDM".
#define WDM_FILTER_MAGIC 'MDWc' 

// ============================================================================
// IOCTL & Device Name Definitions
// ============================================================================

// Base device type for custom IOCTLs. 0xCF54 is within the IHV specific range.
#define FILE_DEVICE_INVERTED 0xCF54

// IOCTL sent by the user-mode application to poll for events.
// The driver holds this request in a manual queue until an event occurs (Inverted Call Model).
#define IOCTL_CUSTOM_GET_EVENT \
    CTL_CODE(FILE_DEVICE_INVERTED, 2049, METHOD_BUFFERED, FILE_ANY_ACCESS)

// IOCTL sent by the user-mode application to supply a verdict (Allow/Block)
// for a previously intercepted and held URB request.
#define IOCTL_CUSTOM_RELEASE_URB \
    CTL_CODE(FILE_DEVICE_INVERTED, 2050, METHOD_BUFFERED, FILE_ANY_ACCESS)

// IOCTL sent by the user-mode application to supply a verdict (Allow/Block)
// for a previously intercepted and held IRP_MN_START_DEVICE request
#define IOCTL_CUSTOM_DEVICE_START_VERDICT \
    CTL_CODE(FILE_DEVICE_INVERTED, 2051, METHOD_BUFFERED, FILE_ANY_ACCESS)

// IOCTL sent by the user-mode application to remove a device
#define IOCTL_CUSTOM_REMOVE_DEVICE \
    CTL_CODE(FILE_DEVICE_INVERTED, 2052, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Device Name for IOCTL 
#define USERMODE_DEVICE_PATH L"\\\\.\\BusFilterIOCTL"

// ============================================================================
// Shared User-Mode / Kernel-Mode Data Structures
// ============================================================================
// These structures must be perfectly aligned between the driver and application.

// Identifies the type of data contained within the master event packet.
typedef enum _FILTER_EVENT_TYPE {
    FilterEventPnp = 1,
    FilterEventUrb = 2,
    FilterEventConfiguration = 3,
    FilterEventStartDevice = 4
} FILTER_EVENT_TYPE;

// Payload for Plug and Play events.
typedef struct _FILTER_PNP_EVENT_DATA {
    // The specific PnP minor function code (e.g., IRP_MN_START_DEVICE, IRP_MN_REMOVE_DEVICE).
    UCHAR MinorFunction;
} FILTER_PNP_EVENT_DATA, * PFILTER_PNP_EVENT_DATA;

// Payload for device enumeration/start events
typedef struct _FILTER_START_EVENT_DATA {
    // A unique identifier assigned by the driver to this specific request.
    // User mode must return this ID in its verdict so the driver can locate the exact IRP.
    UINT64 EventId;

    // The physical USB port number the device is connected to.
    ULONG PortNumber;

    ULONG HardwareIDsLength;
    ULONG CompatibleIDsLength;
    ULONG ContainerIdLength;

    // Contains:
    // Product ID and Vendor ID 
    // Broad Identifiers for device class
    // Container ID (shared across child devices belonging to a composite device) 
    UCHAR DataBuffer[ANYSIZE_ARRAY];
} FILTER_START_EVENT_DATA, * PFILTER_START_EVENT_DATA;

// Payload for intercepted USB Request Blocks (URBs).
typedef struct _FILTER_URB_EVENT_DATA {
    // The URB function code (e.g., 0x0009 for BULK_OR_INTERRUPT_TRANSFER).
    USHORT UrbFunction;

    // The physical USB endpoint address. The highest bit indicates direction (0x80 = IN).
    UCHAR EndpointAddress;

    // The valid number of bytes copied into the Payload array.
    ULONG DataLength;

    // A unique identifier assigned by the driver to this specific request.
    // User mode must return this ID in its verdict so the driver can locate the exact IRP.
    UINT64 EventId;

    // A dynamically sized array containing the raw bytes extracted from the URB transfer buffer.
    UCHAR Payload[ANYSIZE_ARRAY];
} FILTER_URB_EVENT_DATA, * PFILTER_URB_EVENT_DATA;

// Defines properties for a single active USB pipe.
typedef struct _FILTER_PIPE_INFO {
    // The USB interface number this pipe belongs to.
    UCHAR InterfaceNumber;

    // The physical USB endpoint address (e.g., 0x81 for IN, 0x02 for OUT).
    UCHAR EndpointAddress;

    // The transfer type of the pipe (0=Control, 1=Isochronous, 2=Bulk, 3=Interrupt).
    UCHAR PipeType;

    // The maximum number of bytes this endpoint can send/receive in a single transaction.
    USHORT MaximumPacketSize;
} FILTER_PIPE_INFO, * PFILTER_PIPE_INFO;

// Payload for USB Select Configuration events.
typedef struct _FILTER_CONFIG_EVENT_DATA {
    // The physical USB port number the device is connected to.
    ULONG PortNumber;

    // The total number of endpoints configured across all interfaces.
    ULONG NumberOfPipesConfigured;

    // A dynamically sized array containing properties for every configured pipe.
    FILTER_PIPE_INFO Pipes[ANYSIZE_ARRAY];
} FILTER_CONFIG_EVENT_DATA, * PFILTER_CONFIG_EVENT_DATA;

// The master structure sent from the driver to user mode representing a single intercepted event.
typedef struct _BUSFILTER_EVENT_PACKET {
    // Indicates which member of the Data union is currently populated.
    FILTER_EVENT_TYPE EventType;

    // Cast of the physical device object (PDO) pointer. Serves as a unique hardware identifier.
    ULONGLONG DeviceId;

    // Absolute system time of the interception, allowing user mode to sequence events.
    LARGE_INTEGER Timestamp;

    // Union containing the specific event data. Only one struct is valid per packet.
    union {
        FILTER_PNP_EVENT_DATA Pnp;
        FILTER_URB_EVENT_DATA Urb;
        FILTER_CONFIG_EVENT_DATA Config;
        FILTER_START_EVENT_DATA StartEvent;
    } Data;
} BUSFILTER_EVENT_PACKET, * PBUSFILTER_EVENT_PACKET;

// Structure sent by user mode back to the driver 
typedef struct _USERMODE_VERDICT {
    // Matches the EventId provided in the FILTER_URB_EVENT_DATA struct.
    UINT64 EventId;

    BOOLEAN Allow;
} USERMODE_VERDICT, * PUSERMODE_VERDICT;

// Structure sent by user mode back to the driver via IOCTL_CUSTOM_REMOVE_DEVICE.
typedef struct _USERMODE_REMOVE_REQUEST {
    ULONGLONG DeviceId;
} USERMODE_REMOVE_REQUEST, * PUSERMODE_REMOVE_REQUEST;
