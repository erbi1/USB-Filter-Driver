#include "internal.h"

// ============================================================================
// Driver Entry & Initialization
// ============================================================================

PDRIVER_DISPATCH WdfDefaultDispatch[IRP_MJ_MAXIMUM_FUNCTION + 1];
WDFDEVICE g_ControlDevice = NULL;

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    // --- Create driver instance ---

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "DriverEntry\n"));

    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    WDFDRIVER hdriver;
    WDF_OBJECT_ATTRIBUTES driverAttributes;

    // Add Callbacks for adding devices and unloading the driver
    WDF_DRIVER_CONFIG_INIT(&config, HubFilterEvtDeviceAdd);
    config.EvtDriverUnload = BusFilterDriverEvtDriverUnload;

    // tie driver context struct to driver
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&driverAttributes, DRIVER_CONTEXT);

    status = WdfDriverCreate(
        DriverObject, 
        RegistryPath, 
        &driverAttributes, 
        &config, 
        &hdriver
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Save the original Dispatch Functions into WdfDefaultDispatch and set custom callback
    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        WdfDefaultDispatch[i] = DriverObject->MajorFunction[i];
        DriverObject->MajorFunction[i] = ChildFilter_GlobalDispatch;
    }

    // --- Initialize the fields of the driver context ---

    // Retrieve the newly allocated context memory
    PDRIVER_CONTEXT driverCtx = GetDriverContext(hdriver);

    // create list tracking the filter devices
    InitializeListHead(&driverCtx->FilterDeviceListHead);

    // Now initialize the attributes for the Collection and Spinlock
    WDF_OBJECT_ATTRIBUTES objectAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&objectAttributes);
    objectAttributes.ParentObject = hdriver; // Tie them to the driver for cleanup

    // Create the Spinlock
    status = WdfSpinLockCreate(&objectAttributes, &driverCtx->FilterDeviceLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // --- Control Device Initialization ---

    // Allocate initialization structure. 
    // The SDDL string below grants RW access to all
    PWDFDEVICE_INIT controlInit = WdfControlDeviceInitAllocate(hdriver, &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R);
    if (controlInit == NULL) {
        return STATUS_UNSUCCESSFUL;
    }

    // only one app is allowed to connect at a time
    WdfDeviceInitSetExclusive(controlInit, TRUE);

    status = WdfDeviceInitAssignName(controlInit, &ntDeviceName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(controlInit);
        return status;
    }

    // Register create and cleanup callback
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig,
        ControlDeviceEvtFileCreate,
        WDF_NO_EVENT_CALLBACK,
        ControlDeviceEvtFileCleanup);

    WdfDeviceInitSetFileObjectConfig(controlInit, &fileConfig, WDF_NO_OBJECT_ATTRIBUTES);

    // Set context attributes
    WDF_OBJECT_ATTRIBUTES controlAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&controlAttributes, CONTROL_DEVICE_CONTEXT);

    // Create the Control Device
    status = WdfDeviceCreate(&controlInit, &controlAttributes, &g_ControlDevice);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(controlInit);
        return status;
    }

    // Create Symbolic Link for user-mode access
    status = WdfDeviceCreateSymbolicLink(g_ControlDevice, &symbolicLinkName);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(g_ControlDevice);
        g_ControlDevice = NULL;
        return status;
    }

    PCONTROL_DEVICE_CONTEXT controlCtx = GetControlContext(g_ControlDevice);

    // --- Queue Setup for Control Device ---

    // Default Queue for incoming IOCTLs (Inverted Call Model Requests)
    WDF_IO_QUEUE_CONFIG ioctlQueueConfig;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&ioctlQueueConfig, WdfIoQueueDispatchParallel);
    ioctlQueueConfig.EvtIoDeviceControl = InvertedIoDeviceControl; // user-mode IOCTL handler
    ioctlQueueConfig.PowerManaged = WdfFalse;

    status = WdfIoQueueCreate(g_ControlDevice, &ioctlQueueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(g_ControlDevice);
        g_ControlDevice = NULL;
        return status;
    }

    // Manual Queue to hold pending requests for notifications
    WDF_IO_QUEUE_CONFIG manualQueueConfig;
    WDF_IO_QUEUE_CONFIG_INIT(&manualQueueConfig, WdfIoQueueDispatchManual);
    manualQueueConfig.PowerManaged = WdfFalse;

    status = WdfIoQueueCreate(g_ControlDevice, &manualQueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &controlCtx->NotificationQueue);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(g_ControlDevice);
        g_ControlDevice = NULL;
        return status;
    }

    // Mark initialization complete
    WdfControlFinishInitializing(g_ControlDevice);

    return status;
}

VOID
BusFilterDriverEvtDriverUnload(
    _In_ WDFDRIVER Driver
)
{
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BusFilterDriverEvtDriverUnload: Unloading driver.\n"));

    PDRIVER_CONTEXT driverCtx = GetDriverContext(Driver);

    // Acquire the lock to safely traverse and modify the collection
    WdfSpinLockAcquire(driverCtx->FilterDeviceLock);

    while (!IsListEmpty(&driverCtx->FilterDeviceListHead)) {
        // remove current entry and extract from list at the same time
        PLIST_ENTRY entry = RemoveHeadList(&driverCtx->FilterDeviceListHead);

        // get extention
        PCHILD_FILTER_EXTENSION ext = CONTAINING_RECORD(entry, CHILD_FILTER_EXTENSION, GlobalListEntry);

        // get pointer to device
        PDEVICE_OBJECT wdmDevice = ext->WdmDeviceObject;

        // detach from the USB stack if it hasn't been done already
        if (ext->LowerDevice != NULL) {
            IoDetachDevice(ext->LowerDevice);
            ext->LowerDevice = NULL;
        }

        // delete the raw WDM device object
        if (wdmDevice != NULL) {
            IoDeleteDevice(wdmDevice);
        }
    }

    WdfSpinLockRelease(driverCtx->FilterDeviceLock);
}

NTSTATUS
ChildFilter_GlobalDispatch(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
)
{
    PCHILD_FILTER_EXTENSION ext = (PCHILD_FILTER_EXTENSION)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);

    // Verify if this is our raw WDM filter using the Magic Number
    if (ext != NULL && ext->MagicNumber == WDM_FILTER_MAGIC && ext->IsWdmFilter == TRUE) {

        // Route specific IRPs to our custom WDM handlers
        if (irpSp->MajorFunction == IRP_MJ_PNP) {
            return ChildFilter_DispatchPnp(DeviceObject, Irp);
        }
        if (irpSp->MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL) {
            return ChildFilter_DispatchInternalDeviceControl(DeviceObject, Irp);
        }

        // All other IRPs are not of interest for this filter and are passed safely down the stack
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(ext->LowerDevice, Irp);
    }

    // If the magic number check failed, then the Device is not owned by this Driver and needs 
    //    to be passed to the default dispatch function
    return WdfDefaultDispatch[irpSp->MajorFunction](DeviceObject, Irp);
}

// ============================================================================
// Control Device
// ============================================================================

VOID
ControlDeviceEvtFileCreate(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ WDFFILEOBJECT FileObject
)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(Device);

    PDRIVER_CONTEXT driverCtx = GetDriverContext(WdfGetDriver());

    // Mark the application as officially connected
    driverCtx->IsAppConnected = TRUE;

    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID
