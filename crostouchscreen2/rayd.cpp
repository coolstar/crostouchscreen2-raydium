#define DESCRIPTOR_DEF
#include "raydium_i2c.h"

static ULONG RaydDebugLevel = 100;
static ULONG RaydDebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

NTSTATUS
DriverEntry(
	__in PDRIVER_OBJECT  DriverObject,
	__in PUNICODE_STRING RegistryPath
)
{
	NTSTATUS               status = STATUS_SUCCESS;
	WDF_DRIVER_CONFIG      config;
	WDF_OBJECT_ATTRIBUTES  attributes;

	RaydPrint(DEBUG_LEVEL_INFO, DBG_INIT,
		"Driver Entry\n");

	WDF_DRIVER_CONFIG_INIT(&config, RaydEvtDeviceAdd);

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

	//
	// Create a framework driver object to represent our driver.
	//

	status = WdfDriverCreate(DriverObject,
		RegistryPath,
		&attributes,
		&config,
		WDF_NO_HANDLE
	);

	if (!NT_SUCCESS(status))
	{
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_INIT,
			"WdfDriverCreate failed with status 0x%x\n", status);
	}

	return status;
}

#include <pshpack1.h>
struct raydium_bank_switch_header {
	UINT8 cmd;
	UINT32 be_addr;
};
#include <poppack.h>

static NTSTATUS raydium_i2c_send(PRAYD_CONTEXT pDevice, UINT32 addr, const UINT8* data, UINT32 len) {
	NTSTATUS status;

	UINT8 regAddr = addr & 0xFF;
	UINT8 *txBuf = (UINT8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, len + 1, RAYD_POOL_TAG);
	if (!txBuf) {
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Failed to allocate txBuf\n");
		return STATUS_NO_MEMORY;
	}

	LONGLONG Timeout;
	Timeout = -10 * 1000;
	status = WdfWaitLockAcquire(pDevice->I2CContext.SpbLock, &Timeout);
	if (status == STATUS_TIMEOUT) {
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Timed out trying to acquire lock for write\n");
		return STATUS_IO_TIMEOUT;
	}

	txBuf[0] = regAddr;
	RtlCopyMemory(txBuf + 1, data, len);

	status = SpbLockController(&pDevice->I2CContext); //Perform as a single i2c transfer transaction
	if (!NT_SUCCESS(status))
	{
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Failed to lock controller with status 0x%x\n", status);
		goto exit;
	}

	int tries = 0;
	do {
		struct raydium_bank_switch_header header;
		header.cmd = RM_CMD_BANK_SWITCH,
		header.be_addr = RtlUlongByteSwap(addr);

		if (addr > 0xFF) { //need to send RM_CMD_BANK_SWITCH first
			status = SpbWriteDataSynchronously(&pDevice->I2CContext, &header, sizeof(header));
			if (!NT_SUCCESS(status)) {
				RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"Failed to send RM_CMD_BANK_SWITCH 0x%x\n", status);
				goto retry;
			}
		}

		status = SpbWriteDataSynchronously(&pDevice->I2CContext, txBuf, len + 1);
		if (!NT_SUCCESS(status)) {
			RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"Failed to send data 0x%x\n", status);
			goto retry;
		}
		break;
	retry:
		LARGE_INTEGER Interval;
		Interval.QuadPart = -10 * 1000 * RM_RETRY_DELAY_MS;
		KeDelayExecutionThread(KernelMode, FALSE, &Interval);
	} while (++tries < RM_MAX_RETRIES);

exit:
	SpbUnlockController(&pDevice->I2CContext);

	ExFreePoolWithTag(txBuf, RAYD_POOL_TAG);

	WdfWaitLockRelease(pDevice->I2CContext.SpbLock);

	return status;
}

static NTSTATUS raydium_i2c_readSubset(PRAYD_CONTEXT pDevice, UINT32 addr, UINT8* data, UINT32 len) {
	NTSTATUS status;

	UINT8 regAddr = addr & 0xFF;

	LONGLONG Timeout;
	Timeout = -10 * 1000;
	status = WdfWaitLockAcquire(pDevice->I2CContext.SpbLock, &Timeout);
	if (status == STATUS_TIMEOUT) {
		return STATUS_IO_TIMEOUT;
	}

	status = SpbLockController(&pDevice->I2CContext); //Perform as a single i2c transfer transaction
	if (!NT_SUCCESS(status))
	{
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Failed to lock controller with status 0x%x\n", status);
		goto exit;
	}

	struct raydium_bank_switch_header header;
	header.cmd = RM_CMD_BANK_SWITCH,
	header.be_addr = RtlUlongByteSwap(addr);

	if (addr > 0xFF) { //need to send RM_CMD_BANK_SWITCH first
		status = SpbWriteDataSynchronously(&pDevice->I2CContext, &header, sizeof(header));
		if (!NT_SUCCESS(status)) {
			RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"Failed to send RM_CMD_BANK_SWITCH 0x%x\n", status);
			goto exit;
		}
	}

	status = SpbXferDataSynchronously(&pDevice->I2CContext, &regAddr, 1, (PVOID)data, len);
	if (!NT_SUCCESS(status)) {
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Failed to xfer data 0x%x\n", status);
		goto exit;
	}

