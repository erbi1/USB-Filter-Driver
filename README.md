# USB Filter Detection Framework Prototype
This project is a prototype for a Windows USB detection and prevention framework. It consists of a kernel-mode filter driver (KMDF/WDM hybrid) and a user-mode application. The system intercepts USB device enumeration and data traffic in real-time, allowing the user-mode application to inspect payloads and issue Allow or Block verdicts before the operating system processes them.

## Key Features
- Real-Time Device Enumeration Control: Intercepts IRP_MN_START_DEVICE requests natively. It extracts Hardware IDs, Compatible IDs, and Container IDs, sending them to user-mode to determine if a device is permitted to start.
- USB Request Block (URB) Interception: Captures data transfers (such as Bulk and Interrupt transfers) between the USB hardware and the OS. The driver holds the I/O Request Packet (IRP) in a pending state while the user-mode application inspects the payload.
- High-Performance Asynchronous I/O: Utilizes the Inverted Call Model. The user-mode application queues multiple IOCTL_CUSTOM_GET_EVENT requests using I/O Completion Ports (IOCP) to handle concurrent hardware events without thread-blocking.
- Payload Inspection and Manipulation: The user-mode application parses raw USB packets directly. As a demonstration, the prototype currently decodes HID keyboard scan codes and selectively blocks specific keystrokes (such as the 'd' key) by clearing the data buffer before passing the verdict back to the kernel.
- Dynamic Device Removal: Supports retroactive device banning. If malicious or restricted traffic is detected post-enumeration, the user-mode application can send an IOCTL_CUSTOM_REMOVE_DEVICE command, prompting the driver to invalidate the device state and force an immediate disconnect.

## Extensibility for a Detection Framework
This prototype is structurally designed to serve as the foundation for a more comprehensive security or monitoring framework:
- Modular Verdict System: The USERMODE_VERDICT structure and associated IOCTLs provide a standardized communication channel to enforce policies on any intercepted event.
- Scalable Event Processing: The IOCP implementation in user-mode allows the application to scale efficiently, enabling the integration of complex detection logic (such as signature scanning or behavioral heuristics) across multiple devices concurrently.
- Extensible Event Types: The BUSFILTER_EVENT_PACKET master structure uses a union memory layout to easily accommodate new interception types (e.g., USB select configuration events, PnP state changes) without altering the core driver-to-application communication pipeline.