ControlDeviceEvtFileCleanup(
    _In_ WDFFILEOBJECT FileObject
)
{
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "ControlDeviceEvtFileCleanup: User-mode handle closed. Draining pending URBs.\n"));

    UNREFERENCED_PARAMETER(FileObject);

    PDRIVER_CONTEXT driverCtx = GetDriverContext(WdfGetDriver());
    driverCtx->IsAppConnected = FALSE;

    // Create a temporary local list to hold IRPs we need to complete.
    // This allows us to complete them later at the correct IRQL without holding locks.
    //    (Aquiring the Lock lowers the IRQL, meaning completing the IRP within the first loop is not possible)
    LIST_ENTRY irpsToComplete;
    InitializeListHead(&irpsToComplete);

    // Lock the global collection of filter devices
    WdfSpinLockAcquire(driverCtx->FilterDeviceLock);
    PLIST_ENTRY currentGlobalEntry = driverCtx->FilterDeviceListHead.Flink;

    while (currentGlobalEntry != &driverCtx->FilterDeviceListHead) { // traverse filter device contexts
        PCHILD_FILTER_EXTENSION currentFilterExt = CONTAINING_RECORD(currentGlobalEntry, CHILD_FILTER_EXTENSION, GlobalListEntry);

        KIRQL oldIrql;

        // Lock the specific device's pending event list
        KeAcquireSpinLock(&currentFilterExt->EventListLock, &oldIrql);

        PLIST_ENTRY currentEntry = currentFilterExt->PendingEventListHead.Flink;

        while (currentEntry != &currentFilterExt->PendingEventListHead) { // traverse pending events
            PPENDING_IRP_EVENT pendingEvent = CONTAINING_RECORD(currentEntry, PENDING_IRP_EVENT, ListEntry);
            PLIST_ENTRY nextEntry = currentEntry->Flink; // Save the next pointer before modifying the list
            PIRP pendingIrp = pendingEvent->Irp;

            // attempt to take ownership of the IRP back from the system cancel routine
            if (IoSetCancelRoutine(pendingIrp, NULL) != NULL) {
                // The IRP is now owned by this instance again
                // Now perform list operations
                RemoveEntryList(currentEntry);

                // Add it to our local completion list.
                InsertTailList(&irpsToComplete, currentEntry);
            }
            // If IoSetCancelRoutine returns NULL, the system cancel routine is already 
            // executing on another CPU core. We ignore it and let the system handle it.

            currentEntry = nextEntry;
        }

        currentGlobalEntry = currentGlobalEntry->Flink;

        KeReleaseSpinLock(&currentFilterExt->EventListLock, oldIrql);
    }

    WdfSpinLockRelease(driverCtx->FilterDeviceLock);

    // Complete all gathered IRPs safely outside of the spinlocks
    while (!IsListEmpty(&irpsToComplete)) {
        PLIST_ENTRY entry = RemoveHeadList(&irpsToComplete);
        PPENDING_IRP_EVENT pendingEvent = CONTAINING_RECORD(entry, PENDING_IRP_EVENT, ListEntry);
        PIRP irpToComplete = pendingEvent->Irp;

        // Mark the IRP as cancelled
        irpToComplete->IoStatus.Status = STATUS_CANCELLED;
        irpToComplete->IoStatus.Information = 0;

        // Free the memory we allocated for this tracking struct
        ExFreePoolWithTag(pendingEvent, 'kEvt');

        // Allow the I/O Manager to finish returning the IRP to the caller
        IoCompleteRequest(irpToComplete, IO_NO_INCREMENT);
    }
}

// ============================================================================
// KMDF Hub Filter Implementation
// ============================================================================

NTSTATUS
HubFilterEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "HubFilterEvtDeviceAdd\n"));

    UNREFERENCED_PARAMETER(Driver);

    // Mark this Device as a Filter Device
    WdfFdoInitSetFilter(DeviceInit);

    // Register preprocess callback to intercept PnP IRPs for the Hub
    UCHAR minorFunctions[] = { IRP_MN_QUERY_DEVICE_RELATIONS };
    NTSTATUS status = WdfDeviceInitAssignWdmIrpPreprocessCallback(
        DeviceInit,
        HubFilter_WdmPnpPreprocess,
        IRP_MJ_PNP,
        minorFunctions,
        1
    );

    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "HubFilterEvtDeviceAdd: %x\n", status));
        return status;
    }

    WDFDEVICE device;
    return WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
}

NTSTATUS
HubFilter_WdmPnpPreprocess(
    _In_ WDFDEVICE Device,
    _Inout_ PIRP Irp
)
{
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "HubFilter_WdmPnpPreprocess\n"));

    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(Irp);

    if (irpStack->Parameters.QueryDeviceRelations.Type == BusRelations) {
        // Intercept logical Bus Events only (such as a new device connecting)
        // Power Events, etc are ignored

        IoCopyCurrentIrpStackLocationToNext(Irp);

        // Set custom completion routine
        IoSetCompletionRoutine(Irp, HubFilter_BusRelationsCompletion, (PVOID)Device, TRUE, TRUE, TRUE);

        WDFIOTARGET Target = WdfDeviceGetIoTarget(Device);
        PDEVICE_OBJECT LowerDevice = WdfIoTargetWdmGetTargetDeviceObject(Target);

        // After setting the custom routine, send request down the stack
        return IoCallDriver(LowerDevice, Irp);
    }

    return WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
}

PWCHAR
Helper_QueryDevicePropertyString(
    _In_ PDEVICE_OBJECT Pdo,
    _In_ DEVICE_REGISTRY_PROPERTY DeviceProperty,
    _Out_ PULONG OutLength
)
{
    ULONG returnedLength = 0;
    *OutLength = 0;
    NTSTATUS status;

    // First pass: Query the required buffer size
    status = IoGetDeviceProperty(
        Pdo,
        DeviceProperty,
        0,
        NULL,
        &returnedLength
    );

    // If the property exists, the status will be STATUS_BUFFER_TOO_SMALL
    if (status != STATUS_BUFFER_TOO_SMALL || returnedLength == 0) {
        return NULL;
    }

    // Allocate memory from the non-paged pool. 
    // This is required because completion routines can run at DISPATCH_LEVEL.
    PWCHAR buffer = (PWCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, returnedLength, 'pPrD');

    if (buffer == NULL) {
        return NULL;
    }

    // Second pass: Retrieve the actual property data into the allocated buffer
    status = IoGetDeviceProperty(
        Pdo,
        DeviceProperty,
        returnedLength,
        buffer,
        &returnedLength
    );

    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(buffer, 'pPrD');
        return NULL;
    }

    *OutLength = returnedLength;

    return buffer;
}