exit:
	SpbUnlockController(&pDevice->I2CContext);

	WdfWaitLockRelease(pDevice->I2CContext.SpbLock);

	return status;
}

static NTSTATUS raydium_i2c_read(PRAYD_CONTEXT pDevice, UINT32 addr, UINT8* data, UINT32 len) {
	NTSTATUS status = STATUS_SUCCESS;

	while (len) {
		UINT32 xfer_len = min(len, RM_MAX_READ_SIZE);

		status = raydium_i2c_readSubset(pDevice, addr, data, xfer_len);
		if (!NT_SUCCESS(status)) {
			RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"subset read failed! (read %d of %d)\n", total_len - len, total_len);
			return status;
		}

		len -= xfer_len;
		data += xfer_len;
		addr += xfer_len;
	}
	return status;
}

static NTSTATUS raydium_i2c_sw_reset(_In_ PRAYD_CONTEXT pDevice)
{
	const UINT8 soft_rst_cmd = 0x01;
	NTSTATUS status;

	status = raydium_i2c_send(pDevice, RM_RESET_MSG_ADDR, &soft_rst_cmd,
		sizeof(soft_rst_cmd));
	if (!NT_SUCCESS(status)) {
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"software reset failed: %d\n", status);
		return status;
	}

	LARGE_INTEGER Interval;
	Interval.QuadPart = -10 * 1000 * RM_RESET_DELAY_MSEC;
	KeDelayExecutionThread(KernelMode, FALSE, &Interval);

	return 0;
}

static NTSTATUS raydium_i2c_query_ts_info(_In_ PRAYD_CONTEXT pDevice) {
	struct raydium_data_info data_info;
	UINT32 queryBankAddr;

	NTSTATUS status;
	int retry_cnt;

	for (retry_cnt = 0; retry_cnt < RM_MAX_RETRIES; retry_cnt++) {
		status = raydium_i2c_read(pDevice, RM_CMD_DATA_BANK, (UINT8 *)&data_info, sizeof(data_info));
		if (!NT_SUCCESS(status))
			continue;

		pDevice->packageSize = data_info.pkg_size;
		pDevice->reportSize = pDevice->packageSize - RM_PACKET_CRC_SIZE;
		pDevice->contactSize = data_info.tp_info_size;
		pDevice->dataBankAddr = data_info.data_bank_addr;

		status = raydium_i2c_read(pDevice, RM_CMD_QUERY_BANK, (UINT8*)&queryBankAddr, sizeof(queryBankAddr));
		if (!NT_SUCCESS(status))
			continue;

		status = raydium_i2c_read(pDevice, queryBankAddr, (UINT8*)&pDevice->info, sizeof(pDevice->info));
		if (!NT_SUCCESS(status))
			continue;

		DbgPrint("Raydium Touch Screen Initialized (%d x %d)\n", pDevice->info.x_max, pDevice->info.y_max);

		pDevice->max_x_hid[0] = pDevice->info.x_max & 0xFF;
		pDevice->max_x_hid[1] = pDevice->info.x_max >> 8;

		pDevice->max_y_hid[0] = pDevice->info.y_max & 0xFF;
		pDevice->max_y_hid[1] = pDevice->info.y_max >> 8;
		break;
	}
	return status;
}

static NTSTATUS raydium_i2c_check_fw_status(PRAYD_CONTEXT pDevice) {
	static const UINT8 bl_ack = 0x62;
	static const UINT8 main_ack = 0x66;
	UINT8 buf[4];
	NTSTATUS status;

	status = raydium_i2c_read(pDevice, RM_CMD_BOOT_READ, buf, sizeof(buf));
	if (NT_SUCCESS(status)) {
		if (buf[0] == bl_ack)
			pDevice->bootMode = RAYDIUM_TS_BLDR;
		else if (buf[0] == main_ack)
			pDevice->bootMode = RAYDIUM_TS_MAIN;
		return status;
	}
	return status;
}

