#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <iomanip>
#include <unordered_map>
#include <string>
#include "..\Driver\api.h"

#define DBG 1

// ============================================================================
// Driver Definitions (Ported from include.h)
// ============================================================================

#define FILE_DEVICE_INVERTED 0xCF54

#define IOCTL_CUSTOM_GET_EVENT \
    CTL_CODE(FILE_DEVICE_INVERTED, 2049, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_CUSTOM_RELEASE_URB \
    CTL_CODE(FILE_DEVICE_INVERTED, 2050, METHOD_BUFFERED, FILE_ANY_ACCESS)

const DWORD BUFFER_SIZE = 8192; 

// Structure to tie a buffer and an overlapped struct together
struct IoContext {
    OVERLAPPED Overlapped;
    BYTE Buffer[BUFFER_SIZE];
};

// Maps physical USB scan codes (0x04 to 0x38) to ASCII characters (Unshifted)
const char HID_TO_ASCII_UNSHIFTED[128] = {
    0, 0, 0, 0, 
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 
    '\n', 27, '\b', '\t', ' ', // Enter, Esc, Backspace, Tab, Space
    '-', '=', '[', ']', '\\', 0, ';', '\'', '`', ',', '.', '/'
};

// Maps physical USB scan codes to ASCII characters (Shift held down)
const char HID_TO_ASCII_SHIFTED[128] = {
    0, 0, 0, 0, 
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', 
    '\n', 27, '\b', '\t', ' ', 
    '_', '+', '{', '}', '|', 0, ':', '"', '~', '<', '>', '?'
};

const std::unordered_map<unsigned char, std::string> PnpMinorFunctionMap = {
    {0x00, "IRP_MN_START_DEVICE"},
    {0x01, "IRP_MN_QUERY_REMOVE_DEVICE"},
    {0x02, "IRP_MN_REMOVE_DEVICE"},
    {0x03, "IRP_MN_CANCEL_REMOVE_DEVICE"},
    {0x04, "IRP_MN_STOP_DEVICE"},
    {0x05, "IRP_MN_QUERY_STOP_DEVICE"},
    {0x06, "IRP_MN_CANCEL_STOP_DEVICE"},
    {0x07, "IRP_MN_QUERY_DEVICE_RELATIONS"},
    {0x08, "IRP_MN_QUERY_INTERFACE"},
    {0x09, "IRP_MN_QUERY_CAPABILITIES"},
    {0x0A, "IRP_MN_QUERY_RESOURCES"},
    {0x0B, "IRP_MN_QUERY_RESOURCE_REQUIREMENTS"},
    {0x0C, "IRP_MN_QUERY_DEVICE_TEXT"},
    {0x0D, "IRP_MN_FILTER_RESOURCE_REQUIREMENTS"},
    {0x0F, "IRP_MN_READ_CONFIG"},
    {0x10, "IRP_MN_WRITE_CONFIG"},
    {0x11, "IRP_MN_EJECT"},
    {0x12, "IRP_MN_SET_LOCK"},
    {0x13, "IRP_MN_QUERY_ID"},
    {0x14, "IRP_MN_QUERY_PNP_DEVICE_STATE"},
    {0x15, "IRP_MN_QUERY_BUS_INFORMATION"},
    {0x16, "IRP_MN_DEVICE_USAGE_NOTIFICATION"},
    {0x17, "IRP_MN_SURPRISE_REMOVAL"}
};

// ============================================================================
// Application Entry Point
// ============================================================================

int main() {
    // Elevate Process Priority (Allowed for standard users)
    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
        std::cerr << "Failed to set process priority. Error: " << GetLastError() << "\n";
    }

    // Elevate Main Thread Priority (Allowed for standard users)
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
        std::cerr << "Failed to set thread priority. Error: " << GetLastError() << "\n";
    }

    std::cout << "Connecting to driver..." << std::endl;

    // Open handle to the driver using the symbolic link
    HANDLE hDevice = CreateFileW(
        USERMODE_DEVICE_PATH,
        GENERIC_READ | GENERIC_WRITE,
        0,                      // Exclusive access to match driver expectation
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        NULL
    );

    if (hDevice == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to open device. Error code: " << GetLastError() << std::endl;
        std::cerr << "Make sure the driver is loaded and you have administrator privileges." << std::endl;
        return 1;
    }

    // Create the I/O Completion Port
    HANDLE hIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (hIocp == NULL) {
        std::cerr << "Failed to create IOCP. Error: " << GetLastError() << std::endl;
        CloseHandle(hDevice);
        return 1;
    }

    // Bind the device handle to the IOCP
    // We pass the device handle itself as the CompletionKey for reference
    if (CreateIoCompletionPort(hDevice, hIocp, (ULONG_PTR)hDevice, 0) == NULL) {
        std::cerr << "Failed to bind device to IOCP. Error: " << GetLastError() << std::endl;
        CloseHandle(hIocp);
        CloseHandle(hDevice);
        return 1;
    }

    std::cout << "Successfully connected to driver and IOCP. Listening for events...\n\n";

    // Allocate a buffer large enough for the struct header + max URB payload
    const int QUEUE_DEPTH = 128;

    IoContext contexts[QUEUE_DEPTH];
    
    // Initialize the contexts
    for (int i = 0; i < QUEUE_DEPTH; i++) {
        ZeroMemory(&contexts[i].Overlapped, sizeof(OVERLAPPED));
        // hEvent is left as NULL; the IOCP handles notification automatically
    }

    // Enqueue inital Requests to the kernel
    for (int i = 0; i < QUEUE_DEPTH; i++) {
        DWORD bytesReturned = 0;
        BOOL success = DeviceIoControl(
            hDevice,
            IOCTL_CUSTOM_GET_EVENT, //
            NULL,
            0,
            contexts[i].Buffer,
            BUFFER_SIZE,
            &bytesReturned,
            &contexts[i].Overlapped
        );

        // If it returns FALSE, verify it is just pending in the kernel
        if (!success && GetLastError() != ERROR_IO_PENDING) {
            std::cerr << "Failed to queue initial IOCTL " << i << ". Error: " << GetLastError() << std::endl;
        }
    }

    // main loop
    while (true) {
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        LPOVERLAPPED pOverlapped = NULL;

        // Block until exactly one IOCTL completes and is pushed to the port
        BOOL bSuccess = GetQueuedCompletionStatus(
            hIocp,
            &bytesTransferred,
            &completionKey,
            &pOverlapped,
            INFINITE
        );
        
        // If GetQueuedCompletionStatus fails and pOverlapped is NULL, the port itself failed
        if (!bSuccess && pOverlapped == NULL) {
            std::cerr << "IOCP wait failed. Error: " << GetLastError() << std::endl;
            break;
        }

        // If pOverlapped is valid, an I/O operation finished (either successfully or with an error)
        if (pOverlapped != NULL) {
            // Because OVERLAPPED is the first member of IoContext, we can safely cast the pointer back
            IoContext* ctx = (IoContext*)pOverlapped;

            if (bSuccess && bytesTransferred >= FIELD_OFFSET(BUSFILTER_EVENT_PACKET, Data)) {
                PBUSFILTER_EVENT_PACKET packet = (PBUSFILTER_EVENT_PACKET)ctx->Buffer; 
                
                // Process the event based on its type
                switch (packet->EventType) {

                    case FilterEventPnp: {
                    #if DBG
                        std::cout << "[PNP EVENT] Device ID: 0x" << std::hex << packet->DeviceId << std::dec << "\n";
                        std::cout << "    Minor Function: ";
                        // Look up the human-readable string in the map
                        auto it = PnpMinorFunctionMap.find(packet->Data.Pnp.MinorFunction);
                        if (it != PnpMinorFunctionMap.end()) {
                            std::cout << " " << it->second << "\n";
                        } else {
                            std::cout << "0x" << std::hex << (int)packet->Data.Pnp.MinorFunction << std::dec << "\n";
                        }
                        std::cout << "--------------------------------------------------\n";
                    #endif
                        break;
                    }

                    case FilterEventConfiguration: {
                    #if DBG
                        std::cout << "[CONFIG EVENT] Device ID: 0x" << std::hex << packet->DeviceId << std::dec << "\n";
                        std::cout << "    Pipes Configured: " << packet->Data.Config.NumberOfPipesConfigured << "\n";
                        std::cout << "    USB Port Number: " << packet->Data.Config.PortNumber << "\n";
                        for (ULONG i = 0; i < packet->Data.Config.NumberOfPipesConfigured; i++) {
                            FILTER_PIPE_INFO* pipe = &packet->Data.Config.Pipes[i];
                            std::cout << "      Pipe [" << i << "] -> Interface: " << (int)pipe->InterfaceNumber
                                    << ", Endpoint: 0x" << std::hex << (int)pipe->EndpointAddress << std::dec
                                    << ", Type: " << (int)pipe->PipeType
                                    << ", MaxPacket: " << pipe->MaximumPacketSize << "\n";
                        }
                        std::cout << "--------------------------------------------------\n";
                    #endif
                        break;
                    }

                    case FilterEventUrb: {
                        // By default, allow the packet to go through
                        USERMODE_VERDICT verdict = { 0 };
                        verdict.EventId = packet->Data.Urb.EventId;
                        verdict.Allow = TRUE;
                    
                    #if DBG
                        std::cout << "[URB] device 0x" << std::hex << packet->DeviceId;
                        if((int)packet->Data.Urb.EndpointAddress == 0xFF) {
                            // no specific endpoint set
                            std::cout << " without a specified endpoint\n";
                        } else {
                            std::cout << " and endpoint 0x" << std::hex << (int)packet->Data.Urb.EndpointAddress << '\n';
                        }
                    #endif

                        // Extract Keyboard Input if the URB is of the type
                        if (packet->Data.Urb.UrbFunction == 0x0009 &&       // BULK or Interrupt
                            (packet->Data.Urb.EndpointAddress & 0x80) != 0 && // IN Endpoint
                            packet->Data.Urb.DataLength == 8) {               // Fixed length for keypress packets
                            
                            UCHAR* payload = packet->Data.Urb.Payload;

                            // Extract the modifier byte (Byte 0)
                            UCHAR modifiers = payload[0];
                            
                            // Bit 1 (0x02) is Left Shift, Bit 5 (0x20) is Right Shift
                            bool isShiftPressed = (modifiers & 0x02) || (modifiers & 0x20);
                            
                            // Select the correct array
                            const char* currentTable = isShiftPressed ? HID_TO_ASCII_SHIFTED : HID_TO_ASCII_UNSHIFTED;

                            bool isKeyDown = false;
                            
                            // 3. Parse the key array (Bytes 2 to 7)
                            for (int i = 2; i < 8; i++) {
                                UCHAR scanCode = payload[i];
                                
                                if (scanCode != 0x00) {
                                    isKeyDown = true;

                                #if DBG
                                    std::cout << "[KEYBOARD] Action: KEY DOWN\n";
                                #endif

                                    // Safety check to prevent array out-of-bounds
                                    if (scanCode < 128) {
                                        char asciiChar = currentTable[scanCode];
                                        
                                        // For the prototype, block the 'd' character
                                        if(asciiChar == 'd'){
                                            verdict.Allow = FALSE;
                                        }
                                    #if DBG
                                        if (asciiChar != 0) {
                                            std::cout << "  Character: '" << asciiChar << "' (Scan Code: 0x" << std::hex << (int)scanCode << std::dec << ")\n";
                                        } else {
                                            std::cout << "  Non-Printable Key (Scan Code: 0x" << std::hex << (int)scanCode << std::dec << ")\n";
                                        }
                                    #endif
                                    }
                                }
                            }
                            
                        #if DBG
                            if (!isKeyDown) {
                                std::cout << "[KEYBOARD] Action: KEY UP (All keys released)\n";
                            }
                        #endif
                        }
                    #if DBG
                        std::cout << "--------------------------------------------------\n";
                    #endif
                        DWORD verdictBytesReturned = 0;
                        BOOL verdictSuccess = DeviceIoControl(
                            hDevice,
                            IOCTL_CUSTOM_RELEASE_URB,
                            &verdict,
                            sizeof(USERMODE_VERDICT),
                            NULL,
                            0,
                            &verdictBytesReturned,
                            NULL
                        );

                        if (!verdictSuccess) {
                            std::cerr << "Failed to release URB. Error: " << GetLastError() << std::endl;
                        }

                        if (verdict.Allow == FALSE) { 
                            // request to remove device 
                            USERMODE_REMOVE_REQUEST req = { 0 };
                            req.DeviceId = packet->DeviceId;
                            
                            DWORD bytesReturned = 0;
                            BOOL success = DeviceIoControl(
                                hDevice,
                                IOCTL_CUSTOM_REMOVE_DEVICE,
                                &req,
                                sizeof(USERMODE_REMOVE_REQUEST),
                                NULL,
                                0,
                                &bytesReturned,
                                NULL
                            );
                        #if DBG
                            if (success) {
                                std::cout << "\n[SUCCESS] Retroactive removal triggered for Device ID: 0x" 
                                        << std::hex << packet->DeviceId << "\n\n";
                            } else {
                                std::cerr << "\n[ERROR] Removal failed. Ensure the Device ID is valid. Error: " 
                                        << GetLastError() << "\n\n";
                            }
                        #endif
                        }
                    
                        break;
                    }

                    case FilterEventStartDevice: {
                        USERMODE_VERDICT verdict = { 0 };
                        verdict.EventId = packet->Data.StartEvent.EventId;
                        verdict.Allow = TRUE; // Default to allow

                    #if DBG
                        std::cout << "[START EVENT] Device ID: 0x" << std::hex << packet->DeviceId << std::dec << "\n";
                        
                        PUCHAR currentPos = packet->Data.StartEvent.DataBuffer;

                        // Parse Hardware IDs (REG_MULTI_SZ)
                        if (packet->Data.StartEvent.HardwareIDsLength > 0) {
                            std::cout << "    Hardware IDs:\n";
                            PWCHAR hwId = (PWCHAR)currentPos;
                            // Iterate over the REG_MULTI_SZ block until the double null-terminator
                            while (*hwId != L'\0') {
                                std::wcout << L"      - " << hwId << L"\n";
                                
                                // Example Logic: Block a specific flash drive by its specific Hardware ID
                                if (wcscmp(hwId, L"USB\\VID_0781&PID_5581&REV_0100") == 0) {
                                    std::cout << "      [!] Target device detected. Blocking start.\n";
                                    verdict.Allow = FALSE; 
                                }
                                
                                hwId += wcslen(hwId) + 1; // Advance past the current string's null terminator
                            }
                            currentPos += packet->Data.StartEvent.HardwareIDsLength;
                        }

                        // Parse Compatible IDs (REG_MULTI_SZ)
                        if (packet->Data.StartEvent.CompatibleIDsLength > 0) {
                            std::cout << "    Compatible IDs:\n";
                            PWCHAR compId = (PWCHAR)currentPos;
                            while (*compId != L'\0') {
                                std::wcout << L"      - " << compId << L"\n";
                                compId += wcslen(compId) + 1;
                            }
                            currentPos += packet->Data.StartEvent.CompatibleIDsLength;
                        }

                        // Parse Container ID (REG_SZ)
                        if (packet->Data.StartEvent.ContainerIdLength > 0) {
                            PWCHAR containerId = (PWCHAR)currentPos;
                            std::wcout << L"    Container ID: " << containerId << L"\n";
                        }
                        
                        std::cout << "--------------------------------------------------\n";
                    #endif

                        // Send the verdict back to the kernel
                        DWORD verdictBytesReturned = 0;
                        BOOL verdictSuccess = DeviceIoControl(
                            hDevice,
                            IOCTL_CUSTOM_DEVICE_START_VERDICT,
                            &verdict,
                            sizeof(USERMODE_VERDICT),
                            NULL,
                            0,
                            &verdictBytesReturned,
                            NULL
                        );

                        if (!verdictSuccess) {
                            std::cerr << "Failed to send Start Verdict. Error: " << GetLastError() << std::endl;
                        }
                        break;
                    }

                    default: {
                        std::cerr << "Unknown event type received: " << packet->EventType << std::endl;
                        break;
                    }
                }
            }

            // IMMEDIATELY re-queue this context to keep the driver fed
            ZeroMemory(&ctx->Overlapped, sizeof(OVERLAPPED));
            
            DWORD dummy = 0;
            DeviceIoControl(
                hDevice,
                IOCTL_CUSTOM_GET_EVENT, 
                NULL,
                0,
                ctx->Buffer,
                BUFFER_SIZE,
                &dummy,
                &ctx->Overlapped
            );
        }
    }

    CloseHandle(hDevice);
    return 0;
}