_Use_decl_annotations_
NTSTATUS
HubFilter_BusRelationsCompletion(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context
)
{
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "HubFilter_BusRelationsCompletion\n"));

    UNREFERENCED_PARAMETER(Context);

    if (NT_SUCCESS(Irp->IoStatus.Status) && Irp->IoStatus.Information != 0) { // A new (composite) device has connected
        PDEVICE_RELATIONS relations = (PDEVICE_RELATIONS)Irp->IoStatus.Information;

        for (ULONG i = 0; i < relations->Count; i++) { // iterate over possible child devices
            PDEVICE_OBJECT childPdo = relations->Objects[i];

            // prevent duplicates
            PDRIVER_CONTEXT driverCtx = GetDriverContext(WdfGetDriver());
            BOOLEAN alreadyAttached = FALSE;

            WdfSpinLockAcquire(driverCtx->FilterDeviceLock);
            PLIST_ENTRY currentEntry = driverCtx->FilterDeviceListHead.Flink;

            while (currentEntry != &driverCtx->FilterDeviceListHead) {
                PCHILD_FILTER_EXTENSION existingExt = CONTAINING_RECORD(currentEntry, CHILD_FILTER_EXTENSION, GlobalListEntry);
                if (existingExt->Pdo == childPdo) {
                    alreadyAttached = TRUE;
                    break;
                }
                currentEntry = currentEntry->Flink;
            }
            WdfSpinLockRelease(driverCtx->FilterDeviceLock);

            if (alreadyAttached) {
                continue; // Skip this PDO, our filter is already attached
            }

            PDEVICE_OBJECT childFilterDevice;

            // Create raw WDM child filter
            NTSTATUS status = IoCreateDevice(
                DeviceObject->DriverObject,
                sizeof(CHILD_FILTER_EXTENSION),
                NULL,
                childPdo->DeviceType,
                childPdo->Characteristics,
                FALSE,
                &childFilterDevice
            );

            if (NT_SUCCESS(status)) {
                KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "HubFilter_BusRelationsCompletion: Created raw WDM child filter\n"));

                PCHILD_FILTER_EXTENSION ext = (PCHILD_FILTER_EXTENSION)childFilterDevice->DeviceExtension;

                // --- initialize filter extention fields
                ext->WdmDeviceObject = childFilterDevice;
                ext->MagicNumber = WDM_FILTER_MAGIC;
                ext->IsWdmFilter = TRUE; // Mark this as our raw WDM device
                ext->IsBanned = FALSE;

                // Attach to the USB device stack
                PDEVICE_OBJECT lowerDevice = IoAttachDeviceToDeviceStack(childFilterDevice, childPdo);

                if (lowerDevice != NULL) {
                    ext->LowerDevice = lowerDevice;
                    ext->Pdo = childPdo;

                    // Query the USB Port Number
                    ULONG portNumber = 0;
                    ULONG returnedLength = 0;
                    NTSTATUS propStatus = IoGetDeviceProperty(
                        childPdo,
                        DevicePropertyAddress,
                        sizeof(ULONG),
                        &portNumber,
                        &returnedLength
                    );

                    if (NT_SUCCESS(propStatus)) {
                        ext->PortNumber = portNumber;
                    }
                    else {
                        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "HubFilter_BusRelationsCompletion: Get Port Nr. Error %x\n", propStatus));
                        ext->PortNumber = 0; // 0 indicates the query failed or is not applicable
                    }


                    // Query HW ID 
                    PWCHAR hardwareIDs = Helper_QueryDevicePropertyString(childPdo, DevicePropertyHardwareID, &ext->HardwareIDsLength);
                    if (hardwareIDs != NULL) {
                        ext->HardwareIDs = hardwareIDs;
                    }
                    else {
                        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "HubFilter_BusRelationsCompletion: Get Child Hardware ID failed: %ws\n", ext->HardwareIDs));
                        ext->HardwareIDs = NULL;
                        ext->HardwareIDsLength = 0; 
                    }

                    // Compatible IDs
                    PWCHAR compatibleIDs = Helper_QueryDevicePropertyString(childPdo, DevicePropertyCompatibleIDs, &ext->CompatibleIDsLength);
                    if (compatibleIDs != NULL) {
                        ext->CompatibleIDs = compatibleIDs;
                    }
                    else {
                        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "HubFilter_BusRelationsCompletion: Get Child Compatible ID failed: %ws\n", ext->HardwareIDs));
                        ext->CompatibleIDs = NULL;
                        ext->CompatibleIDsLength = 0;
                    }


                    // Base Container ID
                    PWCHAR containerId = Helper_QueryDevicePropertyString(childPdo, DevicePropertyContainerID, &ext->ContainerIdLength);
                    if (containerId != NULL) {
                        ext->ContainerId = containerId; // Save to extension
                    }
                    else {
                        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "HubFilter_BusRelationsCompletion: Get Child Container ID failed: %ws\n", ext->HardwareIDs));
                        ext->ContainerId = NULL;
                        ext->ContainerIdLength = 0;
                    }

                    // Set some WDF flags
                    childFilterDevice->Flags &= ~DO_DEVICE_INITIALIZING;
                    childFilterDevice->Flags |= (lowerDevice->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE));

                    // Add the newly created device to the global tracking list
                    WdfSpinLockAcquire(driverCtx->FilterDeviceLock);
                    InsertTailList(&driverCtx->FilterDeviceListHead, &ext->GlobalListEntry);
                    WdfSpinLockRelease(driverCtx->FilterDeviceLock);

                    // Create the Event List Spinlock
                    KeInitializeSpinLock(&ext->EventListLock);

                    // Create the Endpoint Mapping Spinlock
                    KeInitializeSpinLock(&ext->PipeMappingLock);

                    // Create the List
                    InitializeListHead(&ext->PendingEventListHead);
                }
                else {
                    IoDeleteDevice(childFilterDevice);
                }
            }
        }
    }

    if (Irp->PendingReturned) IoMarkIrpPending(Irp);
    return STATUS_CONTINUE_COMPLETION;
}

// ============================================================================
// WDM Child Filter Implementation (The Interceptor)
// ============================================================================

NTSTATUS
ChildFilter_PnpSynchronousCompletion(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context
)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    PKEVENT event = (PKEVENT)Context;
    KeSetEvent(event, IO_NO_INCREMENT, FALSE);

    // STATUS_MORE_PROCESSING_REQUIRED stops the I/O manager from continuing 
    // to unwind the IRP, giving control back to the dispatch thread.
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
ChildFilter_DispatchPnp(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
)
{
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "ChildFilter_DispatchPnp: Entry\n"));
    PCHILD_FILTER_EXTENSION ext = (PCHILD_FILTER_EXTENSION)DeviceObject->DeviceExtension;

    if (ext == NULL || ext->MagicNumber != WDM_FILTER_MAGIC || ext->IsWdmFilter != TRUE) {
        // If filter is not owned by this instance, then pass use default routine
        return WdfDefaultDispatch[IRP_MJ_PNP](DeviceObject, Irp);
    }

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status;


    if (irpSp->MinorFunction != IRP_MN_START_DEVICE) {

#if DBG
        PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
        if (stack->MinorFunction == IRP_MN_START_DEVICE) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[Debug] Intercepted START_DEVICE. IRP: %p\n", Irp));
        }