NTSTATUS BOOTTOUCHSCREEN(
	_In_  PRAYD_CONTEXT  devContext
)
{
	NTSTATUS status;

	if (devContext->TouchScreenBooted)
		return STATUS_SUCCESS;
	
	int retryCount;
	for (retryCount = 0; retryCount < RM_MAX_RETRIES; retryCount++) {
		/* Wait for Hello packet */
		LARGE_INTEGER Interval;
		Interval.QuadPart = -10 * 1000 * RM_BOOT_DELAY_MS;
		KeDelayExecutionThread(KernelMode, FALSE, &Interval);

		status = raydium_i2c_check_fw_status(devContext);
		if (!NT_SUCCESS(status)) {
			RaydPrint(DEBUG_LEVEL_ERROR, DBG_INIT, "failed to read 'hello' packet: 0x%x\n", status);
			continue;
		}

		if (devContext->bootMode == RAYDIUM_TS_BLDR ||
			devContext->bootMode == RAYDIUM_TS_MAIN) {
			break;
		}
	}

	if (!NT_SUCCESS(status))
		devContext->bootMode = RAYDIUM_TS_BLDR;

	if (devContext->bootMode == RAYDIUM_TS_MAIN) {
		status = raydium_i2c_query_ts_info(devContext);
		if (!NT_SUCCESS(status)) {
			return status;
		}
		devContext->reportData = (UINT8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, devContext->packageSize, RAYD_POOL_TAG);
		if (!devContext->reportData)
			return STATUS_NO_MEMORY;

		devContext->TouchScreenBooted = true;
		return status;
	}
	else {
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_INIT, "Bootloader state not supported\n");
		return STATUS_INVALID_DEVICE_STATE;
	}
}

NTSTATUS
OnPrepareHardware(
	_In_  WDFDEVICE     FxDevice,
	_In_  WDFCMRESLIST  FxResourcesRaw,
	_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

This routine caches the SPB resource connection ID.

Arguments:

FxDevice - a handle to the framework device object
FxResourcesRaw - list of translated hardware resources that
the PnP manager has assigned to the device
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PRAYD_CONTEXT pDevice = GetDeviceContext(FxDevice);
	BOOLEAN fSpbResourceFound = FALSE;
	NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;

	UNREFERENCED_PARAMETER(FxResourcesRaw);

	//
	// Parse the peripheral's resources.
	//

	ULONG resourceCount = WdfCmResourceListGetCount(FxResourcesTranslated);

	for (ULONG i = 0; i < resourceCount; i++)
	{
		PCM_PARTIAL_RESOURCE_DESCRIPTOR pDescriptor;
		UCHAR Class;
		UCHAR Type;

		pDescriptor = WdfCmResourceListGetDescriptor(
			FxResourcesTranslated, i);

		switch (pDescriptor->Type)
		{
		case CmResourceTypeConnection:
			//
			// Look for I2C or SPI resource and save connection ID.
			//
			Class = pDescriptor->u.Connection.Class;
			Type = pDescriptor->u.Connection.Type;
			if (Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
				Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
			{
				if (fSpbResourceFound == FALSE)
				{
					status = STATUS_SUCCESS;
					pDevice->I2CContext.I2cResHubId.LowPart = pDescriptor->u.Connection.IdLowPart;
					pDevice->I2CContext.I2cResHubId.HighPart = pDescriptor->u.Connection.IdHighPart;
					fSpbResourceFound = TRUE;
				}
				else
				{
				}
			}
			break;
		default:
			//
			// Ignoring all other resource types.
			//
			break;
		}
	}

	//
	// An SPB resource is required.
	//

	if (fSpbResourceFound == FALSE)
	{
		status = STATUS_NOT_FOUND;
	}

	status = SpbTargetInitialize(FxDevice, &pDevice->I2CContext);

	if (!NT_SUCCESS(status))
	{
		return status;
	}

	return status;
}

NTSTATUS
OnReleaseHardware(
	_In_  WDFDEVICE     FxDevice,
	_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

Arguments:

FxDevice - a handle to the framework device object
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PRAYD_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	UNREFERENCED_PARAMETER(FxResourcesTranslated);

	if (pDevice->reportData) {
		ExFreePoolWithTag(pDevice->reportData, RAYD_POOL_TAG);
	}

	SpbTargetDeinitialize(FxDevice, &pDevice->I2CContext);

	return status;
}

NTSTATUS
OnD0Entry(
	_In_  WDFDEVICE               FxDevice,
	_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

Routine Description:

This routine allocates objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxPreviousState - previous power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PRAYD_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	status = raydium_i2c_sw_reset(pDevice);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	for (int i = 0; i < 20; i++) {
		pDevice->Flags[i] = 0;
	}

	pDevice->RegsSet = false;
	pDevice->ConnectInterrupt = true;

	status = BOOTTOUCHSCREEN(pDevice);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	return status;
}

NTSTATUS
OnD0Exit(
	_In_  WDFDEVICE               FxDevice,
	_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

Routine Description:

This routine destroys objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxPreviousState - previous power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PRAYD_CONTEXT pDevice = GetDeviceContext(FxDevice);

	pDevice->ConnectInterrupt = false;

	return STATUS_SUCCESS;
}

void RaydProcessInput(PRAYD_CONTEXT pDevice) {
	struct _RAYD_MULTITOUCH_REPORT report;
	report.ReportID = REPORTID_MTOUCH;

	int count = 0, i = 0;
	while (count < 10 && i < 20) {
		if (pDevice->Flags[i] != 0) {
			report.Touch[count].ContactID = (BYTE)i;
			report.Touch[count].Height = pDevice->AREA[i];
			report.Touch[count].Width = pDevice->AREA[i];

			report.Touch[count].XValue = pDevice->XValue[i];
			report.Touch[count].YValue = pDevice->YValue[i];

			uint8_t flags = pDevice->Flags[i];
			if (flags & MXT_T9_DETECT) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT | MULTI_TIPSWITCH_BIT;
			}
			else if (flags & MXT_T9_PRESS) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT | MULTI_TIPSWITCH_BIT;
			}
			else if (flags & MXT_T9_RELEASE) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT;
				pDevice->Flags[i] = 0;
			}
			else
				report.Touch[count].Status = 0;

			count++;
		}
		i++;
	}

	report.ActualCount = (BYTE)count;

	if (count > 0) {
		size_t bytesWritten;
		RaydProcessVendorReport(pDevice, &report, sizeof(report), &bytesWritten);
	}
}

static UINT16 raydium_calc_chksum(const UINT8* buf, UINT16 len)
{
	UINT16 checksum = 0;
	UINT16 i;

	for (i = 0; i < len; i++)
		checksum += buf[i];

	return checksum;
}

BOOLEAN OnInterruptIsr(
	WDFINTERRUPT Interrupt,
	ULONG MessageID) {
	UNREFERENCED_PARAMETER(MessageID);

	WDFDEVICE Device = WdfInterruptGetDevice(Interrupt);
	PRAYD_CONTEXT pDevice = GetDeviceContext(Device);

	UNREFERENCED_PARAMETER(pDevice);
	NTSTATUS status;

	if (!pDevice->ConnectInterrupt) {
		return false;
	}


	if (!pDevice->TouchScreenBooted) {
		return false;
	}

	status = raydium_i2c_read(pDevice, pDevice->dataBankAddr, pDevice->reportData, pDevice->packageSize);
	if (!NT_SUCCESS(status)) {
		return true;
	}

	UINT16 fw_crc = *((UINT16 *)&pDevice->reportData[pDevice->reportSize]);
	UINT16 calc_crc = raydium_calc_chksum(pDevice->reportData, pDevice->reportSize);
	if (fw_crc != calc_crc) {
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_INIT, "Invalid raydium crc %#04x vs %#04x\n", calc_crc, fw_crc);
		return true;
	}

	for (int i = 0; i < pDevice->reportSize / pDevice->contactSize; i++) {
		UINT8* contact = &pDevice->reportData[pDevice->contactSize * i];
		bool state = contact[RM_CONTACT_STATE_POS];
		UINT8 wx, wy;

		if (!state && pDevice->Flags[i] == MXT_T9_DETECT) {
			pDevice->Flags[i] = MXT_T9_RELEASE;
		} else if (state) {
			pDevice->Flags[i] = MXT_T9_DETECT;
		} else {
			pDevice->Flags[i] = 0;
		}

		if (!state)
			continue;

		wx = contact[RM_CONTACT_WIDTH_X_POS];
		wy = contact[RM_CONTACT_WIDTH_Y_POS];

		pDevice->XValue[i] = *((UINT16*)&contact[RM_CONTACT_X_POS]);
		pDevice->YValue[i] = *((UINT16*)&contact[RM_CONTACT_Y_POS]);

		pDevice->AREA[i] = max(wx, wy);
	}

	RaydProcessInput(pDevice);

	return true;
}