#endif

        // --- in any other case, send notification to user ioctl without expecting response
        size_t packetSize = FIELD_OFFSET(BUSFILTER_EVENT_PACKET, Data) + sizeof(FILTER_PNP_EVENT_DATA);
        PBUSFILTER_EVENT_PACKET packet = (PBUSFILTER_EVENT_PACKET)ExAllocatePool2(POOL_FLAG_NON_PAGED, packetSize, 'tkPE');

        if (packet != NULL) {
            packet->EventType = FilterEventPnp;
            packet->DeviceId = (ULONGLONG)ext->Pdo;
            KeQuerySystemTime(&packet->Timestamp);

            packet->Data.Pnp.MinorFunction = irpSp->MinorFunction;

            // the return value is not checked because this acts as extra information to usermode
            // if the usermode doesnt receive this event, then there is no change in the filtering logic
            InvertedEventNotify(packet, packetSize);

            ExFreePoolWithTag(packet, 'tkPE');
        }
    }

    // --- handle bus functionality
    switch (irpSp->MinorFunction) {
    case IRP_MN_QUERY_PNP_DEVICE_STATE: // handle user initiated device removal
    {
        KEVENT completionEvent;
        KeInitializeEvent(&completionEvent, NotificationEvent, FALSE);

        // Prepare to pass the IRP down the stack
        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(
            Irp,
            ChildFilter_PnpSynchronousCompletion,
            &completionEvent,
            TRUE, TRUE, TRUE
        );

        // Send the IRP to the bus driver and wait for it to return
        status = IoCallDriver(ext->LowerDevice, Irp);

        if (status == STATUS_PENDING) {
            KeWaitForSingleObject(&completionEvent, Executive, KernelMode, FALSE, NULL);
            status = Irp->IoStatus.Status;
        }

        // Modify the state on the way back UP the stack
        if (ext->IsBanned == TRUE) {
            status = STATUS_SUCCESS;
            Irp->IoStatus.Status = STATUS_SUCCESS;

            PPNP_DEVICE_STATE deviceState = (PPNP_DEVICE_STATE)&Irp->IoStatus.Information;
            // Append the FAILED flag to whatever state the lower drivers established
            *deviceState |= PNP_DEVICE_FAILED;
        }

        // Resume completing the request
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    case IRP_MN_REMOVE_DEVICE: 
    {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "ChildFilter_DispatchPnp: USB Device Unplugged. Detaching child filter: %p\n", DeviceObject));
        // --- Handle cleaning up the device extention if parent device disconnected ---
        // Free Dynamically Allocated Strings
        if (ext->HardwareIDs != NULL) {
            ExFreePoolWithTag(ext->HardwareIDs, 'pPrD');
            ext->HardwareIDs = NULL;
        }
        if (ext->CompatibleIDs != NULL) {
            ExFreePoolWithTag(ext->CompatibleIDs, 'pPrD');
            ext->CompatibleIDs = NULL;
        }
        if (ext->ContainerId != NULL) {
            ExFreePoolWithTag(ext->ContainerId, 'pPrD');
            ext->ContainerId = NULL;
        }

        // Cancel and drain all pending IRPs for this specific device
        KIRQL oldIrql;
        KeAcquireSpinLock(&ext->EventListLock, &oldIrql);

        while (!IsListEmpty(&ext->PendingEventListHead)) {
            PLIST_ENTRY entry = RemoveHeadList(&ext->PendingEventListHead);
            PPENDING_IRP_EVENT pendingEvent = CONTAINING_RECORD(entry, PENDING_IRP_EVENT, ListEntry);

            PIRP pendingIrp = pendingEvent->Irp;

            // Attempt to clear the cancel routine. 
            // If it returns NULL, the cancel routine is already executing and will complete the IRP.
            if (IoSetCancelRoutine(pendingIrp, NULL) != NULL) {
                pendingIrp->IoStatus.Status = STATUS_CANCELLED;
                pendingIrp->IoStatus.Information = 0;

                // Do not complete the IRP while holding the spinlock
                // We queue it locally or complete it after releasing the lock.
                // For simplicity here, we release, complete, and re-acquire.
                KeReleaseSpinLock(&ext->EventListLock, oldIrql);
                IoCompleteRequest(pendingIrp, IO_NO_INCREMENT);
                KeAcquireSpinLock(&ext->EventListLock, &oldIrql);
            }

            ExFreePoolWithTag(pendingEvent, 'kEvt');
        }
        KeReleaseSpinLock(&ext->EventListLock, oldIrql);

        // Remove the device from the global collection
        PDRIVER_CONTEXT driverCtx = GetDriverContext(WdfGetDriver());
        WdfSpinLockAcquire(driverCtx->FilterDeviceLock);
        RemoveEntryList(&ext->GlobalListEntry);
        WdfSpinLockRelease(driverCtx->FilterDeviceLock);

        // Pass the Remove IRP down the stack first
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->LowerDevice, Irp);

        // Detach from the lower USB device stack
        IoDetachDevice(ext->LowerDevice);

        // Delete our dynamically created WDM child filter device
        IoDeleteDevice(DeviceObject);

        return status;
    }
    case IRP_MN_START_DEVICE: 
    {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "ChildFilter_DispatchPnp: Intercepted IRP_MN_START_DEVICE\n"));

        PDRIVER_CONTEXT driverCtx = GetDriverContext(WdfGetDriver());

        // Free old pointers first and repopulate in case of a PnP rebalance
        if (ext->HardwareIDs != NULL) { ExFreePoolWithTag(ext->HardwareIDs, 'pPrD'); ext->HardwareIDs = NULL; }
        if (ext->CompatibleIDs != NULL) { ExFreePoolWithTag(ext->CompatibleIDs, 'pPrD'); ext->CompatibleIDs = NULL; }
        if (ext->ContainerId != NULL) { ExFreePoolWithTag(ext->ContainerId, 'pPrD'); ext->ContainerId = NULL; }

        ext->HardwareIDs = Helper_QueryDevicePropertyString(ext->Pdo, DevicePropertyHardwareID, &ext->HardwareIDsLength);
        ext->CompatibleIDs = Helper_QueryDevicePropertyString(ext->Pdo, DevicePropertyCompatibleIDs, &ext->CompatibleIDsLength);
        ext->ContainerId = Helper_QueryDevicePropertyString(ext->Pdo, DevicePropertyContainerID, &ext->ContainerIdLength);
        
        if (ext->PortNumber == 0) {
            // retry to get port number
            ULONG portNumber = 0;
            ULONG returnedLength = 0;
            NTSTATUS propStatus = IoGetDeviceProperty(
                ext->Pdo,
                DevicePropertyAddress,
                sizeof(ULONG),
                &portNumber,
                &returnedLength
            );

            if (NT_SUCCESS(propStatus)) {
                ext->PortNumber = portNumber;
            }
            else {
                KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "ChildFilter_DispatchPnp: Get Port Nr. Error %x\n", propStatus));
                ext->PortNumber = 0; // 0 indicates the query failed or is not applicable
            }
        }

        // If no app is connected, immediately pass the IRP down the stack
        if (driverCtx->IsAppConnected == FALSE || ext->HardwareIDsLength == 0 || ext->HardwareIDs == NULL) {
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(ext->LowerDevice, Irp);
        }

        // Prepare the event packet
        size_t packetSize = FIELD_OFFSET(BUSFILTER_EVENT_PACKET, Data.StartEvent.DataBuffer) 
            + ext->HardwareIDsLength
            + ext->CompatibleIDsLength
            + ext->ContainerIdLength;
        PBUSFILTER_EVENT_PACKET packet = (PBUSFILTER_EVENT_PACKET)ExAllocatePool2(POOL_FLAG_NON_PAGED, packetSize, 'tkPE');

        if (packet == NULL) {
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(ext->LowerDevice, Irp);
        }

        UINT64 eventId = (UINT64)InterlockedIncrement64(&ext->NextEventId);

        packet->EventType = FilterEventStartDevice;
        packet->DeviceId = (ULONGLONG)ext->Pdo;
        KeQuerySystemTime(&packet->Timestamp);
        packet->Data.StartEvent.EventId = eventId;
        packet->Data.StartEvent.PortNumber = ext->PortNumber;

        packet->Data.StartEvent.HardwareIDsLength = ext->HardwareIDsLength;
        packet->Data.StartEvent.CompatibleIDsLength = ext->CompatibleIDsLength;
        packet->Data.StartEvent.ContainerIdLength = ext->ContainerIdLength;

        PUCHAR currentBufferPos = packet->Data.StartEvent.DataBuffer;

        if (ext->HardwareIDsLength > 0) {
            RtlCopyMemory(currentBufferPos, ext->HardwareIDs, ext->HardwareIDsLength);
            currentBufferPos += ext->HardwareIDsLength;
        }
        if (ext->CompatibleIDsLength > 0) {
            RtlCopyMemory(currentBufferPos, ext->CompatibleIDs, ext->CompatibleIDsLength);
            currentBufferPos += ext->CompatibleIDsLength;
        }
        if (ext->ContainerIdLength > 0) {
            RtlCopyMemory(currentBufferPos, ext->ContainerId, ext->ContainerIdLength);
        }

        // Prepare the pending event tracker
        PPENDING_IRP_EVENT irpEvent = (PPENDING_IRP_EVENT)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(PENDING_IRP_EVENT), 'kEvt');
        if (irpEvent == NULL) {
            ExFreePoolWithTag(packet, 'tkPE');
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(ext->LowerDevice, Irp);
        }

        irpEvent->EventId = eventId;
        irpEvent->Irp = Irp;
        irpEvent->rawBuffer = NULL;
        irpEvent->urbDataLength = 0;

        // Mark the IRP pending and queue it
        IoMarkIrpPending(Irp);

        KIRQL oldIrql;
        KeAcquireSpinLock(&ext->EventListLock, &oldIrql);

        IoSetCancelRoutine(Irp, ChildFilter_CancelHeldIrp);
        if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL) != NULL) {
            // IRP was already canceled
            KeReleaseSpinLock(&ext->EventListLock, oldIrql);
            ExFreePoolWithTag(irpEvent, 'kEvt');
            ExFreePoolWithTag(packet, 'tkPE');
            return STATUS_CANCELLED;
        }

        InsertTailList(&ext->PendingEventListHead, &irpEvent->ListEntry);
        KeReleaseSpinLock(&ext->EventListLock, oldIrql);

        // Notify user mode
        NTSTATUS notifyStatus = InvertedEventNotify(packet, packetSize);
        ExFreePoolWithTag(packet, 'tkPE');

        if (!NT_SUCCESS(notifyStatus)) {
            // Revert queuing if notification fails
            KeAcquireSpinLock(&ext->EventListLock, &oldIrql);
            if (IoSetCancelRoutine(Irp, NULL) != NULL) {
                RemoveEntryList(&irpEvent->ListEntry);
                KeReleaseSpinLock(&ext->EventListLock, oldIrql);
                ExFreePoolWithTag(irpEvent, 'kEvt');

                // Since we marked it pending, we must pass it down asynchronously or complete it.
                // We will pass it down and allow normal execution
                IoSkipCurrentIrpStackLocation(Irp);
                IoCallDriver(ext->LowerDevice, Irp);
                return STATUS_PENDING;
            }
            KeReleaseSpinLock(&ext->EventListLock, oldIrql);
        }

        // Return STATUS_PENDING because we halted the IRP stack progression
        return STATUS_PENDING;
    }

    default:
        // For all other PnP IRPs (Start, Stop, Surprise Removal, etc.),
        // simply pass them down the stack untouched to maintain normal device operation.
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(ext->LowerDevice, Irp);
    }
}