NTSTATUS
RaydEvtDeviceAdd(
	IN WDFDRIVER       Driver,
	IN PWDFDEVICE_INIT DeviceInit
)
{
	NTSTATUS                      status = STATUS_SUCCESS;
	WDF_IO_QUEUE_CONFIG           queueConfig;
	WDF_OBJECT_ATTRIBUTES         attributes;
	WDFDEVICE                     device;
	WDF_INTERRUPT_CONFIG interruptConfig;
	WDFQUEUE                      queue;
	PRAYD_CONTEXT               devContext;

	UNREFERENCED_PARAMETER(Driver);

	PAGED_CODE();

	RaydPrint(DEBUG_LEVEL_INFO, DBG_PNP,
		"RaydEvtDeviceAdd called\n");

	//
	// Tell framework this is a filter driver. Filter drivers by default are  
	// not power policy owners. This works well for this driver because
	// HIDclass driver is the power policy owner for HID minidrivers.
	//

	WdfFdoInitSetFilter(DeviceInit);

	{
		WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
		WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);

		pnpCallbacks.EvtDevicePrepareHardware = OnPrepareHardware;
		pnpCallbacks.EvtDeviceReleaseHardware = OnReleaseHardware;
		pnpCallbacks.EvtDeviceD0Entry = OnD0Entry;
		pnpCallbacks.EvtDeviceD0Exit = OnD0Exit;

		WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);
	}

	//
	// Setup the device context
	//

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, RAYD_CONTEXT);

	//
	// Create a framework device object.This call will in turn create
	// a WDM device object, attach to the lower stack, and set the
	// appropriate flags and attributes.
	//

	status = WdfDeviceCreate(&DeviceInit, &attributes, &device);

	if (!NT_SUCCESS(status))
	{
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfDeviceCreate failed with status code 0x%x\n", status);

		return status;
	}

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);

	queueConfig.EvtIoInternalDeviceControl = RaydEvtInternalDeviceControl;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&queue
	);

	if (!NT_SUCCESS(status))
	{
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	//
	// Create manual I/O queue to take care of hid report read requests
	//

	devContext = GetDeviceContext(device);

	devContext->TouchScreenBooted = false;

	devContext->FxDevice = device;

	WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

	queueConfig.PowerManaged = WdfFalse;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->ReportQueue
	);

	if (!NT_SUCCESS(status))
	{
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	//
	// Create an interrupt object for hardware notifications
	//
	WDF_INTERRUPT_CONFIG_INIT(
		&interruptConfig,
		OnInterruptIsr,
		NULL);
	interruptConfig.PassiveHandling = TRUE;

	status = WdfInterruptCreate(
		device,
		&interruptConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->Interrupt);

	if (!NT_SUCCESS(status))
	{
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"Error creating WDF interrupt object - %!STATUS!",
			status);

		return status;
	}

	//
	// Initialize DeviceMode
	//

	devContext->DeviceMode = DEVICE_MODE_MOUSE;

	return status;
}

VOID
RaydEvtInternalDeviceControl(
	IN WDFQUEUE     Queue,
	IN WDFREQUEST   Request,
	IN size_t       OutputBufferLength,
	IN size_t       InputBufferLength,
	IN ULONG        IoControlCode
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	WDFDEVICE           device;
	PRAYD_CONTEXT     devContext;
	BOOLEAN             completeRequest = TRUE;

	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	device = WdfIoQueueGetDevice(Queue);
	devContext = GetDeviceContext(device);

	RaydPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
		"%s, Queue:0x%p, Request:0x%p\n",
		DbgHidInternalIoctlString(IoControlCode),
		Queue,
		Request
	);

	//
	// Please note that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl. So depending on the ioctl code, we will either
	// use retreive function or escape to WDM to get the UserBuffer.
	//

	switch (IoControlCode)
	{

	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
		//
		// Retrieves the device's HID descriptor.
		//
		status = RaydGetHidDescriptor(device, Request);
		break;

	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
		//
		//Retrieves a device's attributes in a HID_DEVICE_ATTRIBUTES structure.
		//
		status = RaydGetDeviceAttributes(Request);
		break;

	case IOCTL_HID_GET_REPORT_DESCRIPTOR:
		//
		//Obtains the report descriptor for the HID device.
		//
		status = RaydGetReportDescriptor(device, Request);
		break;

	case IOCTL_HID_GET_STRING:
		//
		// Requests that the HID minidriver retrieve a human-readable string
		// for either the manufacturer ID, the product ID, or the serial number
		// from the string descriptor of the device. The minidriver must send
		// a Get String Descriptor request to the device, in order to retrieve
		// the string descriptor, then it must extract the string at the
		// appropriate index from the string descriptor and return it in the
		// output buffer indicated by the IRP. Before sending the Get String
		// Descriptor request, the minidriver must retrieve the appropriate
		// index for the manufacturer ID, the product ID or the serial number
		// from the device extension of a top level collection associated with
		// the device.
		//
		status = RaydGetString(Request);
		break;

	case IOCTL_HID_WRITE_REPORT:
	case IOCTL_HID_SET_OUTPUT_REPORT:
		//
		//Transmits a class driver-supplied report to the device.
		//
		status = RaydWriteReport(devContext, Request);
		break;

	case IOCTL_HID_READ_REPORT:
	case IOCTL_HID_GET_INPUT_REPORT:
		//
		// Returns a report from the device into a class driver-supplied buffer.
		// 
		status = RaydReadReport(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_SET_FEATURE:
		//
		// This sends a HID class feature report to a top-level collection of
		// a HID class device.
		//
		status = RaydSetFeature(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_GET_FEATURE:
		//
		// returns a feature report associated with a top-level collection
		//
		status = RaydGetFeature(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_ACTIVATE_DEVICE:
		//
		// Makes the device ready for I/O operations.
		//
	case IOCTL_HID_DEACTIVATE_DEVICE:
		//
		// Causes the device to cease operations and terminate all outstanding
		// I/O requests.
		//
	default:
		status = STATUS_NOT_SUPPORTED;
		break;
	}

	if (completeRequest)
	{
		WdfRequestComplete(Request, status);

		RaydPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"%s completed, Queue:0x%p, Request:0x%p\n",
			DbgHidInternalIoctlString(IoControlCode),
			Queue,
			Request
		);
	}
	else
	{
		RaydPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"%s deferred, Queue:0x%p, Request:0x%p\n",
			DbgHidInternalIoctlString(IoControlCode),
			Queue,
			Request
		);
	}

	return;
}

NTSTATUS
RaydGetHidDescriptor(
	IN WDFDEVICE Device,
	IN WDFREQUEST Request
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	size_t              bytesToCopy = 0;
	WDFMEMORY           memory;

	UNREFERENCED_PARAMETER(Device);

	RaydPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"RaydGetHidDescriptor Entry\n");

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputMemory(Request, &memory);

	if (!NT_SUCCESS(status))
	{
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputMemory failed 0x%x\n", status);

		return status;
	}

	//
	// Use hardcoded "HID Descriptor" 
	//
	bytesToCopy = DefaultHidDescriptor.bLength;

	if (bytesToCopy == 0)
	{
		status = STATUS_INVALID_DEVICE_STATE;

		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"DefaultHidDescriptor is zero, 0x%x\n", status);

		return status;
	}

	status = WdfMemoryCopyFromBuffer(memory,
		0, // Offset
		(PVOID)&DefaultHidDescriptor,
		bytesToCopy);

	if (!NT_SUCCESS(status))
	{
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfMemoryCopyFromBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, bytesToCopy);

	RaydPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"RaydGetHidDescriptor Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
RaydGetReportDescriptor(
	IN WDFDEVICE Device,
	IN WDFREQUEST Request
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	ULONG_PTR           bytesToCopy;
	WDFMEMORY           memory;

	PRAYD_CONTEXT devContext = GetDeviceContext(Device);

	UNREFERENCED_PARAMETER(Device);

	RaydPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"RaydGetReportDescriptor Entry\n");

#define MT_TOUCH_COLLECTION												\
			MT_TOUCH_COLLECTION0 \
			0x26, devContext->max_x_hid[0], devContext->max_x_hid[1],                   /*       LOGICAL_MAXIMUM (WIDTH)    */ \
			MT_TOUCH_COLLECTION1 \
			0x26, devContext->max_y_hid[0], devContext->max_y_hid[1],                   /*       LOGICAL_MAXIMUM (HEIGHT)    */ \
			MT_TOUCH_COLLECTION2 \

	HID_REPORT_DESCRIPTOR ReportDescriptor[] = {
		//
		// Multitouch report starts here
		//
		0x05, 0x0d,                         // USAGE_PAGE (Digitizers)
		0x09, 0x04,                         // USAGE (Touch Screen)
		0xa1, 0x01,                         // COLLECTION (Application)
		0x85, REPORTID_MTOUCH,              //   REPORT_ID (Touch)
		0x09, 0x22,                         //   USAGE (Finger)
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		USAGE_PAGE
		0xc0,                               // END_COLLECTION
	};

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputMemory(Request, &memory);
	if (!NT_SUCCESS(status))
	{
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputMemory failed 0x%x\n", status);

		return status;
	}

	//
	// Use hardcoded Report descriptor
	//
	bytesToCopy = DefaultHidDescriptor.DescriptorList[0].wReportLength;

	if (bytesToCopy == 0)
	{
		status = STATUS_INVALID_DEVICE_STATE;

		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"DefaultHidDescriptor's reportLength is zero, 0x%x\n", status);

		return status;
	}

	status = WdfMemoryCopyFromBuffer(memory,
		0,
		(PVOID)ReportDescriptor,
		bytesToCopy);
	if (!NT_SUCCESS(status))
	{
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfMemoryCopyFromBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, bytesToCopy);

	RaydPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"RaydGetReportDescriptor Exit = 0x%x\n", status);

	return status;
}


NTSTATUS
RaydGetDeviceAttributes(
	IN WDFREQUEST Request
)
{
	NTSTATUS                 status = STATUS_SUCCESS;
	PHID_DEVICE_ATTRIBUTES   deviceAttributes = NULL;

	RaydPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"RaydGetDeviceAttributes Entry\n");

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputBuffer(Request,
		sizeof(HID_DEVICE_ATTRIBUTES),
		(PVOID *)&deviceAttributes,
		NULL);
	if (!NT_SUCCESS(status))
	{
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Set USB device descriptor
	//

	deviceAttributes->Size = sizeof(HID_DEVICE_ATTRIBUTES);
	deviceAttributes->VendorID = RAYD_VID;
	deviceAttributes->ProductID = RAYD_PID;
	deviceAttributes->VersionNumber = RAYD_VERSION;

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, sizeof(HID_DEVICE_ATTRIBUTES));

	RaydPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"RaydGetDeviceAttributes Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