NTSTATUS
ChildFilter_DispatchInternalDeviceControl(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
)
{
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "ChildFilter_DispatchInternalDeviceControl: Entry\n"));

#if DBG
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[Debug] InternalDevCtrl IOCTL: 0x%X on IRP: %p\n",
        stack->Parameters.DeviceIoControl.IoControlCode, Irp));
#endif

    PCHILD_FILTER_EXTENSION ext = (PCHILD_FILTER_EXTENSION)DeviceObject->DeviceExtension;

    // If this is the KMDF Hub device, route it back to the saved WDF dispatch routine
    if (ext == NULL || ext->MagicNumber != WDM_FILTER_MAGIC || ext->IsWdmFilter != TRUE) {
        return WdfDefaultDispatch[IRP_MJ_INTERNAL_DEVICE_CONTROL](DeviceObject, Irp);
    }

    // Instantly kill traffic if the device is slated for removal
    if (ext->IsBanned) {
        Irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG ioControlCode = irpSp->Parameters.DeviceIoControl.IoControlCode;

#if DBG
    // View when device issues a reset command
    if (ioControlCode == IOCTL_INTERNAL_USB_RESET_PORT) {
        // debug only
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "ChildFilter_DispatchInternalDeviceControl: USB Port Reset command intercepted on child PDO: %p\n", ext->Pdo));
    }
#endif

    // Intercept URBs and define custom URB completion routine
    if (ioControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB) {
        PURB urb = (PURB)irpSp->Parameters.Others.Argument1;

        if (urb != NULL) {
            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(Irp, ChildFilter_UrbCompletionRoutine, urb, TRUE, TRUE, TRUE);

            // Tell the I/O Manager and upper drivers that this IRP will be completed asynchronously
            IoMarkIrpPending(Irp);

            IoCallDriver(ext->LowerDevice, Irp);

            return STATUS_PENDING;
        }
    }

    // Default pass-down for all other Internal Device Control requests
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->LowerDevice, Irp);
}

PVOID
Helper_GetUsableBuffer(
    _In_ PMDL Mdl,
    _In_ PVOID TransferBuffer
)
{
    if (Mdl != NULL) {
        return MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority | MdlMappingNoExecute);
    }

    if (TransferBuffer != NULL) {
        // If it is a kernel-space address, it is safe to read.
        // If it is a user-space address, we must reject it to prevent a 0x50 BSOD.
        if ((ULONG_PTR)TransferBuffer > (ULONG_PTR)MmHighestUserAddress) {
            return TransferBuffer;
        }
    }

    return NULL;
}

_Success_(return != FALSE) // Returns TRUE if the URB contains a standard data buffer, FALSE otherwise.
BOOLEAN
Helper_ExtractUrbDataParameters(
    _In_ PURB Urb,
    _Out_ PVOID* TransferBuffer,
    _Out_ PMDL* TransferBufferMDL,
    _Out_ PULONG TransferBufferLength,
    _Out_ USBD_PIPE_HANDLE* EndpointHandle
)
{
    // Initialize outputs to safe defaults
    *TransferBuffer = NULL;
    *TransferBufferMDL = NULL;
    *TransferBufferLength = 0;

    if (Urb == NULL) return FALSE;

    switch (Urb->UrbHeader.Function) {

        // Group 1: Bulk and Interrupt Transfers
    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        *TransferBuffer = Urb->UrbBulkOrInterruptTransfer.TransferBuffer;
        *TransferBufferMDL = Urb->UrbBulkOrInterruptTransfer.TransferBufferMDL;
        *TransferBufferLength = Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
        *EndpointHandle = Urb->UrbBulkOrInterruptTransfer.PipeHandle;
        return TRUE;

        // Group 2: Control Transfers (Setup Packet + Data)
    case URB_FUNCTION_CONTROL_TRANSFER:
    case URB_FUNCTION_CONTROL_TRANSFER_EX:
        *TransferBuffer = Urb->UrbControlTransfer.TransferBuffer;
        *TransferBufferMDL = Urb->UrbControlTransfer.TransferBufferMDL;
        *TransferBufferLength = Urb->UrbControlTransfer.TransferBufferLength;
        *EndpointHandle = Urb->UrbControlTransfer.PipeHandle;
        return TRUE;

        // Group 3: Descriptor Requests
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE:
        *TransferBuffer = Urb->UrbControlDescriptorRequest.TransferBuffer;
        *TransferBufferMDL = Urb->UrbControlDescriptorRequest.TransferBufferMDL;
        *TransferBufferLength = Urb->UrbControlDescriptorRequest.TransferBufferLength;
        *EndpointHandle = NULL;
        return TRUE;

        // Group 4: Feature Requests
    case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
    case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
    case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
    case URB_FUNCTION_GET_CONFIGURATION:
    case URB_FUNCTION_GET_INTERFACE:
        *TransferBuffer = Urb->UrbControlGetStatusRequest.TransferBuffer;
        *TransferBufferMDL = Urb->UrbControlGetStatusRequest.TransferBufferMDL;
        *TransferBufferLength = Urb->UrbControlGetStatusRequest.TransferBufferLength;
        *EndpointHandle = NULL;
        return TRUE;

        // Unhandled or non-data URBs (e.g., SELECT_CONFIGURATION, ISOCH)
    default:
        return FALSE;
    }
}

VOID
Helper_SavePipeMapping(
    _In_ PCHILD_FILTER_EXTENSION Ext,
    _In_ USBD_PIPE_HANDLE PipeHandle,
    _In_ UCHAR EndpointAddress,
    _In_ UCHAR InterfaceNumber
)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&Ext->PipeMappingLock, &oldIrql);

    // Check if this handle already exists (e.g., device was reconfigured)
    for (ULONG i = 0; i < MAX_SUPPORTED_PIPES; i++) {
        if (Ext->PipeMappings[i].IsValid && Ext->PipeMappings[i].PipeHandle == PipeHandle) {
            Ext->PipeMappings[i].EndpointAddress = EndpointAddress;
            Ext->PipeMappings[i].InterfaceNumber = InterfaceNumber;
            KeReleaseSpinLock(&Ext->PipeMappingLock, oldIrql);
            return;
        }
    }

    // If it does not exist, find the first empty slot and save it
    for (ULONG i = 0; i < MAX_SUPPORTED_PIPES; i++) {
        if (!Ext->PipeMappings[i].IsValid) {
            Ext->PipeMappings[i].PipeHandle = PipeHandle;
            Ext->PipeMappings[i].EndpointAddress = EndpointAddress;
            Ext->PipeMappings[i].InterfaceNumber = InterfaceNumber;
            Ext->PipeMappings[i].IsValid = TRUE;
            break;
        }
    }

    KeReleaseSpinLock(&Ext->PipeMappingLock, oldIrql);
}

BOOLEAN
Helper_GetEndpointFromPipeHandle(
    _In_ PCHILD_FILTER_EXTENSION Ext,
    _In_ USBD_PIPE_HANDLE PipeHandle,
    _Out_ PUCHAR EndpointAddress
)
{
    KIRQL oldIrql;
    BOOLEAN found = FALSE;

    // Initialize output to a safe default (0 is usually the control endpoint)
    *EndpointAddress = 0;

    if (PipeHandle == NULL) {
        return FALSE;
    }

    KeAcquireSpinLock(&Ext->PipeMappingLock, &oldIrql);

    // Loop over existing pipe handles to find endpoint address
    for (ULONG i = 0; i < MAX_SUPPORTED_PIPES; i++) {
        if (Ext->PipeMappings[i].IsValid && Ext->PipeMappings[i].PipeHandle == PipeHandle) {
            *EndpointAddress = Ext->PipeMappings[i].EndpointAddress;
            found = TRUE;
            break;
        }
    }

    KeReleaseSpinLock(&Ext->PipeMappingLock, oldIrql);
    return found;
}