RaydGetString(
	IN WDFREQUEST Request
)
{

	NTSTATUS status = STATUS_SUCCESS;
	PWSTR pwstrID;
	size_t lenID;
	WDF_REQUEST_PARAMETERS params;
	void *pStringBuffer = NULL;

	RaydPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"RaydGetString Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	switch ((ULONG_PTR)params.Parameters.DeviceIoControl.Type3InputBuffer & 0xFFFF)
	{
	case HID_STRING_ID_IMANUFACTURER:
		pwstrID = L"Rayd.\0";
		break;

	case HID_STRING_ID_IPRODUCT:
		pwstrID = L"Touch Screen\0";
		break;

	case HID_STRING_ID_ISERIALNUMBER:
		pwstrID = L"123123123\0";
		break;

	default:
		pwstrID = NULL;
		break;
	}

	lenID = pwstrID ? wcslen(pwstrID) * sizeof(WCHAR) + sizeof(UNICODE_NULL) : 0;

	if (pwstrID == NULL)
	{

		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"RaydGetString Invalid request type\n");

		status = STATUS_INVALID_PARAMETER;

		return status;
	}

	status = WdfRequestRetrieveOutputBuffer(Request,
		lenID,
		&pStringBuffer,
		&lenID);

	if (!NT_SUCCESS(status))
	{

		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"RaydGetString WdfRequestRetrieveOutputBuffer failed Status 0x%x\n", status);

		return status;
	}

	RtlCopyMemory(pStringBuffer, pwstrID, lenID);

	WdfRequestSetInformation(Request, lenID);

	RaydPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"RaydGetString Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
RaydWriteReport(
	IN PRAYD_CONTEXT DevContext,
	IN WDFREQUEST Request
)
{
	UNREFERENCED_PARAMETER(DevContext);

	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;

	RaydPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"RaydWriteReport Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET))
	{
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"RaydWriteReport Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"RaydWriteReport No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			//switch (transferPacket->reportId)
			//{
			//default:

				RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"RaydWriteReport Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				//break;
			//}
		}
	}

	RaydPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"RaydWriteReport Exit = 0x%x\n", status);

	return status;

}

NTSTATUS
RaydProcessVendorReport(
	IN PRAYD_CONTEXT DevContext,
	IN PVOID ReportBuffer,
	IN ULONG ReportBufferLen,
	OUT size_t* BytesWritten
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDFREQUEST reqRead;
	PVOID pReadReport = NULL;
	size_t bytesReturned = 0;

	RaydPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"RaydProcessVendorReport Entry\n");

	status = WdfIoQueueRetrieveNextRequest(DevContext->ReportQueue,
		&reqRead);

	if (NT_SUCCESS(status))
	{
		status = WdfRequestRetrieveOutputBuffer(reqRead,
			ReportBufferLen,
			&pReadReport,
			&bytesReturned);

		if (NT_SUCCESS(status))
		{
			//
			// Copy ReportBuffer into read request
			//

			if (bytesReturned > ReportBufferLen)
			{
				bytesReturned = ReportBufferLen;
			}

			RtlCopyMemory(pReadReport,
				ReportBuffer,
				bytesReturned);

			//
			// Complete read with the number of bytes returned as info
			//

			WdfRequestCompleteWithInformation(reqRead,
				status,
				bytesReturned);

			RaydPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
				"RaydProcessVendorReport %d bytes returned\n", bytesReturned);

			//
			// Return the number of bytes written for the write request completion
			//

			*BytesWritten = bytesReturned;

			RaydPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
				"%s completed, Queue:0x%p, Request:0x%p\n",
				DbgHidInternalIoctlString(IOCTL_HID_READ_REPORT),
				DevContext->ReportQueue,
				reqRead);
		}
		else
		{
			RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"WdfRequestRetrieveOutputBuffer failed Status 0x%x\n", status);
		}
	}
	else
	{
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfIoQueueRetrieveNextRequest failed Status 0x%x\n", status);
	}

	RaydPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"RaydProcessVendorReport Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
RaydReadReport(
	IN PRAYD_CONTEXT DevContext,
	IN WDFREQUEST Request,
	OUT BOOLEAN* CompleteRequest
)
{
	NTSTATUS status = STATUS_SUCCESS;

	RaydPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"RaydReadReport Entry\n");

	//
	// Forward this read request to our manual queue
	// (in other words, we are going to defer this request
	// until we have a corresponding write request to
	// match it with)
	//

	status = WdfRequestForwardToIoQueue(Request, DevContext->ReportQueue);

	if (!NT_SUCCESS(status))
	{
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestForwardToIoQueue failed Status 0x%x\n", status);
	}
	else
	{
		*CompleteRequest = FALSE;
	}

	RaydPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"RaydReadReport Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