VOID
ChildFilter_CancelHeldIrp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
)
{
    PCHILD_FILTER_EXTENSION ext = (PCHILD_FILTER_EXTENSION)DeviceObject->DeviceExtension;

    // Mandatory: Release the system-wide cancel spin lock immediately.
    // The Irp->CancelIrql contains the IRQL from before the system lock was acquired.
    IoReleaseCancelSpinLock(Irp->CancelIrql);

    // -- find entry inside extention list and remove it 
    KIRQL oldIrql;
    KeAcquireSpinLock(&ext->EventListLock, &oldIrql);

    PLIST_ENTRY currentEntry = ext->PendingEventListHead.Flink;
    PPENDING_IRP_EVENT currentEvent;

    // check if eventID is contained in current filter device
    // LIST_ENTRY is a circular linked list
    // -> if we arrive at the start then the whole list was traversed
    while (currentEntry != &ext->PendingEventListHead) {
        currentEvent = CONTAINING_RECORD(
            currentEntry, // Pointer to the LIST_ENTRY
            PENDING_IRP_EVENT, // Type of the parent struct
            ListEntry // Name of the LIST_ENTRY field)
        );

        if (currentEvent->Irp == Irp) {
            RemoveEntryList(currentEntry);
            ExFreePoolWithTag(currentEvent, 'kEvt');
            break;
        }
        currentEntry = currentEntry->Flink;
    }
    KeReleaseSpinLock(&ext->EventListLock, oldIrql);

    // Complete the IRP with the cancelled status
    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;

    // Resume the unwinding process that we halted earlier
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

_Use_decl_annotations_
NTSTATUS
ChildFilter_UrbCompletionRoutine(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context
)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PURB urb = (PURB)Context;

    // Framework Constraint: If the Irp was already marked as canceled, then forward it that way
    if (Irp->PendingReturned) {
        IoMarkIrpPending(Irp);
    }

#if DBG
    if (urb != NULL) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "ChildFilter_UrbCompletionRoutine: Current URB Function %04X\n", urb->UrbHeader.Function));
    }
#endif

    if (NT_SUCCESS(Irp->IoStatus.Status) && urb != NULL) {
        PCHILD_FILTER_EXTENSION ext = (PCHILD_FILTER_EXTENSION)DeviceObject->DeviceExtension;
        PVOID rawBuffer = NULL;
        PMDL rawMdl = NULL;
        ULONG dataLength = 0;
        USBD_PIPE_HANDLE endpointHandle = NULL;

        //
        // Extract Data from URB and pass to User Mode
        // 

        if (ext->IsBanned) {
            if (Helper_ExtractUrbDataParameters(urb, &rawBuffer, &rawMdl, &dataLength, &endpointHandle)) {
                PVOID safeBuffer = Helper_GetUsableBuffer(rawMdl, rawBuffer);
                if (safeBuffer != NULL && dataLength > 0) {
                    // Zeroing the buffer forces a "KEY UP" state to avoid stuck keys
                    RtlZeroMemory(safeBuffer, dataLength);
                }
            }
            // Return early so we don't send useless events to user-mode
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[Debug] URB Completion Routine, device is banned: %p\n", Irp));
            return STATUS_SUCCESS;
        }
        
        // Attempt to extract the buffer parameters generically
        if (Helper_ExtractUrbDataParameters(urb, &rawBuffer, &rawMdl, &dataLength, &endpointHandle)) {
            // Resolve the safe kernel memory address
            // Mdl vs rawBuffer require different handling
            PVOID safeBuffer = Helper_GetUsableBuffer(rawMdl, rawBuffer);

            if (safeBuffer != NULL && dataLength > 0) {

                // Cap the data length to prevent overflows and massive allocations
                ULONG copyLength = dataLength;
                if (copyLength > MAX_URB_PAYLOAD_SIZE) {
                    copyLength = MAX_URB_PAYLOAD_SIZE;
                }

                // Dynamically allocate and fill generic struct
                size_t packetSize = FIELD_OFFSET(BUSFILTER_EVENT_PACKET, Data.Urb.Payload) + copyLength;
                PBUSFILTER_EVENT_PACKET packet = (PBUSFILTER_EVENT_PACKET)ExAllocatePool2(POOL_FLAG_NON_PAGED, packetSize, 'tkPE');

                if (packet != NULL) {
                    packet->EventType = FilterEventUrb;
                    packet->DeviceId = (ULONGLONG)ext->Pdo;
                    KeQuerySystemTime(&packet->Timestamp);

                    packet->Data.Urb.UrbFunction = urb->UrbHeader.Function;
                    packet->Data.Urb.DataLength = copyLength;

                    UINT64 eventId = (UINT64)InterlockedIncrement64(&ext->NextEventId);
                    packet->Data.Urb.EventId = eventId;

                    // Extract the PipeHandle from the URB
                    UCHAR resolvedEndpoint;
                    if (Helper_GetEndpointFromPipeHandle(ext, endpointHandle, &resolvedEndpoint)) {
                        // Successfully found the endpoint
                        packet->Data.Urb.EndpointAddress = resolvedEndpoint;
                    }
                    else {
                        // Fallback if mapping failed or wasn't captured
                        packet->Data.Urb.EndpointAddress = 0xFF;
                    }

                    RtlCopyMemory(packet->Data.Urb.Payload, safeBuffer, copyLength);

                    // Pre-allocate the tracking structure before sending to user mode to prevent race condition
                    PPENDING_IRP_EVENT irpEvent = (PPENDING_IRP_EVENT)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(PENDING_IRP_EVENT), 'kEvt');

                    if (irpEvent == NULL) {
                        ExFreePoolWithTag(packet, 'tkPE');
                        return STATUS_SUCCESS; // Fail-safe: let the IRP through if out of memory
                    }

                    irpEvent->EventId = eventId;
                    irpEvent->Irp = Irp;
                    irpEvent->rawBuffer = safeBuffer;
                    irpEvent->urbDataLength = dataLength;

                    // Lock and queue the IRP securely BEFORE notifying user mode
                    KIRQL oldIrql;
                    KeAcquireSpinLock(&ext->EventListLock, &oldIrql);

                    // If the user mode app is disconnected then return early and let request through
                    PDRIVER_CONTEXT driverCtx = GetDriverContext(WdfGetDriver());
                    if (driverCtx->IsAppConnected == FALSE) {
                        // App disconnected, abort queuing
                        KeReleaseSpinLock(&ext->EventListLock, oldIrql);
                        ExFreePoolWithTag(irpEvent, 'kEvt');
                        ExFreePoolWithTag(packet, 'tkPE');
                        return STATUS_SUCCESS;
                    }

                    // Set cancel routine in case the user mode app fails to respond in time (usually 500ms timeout)
                    // or if the framework otherwise determines the IRP needs to be canceled
                    IoSetCancelRoutine(Irp, ChildFilter_CancelHeldIrp);

                    if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL) != NULL) {
                        // IRP was already cancelled, abort queuing
                        KeReleaseSpinLock(&ext->EventListLock, oldIrql);
                        ExFreePoolWithTag(irpEvent, 'kEvt');
                        ExFreePoolWithTag(packet, 'tkPE');
                        return STATUS_SUCCESS;
                    }

                    // Safely insert into pending event list
                    InsertTailList(&ext->PendingEventListHead, &irpEvent->ListEntry);
                    KeReleaseSpinLock(&ext->EventListLock, oldIrql);

                    // notify user mode
                    NTSTATUS userModeNotified = InvertedEventNotify(packet, packetSize);
                    ExFreePoolWithTag(packet, 'tkPE');

                    if (NT_SUCCESS(userModeNotified)) {
                        // If the notification was succuessful, then we can safely halt the IRP
                        return STATUS_MORE_PROCESSING_REQUIRED;
                    }
                    else {
                        // Notification failed (e.g., IOCTL queue empty) -> undo the queuing
                        KeAcquireSpinLock(&ext->EventListLock, &oldIrql);

                        if (IoSetCancelRoutine(Irp, NULL) != NULL) {
                            RemoveEntryList(&irpEvent->ListEntry);
                            KeReleaseSpinLock(&ext->EventListLock, oldIrql);
                            ExFreePoolWithTag(irpEvent, 'kEvt');
                            return STATUS_SUCCESS; // Let OS complete it
                        }
                        else {
                            // Cancel routine is already running and has ownership, let it handle completion
                            KeReleaseSpinLock(&ext->EventListLock, oldIrql);
                            return STATUS_MORE_PROCESSING_REQUIRED;
                        }
                    }
                }
            }
        }
        else { // Specific URB Function wasn't handled generically in ExtractUrbDataParameters
            if (urb->UrbHeader.Function == URB_FUNCTION_SELECT_CONFIGURATION) {
                // Special Handling for select configuration
                struct _URB_SELECT_CONFIGURATION* configReq = &urb->UrbSelectConfiguration;

                // First pass: Count total pipes across all interfaces to calculate allocation size
                ULONG totalPipes = 0;
                PUSBD_INTERFACE_INFORMATION interfaceInfo = &configReq->Interface;

                // If the ConfigurationDescriptor is NULL, the device is being unconfigured.
                if (configReq->ConfigurationDescriptor != NULL) {
                    // Iterate through all packed interfaces. 
                    // WDM packs them tightly, so we advance the pointer by the 'Length' field.
                    PUCHAR currentPos = (PUCHAR)interfaceInfo;

                    // Safety check to ensure we don't read past the URB size
                    while (currentPos < ((PUCHAR)urb + urb->UrbHeader.Length)) {
                        interfaceInfo = (PUSBD_INTERFACE_INFORMATION)currentPos;
                        totalPipes += interfaceInfo->NumberOfPipes;

                        // Advance to the next interface block
                        currentPos += interfaceInfo->Length;
                    }
                }

                if (totalPipes > 0) {
                    // Calculate dynamic size based on the pipe count
                    size_t configDataSize = FIELD_OFFSET(BUSFILTER_EVENT_PACKET, Data.Config.Pipes) +
                        (totalPipes * sizeof(FILTER_PIPE_INFO));

                    PBUSFILTER_EVENT_PACKET packet = (PBUSFILTER_EVENT_PACKET)ExAllocatePool2(POOL_FLAG_NON_PAGED, configDataSize, 'tkPE');

                    if (packet != NULL) {
                        packet->EventType = FilterEventConfiguration;
                        packet->DeviceId = (ULONGLONG)ext->Pdo;
                        KeQuerySystemTime(&packet->Timestamp);
                        packet->Data.Config.NumberOfPipesConfigured = totalPipes;
                        packet->Data.Config.PortNumber = ext->PortNumber;

                        // Second pass: Extract the data and fill the packet
                        ULONG pipeIndex = 0;
                        interfaceInfo = &configReq->Interface;
                        PUCHAR currentPos = (PUCHAR)interfaceInfo;

                        while (currentPos < ((PUCHAR)urb + urb->UrbHeader.Length)) {
                            interfaceInfo = (PUSBD_INTERFACE_INFORMATION)currentPos;

                            for (ULONG i = 0; i < interfaceInfo->NumberOfPipes; i++) {
                                PUSBD_PIPE_INFORMATION pipe = &interfaceInfo->Pipes[i];

                                // Fill the user-mode packet
                                packet->Data.Config.Pipes[pipeIndex].InterfaceNumber = interfaceInfo->InterfaceNumber;
                                packet->Data.Config.Pipes[pipeIndex].EndpointAddress = pipe->EndpointAddress;
                                packet->Data.Config.Pipes[pipeIndex].PipeType = (UCHAR)pipe->PipeType;
                                packet->Data.Config.Pipes[pipeIndex].MaximumPacketSize = pipe->MaximumPacketSize;

                                // Save the PipeHandle -> EndpointAddress mapping for reference for later URB events
                                Helper_SavePipeMapping(
                                    ext,
                                    pipe->PipeHandle,
                                    pipe->EndpointAddress,
                                    interfaceInfo->InterfaceNumber
                                );

                                pipeIndex++;
                            }
                            currentPos += interfaceInfo->Length;
                        }

                        // Send to user mode
                        InvertedEventNotify(packet, configDataSize); // dont check return as it doesnt matter for filtering
                        ExFreePoolWithTag(packet, 'tkPE');
                    }
                }
            }
        }

    }
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[Debug] URB Completion Routine at the bottom, Queued IRP: %p\n", Irp));

    return STATUS_SUCCESS;
}

// ============================================================================
// IOCTL Implementation
// ============================================================================

NTSTATUS
InvertedEventNotify(
    _In_ PBUSFILTER_EVENT_PACKET EventPacket,
    _In_ size_t PacketSize
)
{
    NTSTATUS status;
    WDFREQUEST pendingRequest;
    PCONTROL_DEVICE_CONTEXT controlCtx;
    PVOID outputBuffer;

    if (g_ControlDevice == NULL || EventPacket == NULL) {
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }

    controlCtx = GetControlContext(g_ControlDevice);

    status = WdfIoQueueRetrieveNextRequest(controlCtx->NotificationQueue, &pendingRequest);

    if (NT_SUCCESS(status)) {

        // Pass the dynamic PacketSize to ensure the user-mode buffer is large enough
        status = WdfRequestRetrieveOutputBuffer(pendingRequest,
            PacketSize,
            &outputBuffer,
            NULL);

        if (NT_SUCCESS(status)) {
            // copy event packet into output buffer
            RtlCopyMemory(outputBuffer, EventPacket, PacketSize);
            WdfRequestCompleteWithInformation(pendingRequest, STATUS_SUCCESS, PacketSize);
        }
        else {
            // retrieving the output buffer failed (e.g. Buffer too small) 
            // then return with the error status
            WdfRequestComplete(pendingRequest, status);
        }
    }
#if DBG
    else if (status == STATUS_NO_MORE_ENTRIES) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "Event dropped: No pending IOCTLs in queue.\n"));
    }
#endif
    return status;
}