RaydSetFeature(
	IN PRAYD_CONTEXT DevContext,
	IN WDFREQUEST Request,
	OUT BOOLEAN* CompleteRequest
)
{
	UNREFERENCED_PARAMETER(CompleteRequest);

	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;
	RaydFeatureReport* pReport = NULL;

	RaydPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"RaydSetFeature Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET))
	{
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"RaydSetFeature Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"RaydWriteReport No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			case REPORTID_FEATURE:

				if (transferPacket->reportBufferLen == sizeof(RaydFeatureReport))
				{
					pReport = (RaydFeatureReport*)transferPacket->reportBuffer;

					DevContext->DeviceMode = pReport->DeviceMode;

					RaydPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
						"RaydSetFeature DeviceMode = 0x%x\n", DevContext->DeviceMode);
				}
				else
				{
					status = STATUS_INVALID_PARAMETER;

					RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
						"RaydSetFeature Error transferPacket->reportBufferLen (%d) is different from sizeof(RaydFeatureReport) (%d)\n",
						transferPacket->reportBufferLen,
						sizeof(RaydFeatureReport));
				}

				break;

			default:

				RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"RaydSetFeature Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	RaydPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"RaydSetFeature Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
RaydGetFeature(
	IN PRAYD_CONTEXT DevContext,
	IN WDFREQUEST Request,
	OUT BOOLEAN* CompleteRequest
)
{
	UNREFERENCED_PARAMETER(CompleteRequest);

	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;

	RaydPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"RaydGetFeature Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.OutputBufferLength < sizeof(HID_XFER_PACKET))
	{
		RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"RaydGetFeature Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"RaydGetFeature No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			case REPORTID_MTOUCH:
			{

				RaydMaxCountReport* pReport = NULL;

				if (transferPacket->reportBufferLen == sizeof(RaydMaxCountReport))
				{
					pReport = (RaydMaxCountReport*)transferPacket->reportBuffer;

					pReport->MaximumCount = MULTI_MAX_COUNT;

					RaydPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
						"RaydGetFeature MaximumCount = 0x%x\n", MULTI_MAX_COUNT);
				}
				else
				{
					status = STATUS_INVALID_PARAMETER;

					RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
						"RaydGetFeature Error transferPacket->reportBufferLen (%d) is different from sizeof(RaydMaxCountReport) (%d)\n",
						transferPacket->reportBufferLen,
						sizeof(RaydMaxCountReport));
				}

				break;
			}

			case REPORTID_FEATURE:
			{

				RaydFeatureReport* pReport = NULL;

				if (transferPacket->reportBufferLen == sizeof(RaydFeatureReport))
				{
					pReport = (RaydFeatureReport*)transferPacket->reportBuffer;

					pReport->DeviceMode = DevContext->DeviceMode;

					pReport->DeviceIdentifier = 0;

					RaydPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
						"RaydGetFeature DeviceMode = 0x%x\n", DevContext->DeviceMode);
				}
				else
				{
					status = STATUS_INVALID_PARAMETER;

					RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
						"RaydGetFeature Error transferPacket->reportBufferLen (%d) is different from sizeof(RaydFeatureReport) (%d)\n",
						transferPacket->reportBufferLen,
						sizeof(RaydFeatureReport));
				}

				break;
			}

			default:

				RaydPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"RaydGetFeature Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	RaydPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"RaydGetFeature Exit = 0x%x\n", status);

	return status;
}

PCHAR
DbgHidInternalIoctlString(
	IN ULONG IoControlCode
)
{
	switch (IoControlCode)
	{
	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
		return "IOCTL_HID_GET_DEVICE_DESCRIPTOR";
	case IOCTL_HID_GET_REPORT_DESCRIPTOR:
		return "IOCTL_HID_GET_REPORT_DESCRIPTOR";
	case IOCTL_HID_READ_REPORT:
		return "IOCTL_HID_READ_REPORT";
	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
		return "IOCTL_HID_GET_DEVICE_ATTRIBUTES";
	case IOCTL_HID_WRITE_REPORT:
		return "IOCTL_HID_WRITE_REPORT";
	case IOCTL_HID_SET_FEATURE:
		return "IOCTL_HID_SET_FEATURE";
	case IOCTL_HID_GET_FEATURE:
		return "IOCTL_HID_GET_FEATURE";
	case IOCTL_HID_GET_STRING:
		return "IOCTL_HID_GET_STRING";
	case IOCTL_HID_ACTIVATE_DEVICE:
		return "IOCTL_HID_ACTIVATE_DEVICE";
	case IOCTL_HID_DEACTIVATE_DEVICE:
		return "IOCTL_HID_DEACTIVATE_DEVICE";
	case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
		return "IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST";
	case IOCTL_HID_SET_OUTPUT_REPORT:
		return "IOCTL_HID_SET_OUTPUT_REPORT";
	case IOCTL_HID_GET_INPUT_REPORT:
		return "IOCTL_HID_GET_INPUT_REPORT";
	default:
		return "Unknown IOCTL";
	}
}