VOID
InvertedIoDeviceControl(WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode
)
{
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "InvertedIoDeviceControl: Entry\n"));

    PCONTROL_DEVICE_CONTEXT controlCtx;
    NTSTATUS status;

    controlCtx = GetControlContext(WdfIoQueueGetDevice(Queue));

    switch (IoControlCode) {

        // This IOCTLs are sent by the user application, and will be completed
        // by the driver when an event occurs.
    case IOCTL_CUSTOM_GET_EVENT: {

        // Validate the output buffer can hold the shared structure header
        if (OutputBufferLength < FIELD_OFFSET(BUSFILTER_EVENT_PACKET, Data)) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            break;
        }

        status = WdfRequestForwardToIoQueue(Request,
            controlCtx->NotificationQueue);

        // If we can't forward the Request to our holding queue,
        // we have to complete it.  We'll use whatever status we get
        // back from WdfRequestForwardToIoQueue.
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
        }
        break;
    }

    case IOCTL_CUSTOM_DEVICE_START_VERDICT:
    {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[Debug] Got verdict request for start device\n"));

        size_t verdictLength = sizeof(USERMODE_VERDICT);

        if (InputBufferLength != verdictLength) {
            WdfRequestComplete(Request, STATUS_INFO_LENGTH_MISMATCH);
            break;
        }

        PVOID inputBuffer;
        status = WdfRequestRetrieveInputBuffer(Request, verdictLength, &inputBuffer, NULL);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[Debug] start verdict: couldnt get buffer\n"));
            WdfRequestComplete(Request, status);
            return;
        }

        PUSERMODE_VERDICT verdictStruct = (PUSERMODE_VERDICT)inputBuffer;
        PDRIVER_CONTEXT driverCtx = GetDriverContext(WdfGetDriver());

        PIRP irpToComplete = NULL;
        PCHILD_FILTER_EXTENSION targetExt = NULL;
        NTSTATUS returnStatus = STATUS_NOT_FOUND;

        // Locate the specific IRP in the global collection
        WdfSpinLockAcquire(driverCtx->FilterDeviceLock);
        PLIST_ENTRY currentGlobalEntry = driverCtx->FilterDeviceListHead.Flink;
        BOOLEAN found = FALSE;

        while (currentGlobalEntry != &driverCtx->FilterDeviceListHead) {
            PCHILD_FILTER_EXTENSION currentFilterExt = CONTAINING_RECORD(currentGlobalEntry, CHILD_FILTER_EXTENSION, GlobalListEntry);

            KIRQL oldIrql;
            KeAcquireSpinLock(&currentFilterExt->EventListLock, &oldIrql);

            PLIST_ENTRY currentEntry = currentFilterExt->PendingEventListHead.Flink;
            while (currentEntry != &currentFilterExt->PendingEventListHead) {
                PPENDING_IRP_EVENT currentIrpEvent = CONTAINING_RECORD(currentEntry, PENDING_IRP_EVENT, ListEntry);

                if (currentIrpEvent->EventId == verdictStruct->EventId) {
                    found = TRUE;
                    if (IoSetCancelRoutine(currentIrpEvent->Irp, NULL) != NULL) {
                        irpToComplete = currentIrpEvent->Irp;
                        targetExt = currentFilterExt;
                        returnStatus = STATUS_SUCCESS;

                        RemoveEntryList(currentEntry);
                        ExFreePoolWithTag(currentIrpEvent, 'kEvt');
                    }
                    else {
                        returnStatus = STATUS_CANCELLED;
                    }
                    break;
                }
                currentEntry = currentEntry->Flink;
            }
            KeReleaseSpinLock(&currentFilterExt->EventListLock, oldIrql);
            if (found) break;
            currentGlobalEntry = currentGlobalEntry->Flink;
        }
        WdfSpinLockRelease(driverCtx->FilterDeviceLock);

        // Resume the IRP at PASSIVE_LEVEL based on the user-mode verdict
        if (irpToComplete != NULL && targetExt != NULL) {
            if (verdictStruct->Allow) {
                KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[Debug] User verdict allow for start device, calling IoCallDriver: %p\n", irpToComplete));
                // Allow: Pass the start request down to the USB hub driver
                IoSkipCurrentIrpStackLocation(irpToComplete);
                IoCallDriver(targetExt->LowerDevice, irpToComplete);
            }
            else {
                // Deny: Fail the IRP immediately. The OS will abort device start.
                irpToComplete->IoStatus.Status = STATUS_ACCESS_DENIED;
                irpToComplete->IoStatus.Information = 0;
                KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[Debug] User verdict received. Calling IoCompleteRequest on IRP: %p\n", irpToComplete));
                IoCompleteRequest(irpToComplete, IO_NO_INCREMENT);
            }
        }
        else {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[Debug] start verdict: couldnt find IRP\n"));
        }

        WdfRequestComplete(Request, returnStatus);
        break;
    }

    case IOCTL_CUSTOM_REMOVE_DEVICE:
    {
        size_t requestLength = sizeof(USERMODE_REMOVE_REQUEST);

        if (InputBufferLength != requestLength) {
            WdfRequestComplete(Request, STATUS_INFO_LENGTH_MISMATCH);
            break;
        }

        PVOID inputBuffer;
        status = WdfRequestRetrieveInputBuffer(Request, requestLength, &inputBuffer, NULL);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }

        PUSERMODE_REMOVE_REQUEST removeReq = (PUSERMODE_REMOVE_REQUEST)inputBuffer;
        PDRIVER_CONTEXT driverCtx = GetDriverContext(WdfGetDriver());
        BOOLEAN deviceFound = FALSE;

        // Locate the target device in the global list
        WdfSpinLockAcquire(driverCtx->FilterDeviceLock);
        PLIST_ENTRY currentEntry = driverCtx->FilterDeviceListHead.Flink;
        PDEVICE_OBJECT foundPdo = NULL;

        while (currentEntry != &driverCtx->FilterDeviceListHead) {
            PCHILD_FILTER_EXTENSION currentFilterExt = CONTAINING_RECORD(currentEntry, CHILD_FILTER_EXTENSION, GlobalListEntry);

            if ((ULONGLONG)currentFilterExt->Pdo == removeReq->DeviceId) {
                // 2. Set the internal banned flag
                currentFilterExt->IsBanned = TRUE;
                deviceFound = TRUE;
                foundPdo = currentFilterExt->Pdo;
                break;
            }
            currentEntry = currentEntry->Flink;
        }
        WdfSpinLockRelease(driverCtx->FilterDeviceLock);

        if (deviceFound == TRUE && foundPdo != NULL) {
            // 3. Command the PnP manager to query this specific device's state
            IoInvalidateDeviceState(foundPdo);
            WdfRequestComplete(Request, STATUS_SUCCESS);
        }
        else {
            WdfRequestComplete(Request, STATUS_NOT_FOUND);
        }
        break;
    }

    case IOCTL_CUSTOM_RELEASE_URB: {
        // Here, the verdict for a certain eventId from the user mode app is processed

        size_t verdictLength = sizeof(USERMODE_VERDICT);

        if (InputBufferLength != verdictLength) {
            WdfRequestComplete(Request, STATUS_INFO_LENGTH_MISMATCH);
            break;
        }

        PVOID inputBuffer;
        status = WdfRequestRetrieveInputBuffer(Request, verdictLength, &inputBuffer, NULL);

        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }

        PUSERMODE_VERDICT verdictStruct = (PUSERMODE_VERDICT)inputBuffer;
        PDRIVER_CONTEXT driverCtx = GetDriverContext(WdfGetDriver());

        // Local variables to hold data safely outside the spinlocks
        PIRP irpToComplete = NULL;
        PVOID safeBufferToClear = NULL;
        ULONG bufferLengthToClear = 0;
        NTSTATUS returnStatus = STATUS_NOT_FOUND;

        // --- Find the Pending IRP entry inside a filter context ---

        // Acquire the global device collection lock
        WdfSpinLockAcquire(driverCtx->FilterDeviceLock);

        PLIST_ENTRY currentGlobalEntry = driverCtx->FilterDeviceListHead.Flink;
        BOOLEAN found = FALSE;

        while (currentGlobalEntry != &driverCtx->FilterDeviceListHead) { // loop over filter devices
            PCHILD_FILTER_EXTENSION currentFilterExt = CONTAINING_RECORD(currentGlobalEntry, CHILD_FILTER_EXTENSION, GlobalListEntry);

            KIRQL oldIrql;
            // Acquire the specific device's event list lock
            KeAcquireSpinLock(&currentFilterExt->EventListLock, &oldIrql);

            PLIST_ENTRY currentEntry = currentFilterExt->PendingEventListHead.Flink;

            // Search the current event list
            while (currentEntry != &currentFilterExt->PendingEventListHead) {
                PPENDING_IRP_EVENT currentIrpEvent = CONTAINING_RECORD(currentEntry, PENDING_IRP_EVENT, ListEntry);

                if (currentIrpEvent->EventId == verdictStruct->EventId) {
                    found = TRUE;

                    // Try to claim ownership from the cancel routine
                    if (IoSetCancelRoutine(currentIrpEvent->Irp, NULL) != NULL) {
                        // Success: We own the IRP. Extract all needed data.
                        irpToComplete = currentIrpEvent->Irp;
                        safeBufferToClear = currentIrpEvent->rawBuffer;
                        bufferLengthToClear = currentIrpEvent->urbDataLength;
                        returnStatus = STATUS_SUCCESS;

                        // Remove from list and free the tracking struct immediately
                        RemoveEntryList(currentEntry);
                        ExFreePoolWithTag(currentIrpEvent, 'kEvt');
                    }
                    else {
                        // Failure: The cancel routine is currently executing.
                        returnStatus = STATUS_CANCELLED;
                    }
                    break; // Exit the while loop
                }
                currentEntry = currentEntry->Flink;
            }

            // Release the specific device's event list lock
            KeReleaseSpinLock(&currentFilterExt->EventListLock, oldIrql);

            if (found) {
                break; // Exit the for loop if we found the event (even if we couldn't claim it)
            }

            currentGlobalEntry = currentGlobalEntry->Flink;
        }

        // Release the global device collection lock before operating on any IRPs
        //   in order to fullfill framework constraints
        WdfSpinLockRelease(driverCtx->FilterDeviceLock);

        // Safely operate on the IRP at PASSIVE_LEVEL / un-elevated IRQL
        if (irpToComplete != NULL) {
            if (verdictStruct->Allow == FALSE) {
                // Deny the data: clear the buffer so the OS gets zeros
                if (safeBufferToClear != NULL && bufferLengthToClear > 0) {
                    RtlZeroMemory(safeBufferToClear, bufferLengthToClear);
                }
                irpToComplete->IoStatus.Status = STATUS_SUCCESS;
                irpToComplete->IoStatus.Information = bufferLengthToClear;
            }
            // If Allow == TRUE, we leave IoStatus untouched to pass the original data intact

            IoCompleteRequest(irpToComplete, IO_NO_INCREMENT);
        }

        // Complete the user-mode IOCTL Request itself
        WdfRequestComplete(Request, returnStatus);
        break;
    }

    default: {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        break;
    }
    } // end switch
}