//-- includes -----
#include "USBApiInterface.h"
#include "USBDeviceManager.h"
#include "USBDeviceFilter.h"
#include "LibUSBBulkTransferBundle.h"
#include "LibUSBApi.h"
#include "NullUSBApi.h"
#include "WinUSBApi.h"
#include "Logger.h"
#include "Utility.h"

#include <atomic>
#include <thread>
#include <vector>
#include <map>

#include "readerwriterqueue.h" // lockfree queue

//-- typedefs -----
typedef std::map<t_usb_device_handle, USBDeviceState *> t_usb_device_map;
typedef std::map<t_usb_device_handle, USBDeviceState *>::iterator t_usb_device_map_iterator;
typedef std::pair<t_usb_device_handle, USBDeviceState *> t_handle_usb_device_pair;

//-- constants -----
const char * k_nullusb_api_name= "nullusb_api";
const char * k_libusb_api_name= "libusb_api";
const char * k_winusb_api_name= "winusb_api";

//-- private implementation -----

//-- USB Manager Config -----
const int USBManagerConfig::CONFIG_VERSION = 1;

USBManagerConfig::USBManagerConfig(const std::string &fnamebase)
    : PSVRConfig(fnamebase)
{
	usb_api_name= k_libusb_api_name;
	enable_usb_transfers= true;
};

const configuru::Config
USBManagerConfig::writeToJSON()
{
    configuru::Config pt{
        {"version", USBManagerConfig::CONFIG_VERSION},
	    {"usb_api", usb_api_name},
	    {"enable_usb_transfers", enable_usb_transfers}
    };

    return pt;
}

void
USBManagerConfig::readFromJSON(const configuru::Config &pt)
{
    version = pt.get_or<int>("version", 0);

    if (version == USBManagerConfig::CONFIG_VERSION)
    {
		usb_api_name = pt.get_or<std::string>("usb_api", usb_api_name);
		enable_usb_transfers = pt.get_or<bool>("enable_usb_transfers", enable_usb_transfers);
    }
    else
    {
        PSVR_LOG_WARNING("USBManagerConfig") <<
            "Config version " << version << " does not match expected version " <<
            USBManagerConfig::CONFIG_VERSION << ", Using defaults.";
    }
}

// -USBAsyncRequestManagerImpl-
/// Internal implementation of the USB async request manager.
class USBDeviceManagerImpl
{
public:
    USBDeviceManagerImpl()
        : m_api_type(_USBApiType_INVALID)
		, m_usb_api(nullptr)
        , m_exit_signaled({ false })
		, m_active_bulk_transfers(0)
        , m_active_control_transfers(0)
		, m_active_interrupt_transfers(0)
		, m_transfers_enabled(false)
        , m_thread_started(false)
		, m_next_usb_device_handle(0)
    {

    }

    virtual ~USBDeviceManagerImpl()
    {
    }

    // -- System ----
    bool startup(USBManagerConfig &cfg)
    {
        bool bSuccess= false;

		m_transfers_enabled= cfg.enable_usb_transfers;

		if (m_usb_api == nullptr)
		{
			if (cfg.usb_api_name == k_nullusb_api_name)
			{
				PSVR_LOG_INFO("USBAsyncRequestManager::startup") << "Requested NullUSBApi";
				m_api_type= _USBApiType_NullUSB;
			}
			else if (cfg.usb_api_name == k_libusb_api_name)
			{
				PSVR_LOG_INFO("USBAsyncRequestManager::startup") << "Requested LibUSBApi";
				m_api_type= _USBApiType_LibUSB;
			}
			else if (cfg.usb_api_name == k_winusb_api_name)
			{
				PSVR_LOG_INFO("USBAsyncRequestManager::startup") << "Requested WinUSBApi";
				m_api_type= _USBApiType_WinUSB;
			}
			else
			{
				PSVR_LOG_WARNING("USBAsyncRequestManager::startup") << "Requested unknown usb_api: \'" << cfg.usb_api_name << "\'. Defaulting to " << k_libusb_api_name;
				m_api_type= _USBApiType_LibUSB;
			}

			switch (m_api_type)
			{
			case _USBApiType_NullUSB:
				PSVR_LOG_INFO("USBAsyncRequestManager::startup") << "Creating NullUSBApi";
				m_usb_api = new NullUSBApi;
				break;
			case _USBApiType_LibUSB:
				PSVR_LOG_INFO("USBAsyncRequestManager::startup") << "Creating LibUSBApi";
				m_usb_api = new LibUSBApi;
				break;
			case _USBApiType_WinUSB:
#ifdef _WIN32
				PSVR_LOG_INFO("USBAsyncRequestManager::startup") << "Creating WinUSBApi";
				m_usb_api = new WinUSBApi;
#else
				PSVR_LOG_INFO("USBAsyncRequestManager::startup") << "Creating LibUSBApi (WinUSBApi not available on this platform)";
				m_usb_api = new LibUSBApi;
#endif
				break;
			default:
				assert(0 && "unreachable");
				break;
			}

			if (m_usb_api != nullptr && m_usb_api->startup())
			{
				PSVR_LOG_INFO("USBAsyncRequestManager::startup") << "Initialized USB API";
				startWorkerThread();
				bSuccess= true;
			}
			else
			{
				PSVR_LOG_ERROR("USBAsyncRequestManager::startup") << "Failed to initialize USB API";
			}
		}
		else
		{
			PSVR_LOG_WARNING("USBAsyncRequestManager::startup") << "USB API aready initialized";
			bSuccess= true;
		}

        return bSuccess;
    }

    void update()
    {
        // If the thread terminated, reset the started and exited flags
        if (m_exit_signaled)
        {
            m_thread_started= false;
            m_exit_signaled= false;
        }

		if (m_transfers_enabled)
		{
			processResults();
		}
    }

    void shutdown()
    {
        // Shutdown any async transfers
        if (m_thread_started)
        {
            stopWorkerThread();
        }

        // Cleanup any requests
		if (m_transfers_enabled)
		{
			requestProcessingTeardown();
		}

        if (m_usb_api != nullptr)
        {
            // Unref any libusb devices
            freeDeviceStateList();

            // Free the usb context
			delete m_usb_api;
            m_usb_api= nullptr;
			m_api_type = _USBApiType_INVALID;
        }
    }

    // -- Device Actions ----
	t_usb_device_handle openUSBDevice(
        struct USBDeviceEnumerator* enumerator, 
        int interface_index,
        int configuration_index,
        bool reset_device)
    {
		t_usb_device_handle handle= k_invalid_usb_device_handle;

		USBDeviceState *state = m_usb_api->open_usb_device(enumerator, interface_index, configuration_index, reset_device);

        if (state != nullptr)
        {
			handle = m_next_usb_device_handle;
			state->public_handle = m_next_usb_device_handle;
			++m_next_usb_device_handle;

			m_device_state_map.insert(t_handle_usb_device_pair(state->public_handle, state));
        }

        return handle;
    }

    void closeUSBDevice(t_usb_device_handle handle)
    {
		t_usb_device_map_iterator iter= m_device_state_map.find(handle);

		if (iter != m_device_state_map.end())
		{
			USBDeviceState *usb_device_state= iter->second;

			m_device_state_map.erase(iter);
			m_usb_api->close_usb_device(usb_device_state);
		}
    }

	bool canUSBDeviceBeOpened(struct USBDeviceEnumerator* enumerator, char *outReason, size_t bufferSize)
	{
		return m_usb_api->can_usb_device_be_opened(enumerator, outReason, bufferSize);
	}

    bool getIsUSBDeviceOpen(t_usb_device_handle handle) const
    {
		bool bIsOpen= (m_device_state_map.find(handle) != m_device_state_map.end());

        return bIsOpen;
    }

    // -- Request Queue ----
    bool submitTransferRequest(const USBTransferRequest &request, std::function<void(USBTransferResult&)> callback)
    {
		bool bAddedRequest= false;

		if (m_transfers_enabled)
		{
			USBTransferRequestState requestState = {request, callback};

			if (request_queue.enqueue(requestState))
			{
				// Give the other thread a chance to process the request
				Utility::sleep_ms(10);
				bAddedRequest= true;
			}
		}
		else
		{
			USBTransferResult result;
			memset(&result, 0, sizeof(USBTransferResult));

			switch (request.request_type)
			{
			case eUSBTransferRequestType::_USBRequestType_InterruptTransfer:
				result.result_type= _USBResultType_InterruptTransfer;
				result.payload.interrupt_transfer.result_code= eUSBResultCode::_USBResultCode_SubmitFailed;
				result.payload.interrupt_transfer.usb_device_handle= request.payload.interrupt_transfer.usb_device_handle;
				break;
			case eUSBTransferRequestType::_USBRequestType_ControlTransfer:
				result.result_type= _USBResultType_ControlTransfer;
				result.payload.control_transfer.result_code= eUSBResultCode::_USBResultCode_SubmitFailed;
				result.payload.control_transfer.usb_device_handle= request.payload.control_transfer.usb_device_handle;
				break;
			case eUSBTransferRequestType::_USBRequestType_BulkTransfer:
				result.result_type= _USBResultType_BulkTransfer;
				result.payload.control_transfer.result_code= eUSBResultCode::_USBResultCode_SubmitFailed;
				result.payload.control_transfer.usb_device_handle= request.payload.bulk_transfer.usb_device_handle;
				break;
			case eUSBTransferRequestType::_USBRequestType_StartBulkTransferBundle:
				result.result_type= _USBResultType_BulkTransferBundle;
				result.payload.bulk_transfer.result_code= eUSBResultCode::_USBResultCode_SubmitFailed;
				result.payload.bulk_transfer.usb_device_handle= request.payload.start_bulk_transfer_bundle.usb_device_handle;
				break;
			case eUSBTransferRequestType::_USBRequestType_CancelBulkTransferBundle:
				result.result_type= _USBResultType_BulkTransferBundle;
				result.payload.bulk_transfer.result_code= eUSBResultCode::_USBResultCode_SubmitFailed;
				result.payload.bulk_transfer.usb_device_handle= request.payload.cancel_bulk_transfer_bundle.usb_device_handle;
				break;
			}
			
			callback(result);
		}

        return bAddedRequest;
    }

	// -- accessors ----
	inline const IUSBApi *getUSBApiConst() const { return m_usb_api; }
	inline IUSBApi *getUSBApi() { return m_usb_api; }

	bool getUsbDeviceFilter(t_usb_device_handle handle, USBDeviceFilter &outDeviceInfo)
	{
		t_usb_device_map_iterator iter = m_device_state_map.find(handle);
		bool bSuccess = false;

		if (iter != m_device_state_map.end())
		{
			bSuccess= m_usb_api->get_usb_device_filter(iter->second, &outDeviceInfo);
		}

		return bSuccess;
	}

	bool getUsbDeviceFullPath(t_usb_device_handle handle, char *outBuffer, size_t bufferSize)
	{
		t_usb_device_map_iterator iter = m_device_state_map.find(handle);
		bool bSuccess = false;

		if (iter != m_device_state_map.end())
		{
			bSuccess = m_usb_api->get_usb_device_path(iter->second, outBuffer, bufferSize);
		}

		return bSuccess;
	}

	bool getUsbDevicePortPath(t_usb_device_handle handle, char *outBuffer, size_t bufferSize)
	{
		t_usb_device_map_iterator iter = m_device_state_map.find(handle);
		bool bSuccess = false;

		if (iter != m_device_state_map.end())
		{
			bSuccess = m_usb_api->get_usb_device_port_path(iter->second, outBuffer, bufferSize);
		}

		return bSuccess;
	}

	bool getUsbDeviceIsOpen(t_usb_device_handle handle)
	{
		t_usb_device_map_iterator iter = m_device_state_map.find(handle);
		const bool bIsOpen = (iter != m_device_state_map.end());

		return bIsOpen;
	}

	void postUSBTransferResult(const USBTransferResult &result, std::function<void(USBTransferResult&)> callback)
	{
		USBTransferResultState state = { result, callback };

		// If a control transfer just completed (successfully or unsuccessfully)
		// decrement the outstanding control transfer count
		if (result.result_type == _USBResultType_ControlTransfer)
		{
			assert(m_active_control_transfers > 0);
			--m_active_control_transfers;
		}
		// If a interrupt transfer just completed (successfully or unsuccessfully)
		// decrement the outstanding interrupt transfer count
		else if (result.result_type == _USBResultType_InterruptTransfer)
		{
			assert(m_active_interrupt_transfers > 0);
			--m_active_interrupt_transfers;
		}
		// If a bulk transfer just completed (successfully or unsuccessfully)
		// decrement the outstanding bulk transfer count
		else if (result.result_type == _USBResultType_BulkTransfer)
		{
			assert(m_active_bulk_transfers > 0);
			--m_active_bulk_transfers;
		}

		result_queue.enqueue(state);
	}

protected:
    void startWorkerThread()
    {
        if (!m_thread_started)
        {
            PSVR_LOG_INFO("USBAsyncRequestManager::startup") << "Starting USB event thread";
            m_worker_thread = std::thread(&USBDeviceManagerImpl::workerThreadFunc, this);
            m_thread_started = true;
        }
    }

    bool processRequests()
    {
        bool bHadRequests= false;

        // Process incoming USB transfer requests
		USBTransferRequestState requestState;
        while (request_queue.try_dequeue(requestState))
        {
            switch (requestState.request.request_type)
            {
			case eUSBTransferRequestType::_USBRequestType_InterruptTransfer:
				handleInterruptTransferRequest(requestState);
				break;
            case eUSBTransferRequestType::_USBRequestType_ControlTransfer:
                handleControlTransferRequest(requestState);
                break;
            case eUSBTransferRequestType::_USBRequestType_BulkTransfer:
                handleBulkTransferRequest(requestState);
                break;
            case eUSBTransferRequestType::_USBRequestType_StartBulkTransferBundle:
                handleStartBulkTransferRequest(requestState);
                break;
            case eUSBTransferRequestType::_USBRequestType_CancelBulkTransferBundle:
                handleCancelBulkTransferRequest(requestState);
                break;
            }

            bHadRequests= true;
        }

        if (m_active_bulk_transfer_bundles.size() > 0 || 
            m_canceled_bulk_transfer_bundles.size() > 0 ||
			m_active_bulk_transfers > 0 ||
            m_active_control_transfers > 0 ||
			m_active_interrupt_transfers > 0)
        {
            // If we have a blocking transfer pending keep polling until we get the result back.
			// Otherwise just poll once.
            do
            {
				m_usb_api->poll();
            } while (m_active_bulk_transfers > 0 || m_active_control_transfers > 0 || m_active_interrupt_transfers > 0);

            // Cleanup any requests that no longer have any pending cancellations
            cleanupCanceledRequests(false);
        }

        return bHadRequests;
    }

    void processResults()
    {
        USBTransferResultState resultState;

        // Process all pending results
        while (result_queue.try_dequeue(resultState))
        {
            // Fire the callback on the result
            resultState.callback(resultState.result);
        }
    }

    void requestProcessingTeardown()
    {
        // Drain the request queue
        while (request_queue.pop());

        // Cancel all active transfers
        while (m_active_bulk_transfer_bundles.size() > 0)
        {
            IUSBBulkTransferBundle *bundle= m_active_bulk_transfer_bundles.back();
            m_active_bulk_transfer_bundles.pop_back();
            bundle->cancelTransfers();
            m_canceled_bulk_transfer_bundles.push_back(bundle);
        }

        // Wait for the canceled bulk transfers and control transfers to exit
		const int k_max_cleanup_poll_attempts= 100;
		int cleanup_attempts= 0;
        while ((m_canceled_bulk_transfer_bundles.size() > 0 || m_active_control_transfers > 0 || m_active_interrupt_transfers > 0) &&
				cleanup_attempts < k_max_cleanup_poll_attempts)
        {
			m_usb_api->poll();

            // Cleanup any requests that no longer have any pending cancellations
            cleanupCanceledRequests(false);

			++cleanup_attempts;
        }

		if (m_canceled_bulk_transfer_bundles.size() > 0)
		{
			cleanupCanceledRequests(true);
		}
    }

    void workerThreadFunc()
    {
        Utility::set_current_thread_name("USB Async Worker Thread");

        // Stay in the message loop until asked to exit by the main thread
        while (!m_exit_signaled)
        {
            processRequests();
        }
    }

    void cleanupCanceledRequests(bool bForceCleanup)
    {
		auto it = m_canceled_bulk_transfer_bundles.begin();
        while (it != m_canceled_bulk_transfer_bundles.end())
        {
            IUSBBulkTransferBundle *bundle = *it;

            if (bundle->getActiveTransferCount() == 0 || bForceCleanup)
            {
                it= m_canceled_bulk_transfer_bundles.erase(it);
                delete bundle;
            }
			else
			{
				++it;
			}
        }
    }

	void handleInterruptTransferRequest(const USBTransferRequestState &requestState)
	{
		const USBRequestPayload_InterruptTransfer &request = requestState.request.payload.interrupt_transfer;

		t_usb_device_map_iterator iter = m_device_state_map.find(request.usb_device_handle);
		USBDeviceState *state = iter->second;

		eUSBResultCode result_code;
		bool bSuccess = true;

#if defined(DEBUG_USB)
		if ((request.endpoint & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT)
		{
			debug("USBMgr REQUEST: interrupt transfer write - dev: %d, endpoint: 0x%X, datalen: %d\n",
				requestState.request.payload.interrupt_transfer.usb_device_handle,
				requestState.request.payload.interrupt_transfer.endpoint,
				requestState.request.payload.interrupt_transfer.length);
		}
		else
		{
			debug("USBMgr REQUEST: control transfer read - dev: %d, endpoint: 0x%X, datalen: %d\n",
				requestState.request.payload.interrupt_transfer.usb_device_handle,
				requestState.request.payload.interrupt_transfer.endpoint,
				requestState.request.payload.interrupt_transfer.length);
		}
#endif

        ++m_active_interrupt_transfers;

		if (state != nullptr)
		{
			result_code= m_usb_api->submit_interrupt_transfer(state, &requestState);
			if (result_code != _USBResultCode_Started && result_code != _USBResultCode_Completed)
			{
				bSuccess = false;
			}
		}
		else
		{
			result_code = _USBResultCode_BadHandle;
			bSuccess = false;
		}

		// If the control transfer didn't successfully start, post a failure result now
		if (!bSuccess)
		{
			USBTransferResult result;

			memset(&result, 0, sizeof(USBTransferResult));
			result.payload.interrupt_transfer.usb_device_handle = request.usb_device_handle;
			result.payload.interrupt_transfer.result_code = result_code;
			result.result_type = _USBResultType_InterruptTransfer;

			postUSBTransferResult(result, requestState.callback);
		}
	}

    void handleControlTransferRequest(const USBTransferRequestState &requestState)
    {
        const USBRequestPayload_ControlTransfer &request = requestState.request.payload.control_transfer;

		t_usb_device_map_iterator iter = m_device_state_map.find(request.usb_device_handle);
		USBDeviceState *state = iter->second;

        eUSBResultCode result_code;
        bool bSuccess= true;

#if defined(DEBUG_USB)
        if ((request.bmRequestType & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT)
        {
            debug("USBMgr REQUEST: control transfer write - dev: %d, reg: 0x%X, value: 0x%x\n", 
                requestState.request.payload.control_transfer.usb_device_handle,
                requestState.request.payload.control_transfer.wIndex,
                requestState.request.payload.control_transfer.data[0]);
        }
        else
        {
            debug("USBMgr REQUEST: control transfer read - dev: %d, reg: 0x%X\n", 
                requestState.request.payload.control_transfer.usb_device_handle,
                requestState.request.payload.control_transfer.wIndex);
        }
#endif

        ++m_active_control_transfers;

		if (state != nullptr)
		{
			result_code = m_usb_api->submit_control_transfer(state, &requestState);
			if (result_code != _USBResultCode_Started && result_code != _USBResultCode_Completed)
			{
                bSuccess = false;
			}
		}
		else
		{
			result_code = _USBResultCode_BadHandle;
			bSuccess = false;
		}

        // If the control transfer didn't successfully start, post a failure result now
        if (!bSuccess)
        {
            USBTransferResult result;

            memset(&result, 0, sizeof(USBTransferResult));
            result.payload.control_transfer.usb_device_handle= request.usb_device_handle;
            result.payload.control_transfer.result_code= result_code;
            result.result_type = _USBResultType_ControlTransfer;

            postUSBTransferResult(result, requestState.callback);
        }
    }

	void handleBulkTransferRequest(const USBTransferRequestState &requestState)
	{
		const USBRequestPayload_BulkTransfer &request = requestState.request.payload.bulk_transfer;

		t_usb_device_map_iterator iter = m_device_state_map.find(request.usb_device_handle);
		USBDeviceState *state = iter->second;

		eUSBResultCode result_code;
		bool bSuccess = true;

#if defined(DEBUG_USB)
		if ((request.endpoint & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT)
		{
			debug("USBMgr REQUEST: bulk transfer write - dev: %d, endpoint: 0x%X, datalen: %d\n",
				requestState.request.payload.bulk_transfer.usb_device_handle,
				requestState.request.payload.bulk_transfer.endpoint,
				requestState.request.payload.bulk_transfer.length);
		}
		else
		{
			debug("USBMgr REQUEST: control transfer read - dev: %d, endpoint: 0x%X, datalen: %d\n",
				requestState.request.payload.bulk_transfer.usb_device_handle,
				requestState.request.payload.bulk_transfer.endpoint,
				requestState.request.payload.bulk_transfer.length);
		}
#endif

        ++m_active_bulk_transfers;

		if (state != nullptr)
		{
			result_code= m_usb_api->submit_bulk_transfer(state, &requestState);
			if (result_code != _USBResultCode_Started && result_code != _USBResultCode_Completed)
			{
				bSuccess = false;
			}
		}
		else
		{
			result_code = _USBResultCode_BadHandle;
			bSuccess = false;
		}

		// If the control transfer didn't successfully start, post a failure result now
		if (!bSuccess)
		{
			USBTransferResult result;

			memset(&result, 0, sizeof(USBTransferResult));
			result.payload.bulk_transfer.usb_device_handle = request.usb_device_handle;
			result.payload.bulk_transfer.result_code = result_code;
			result.result_type = _USBResultType_BulkTransfer;

			postUSBTransferResult(result, requestState.callback);
		}
	}

    void handleStartBulkTransferRequest(const USBTransferRequestState &requestState)
    {
        const USBRequestPayload_BulkTransferBundle &request= requestState.request.payload.start_bulk_transfer_bundle;

		t_usb_device_map_iterator iter = m_device_state_map.find(request.usb_device_handle);
		USBDeviceState *state = iter->second;

		eUSBResultCode result_code;
		bool bSuccess = true;

        if (state != nullptr)
        {
            // Only start a bulk transfer if the device doesn't have one going already
            auto it = std::find_if(
                m_active_bulk_transfer_bundles.begin(),
                m_active_bulk_transfer_bundles.end(),
                [&request](const IUSBBulkTransferBundle *bundle) {
                    return bundle->getUSBDeviceHandle() == request.usb_device_handle;
            });

            if (it == m_active_bulk_transfer_bundles.end())
            {
                IUSBBulkTransferBundle *bundle = m_usb_api->allocate_bulk_transfer_bundle(state, &requestState.request.payload.start_bulk_transfer_bundle);

                // Allocate and initialize the bulk transfers
                if (bundle->initialize())
                {
                    // Attempt to start all the transfers
                    if (bundle->startTransfers())
                    {
                        // Success! Add the bundle to the list of active bundles
                        m_active_bulk_transfer_bundles.push_back(bundle);
                        result_code = _USBResultCode_Started;
                    }
                    else
                    {                            
                        // Unable to start all of the transfers in the bundle
                        if (bundle->getActiveTransferCount() > 0)
                        {
                            // If any transfers started we have to cancel the ones that started
                            // and wait for the cancellation request to complete.
                            bundle->cancelTransfers();
                            m_canceled_bulk_transfer_bundles.push_back(bundle);
                        }
                        else
                        {
                            // No transfer requests started.
                            // Delete the bundle right away.
                            delete bundle;
                        }

                        result_code = _USBResultCode_SubmitFailed;
                    }
                }
                else
                {
                    result_code = _USBResultCode_NoMemory;
                    delete bundle;
                }
            }
            else
            {
                result_code = _USBResultCode_TransferAlreadyStarted;
            }
        }
        else
        {
            result_code = _USBResultCode_BadHandle;
        }

        // Post the transfer result to the outbound result queue
        {
            USBTransferResult result;

            result.result_type = _USBResultType_BulkTransferBundle;
            result.payload.bulk_transfer.usb_device_handle= request.usb_device_handle;
            result.payload.bulk_transfer.result_code = result_code;

            postUSBTransferResult(result, requestState.callback);
        }
    }

    void handleCancelBulkTransferRequest(const USBTransferRequestState &requestState)
    {
        const USBRequestPayload_CancelBulkTransferBundle &request= requestState.request.payload.cancel_bulk_transfer_bundle;

		t_usb_device_map_iterator iter = m_device_state_map.find(request.usb_device_handle);

        eUSBResultCode result_code;

        if (iter != m_device_state_map.end())
        {
			USBDeviceState *state = iter->second;

			auto it = std::find_if(
                m_active_bulk_transfer_bundles.begin(),
                m_active_bulk_transfer_bundles.end(),
                [&request](const IUSBBulkTransferBundle *bundle) {
                    return bundle->getUSBDeviceHandle() == request.usb_device_handle;
                });

            if (it != m_active_bulk_transfer_bundles.end())
            {
                IUSBBulkTransferBundle *bundle = *it;

                // Tell the bundle to cancel all active transfers.
                // This is an asynchronous operation.
                bundle->cancelTransfers();

                // Remove the bundle from the list of active transfers
                m_active_bulk_transfer_bundles.erase(it);

                // Put the bundle on the list of canceled transfers.
                // The bundle will get cleaned up once all active transfers are done.
                m_canceled_bulk_transfer_bundles.push_back(bundle);

                result_code = _USBResultCode_Canceled;
            }
            else
            {
                result_code = _USBResultCode_TransferNotActive;
            }
        }
        else
        {
            result_code= _USBResultCode_BadHandle;
        }

        // Post the transfer result to the outbound result queue
        {
            USBTransferResult result;

            result.result_type = _USBResultType_BulkTransferBundle;
            result.payload.bulk_transfer.usb_device_handle = request.usb_device_handle;
            result.payload.bulk_transfer.result_code = result_code;

            postUSBTransferResult(result, requestState.callback);
        }
    }

    void stopWorkerThread()
    {
        if (m_thread_started)
        {
            if (!m_exit_signaled)
            {
                PSVR_LOG_INFO("USBAsyncRequestManager::startup") << "Stopping USB event thread...";
                m_exit_signaled = true;
                m_worker_thread.join();
                PSVR_LOG_INFO("USBAsyncRequestManager::startup") << "USB event thread stopped";
            }
            else
            {
                PSVR_LOG_INFO("USBAsyncRequestManager::startup") << "USB event thread already stopped";
            }

            m_thread_started = false;
            m_exit_signaled = false;
        }
    }

    void freeDeviceStateList()
    {
        for (auto it = m_device_state_map.begin(); it != m_device_state_map.end(); ++it)
        {
            m_usb_api->close_usb_device(it->second);
        }

		m_device_state_map.clear();
    }

private:
    // Multithreaded state
	eUSBApiType m_api_type;
	IUSBApi *m_usb_api;
    bool m_bUseMultithreading;
    std::atomic_bool m_exit_signaled;
    moodycamel::ReaderWriterQueue<USBTransferRequestState, 128> request_queue;
    moodycamel::ReaderWriterQueue<USBTransferResultState, 128> result_queue;

    // Worker thread state
    std::vector<IUSBBulkTransferBundle *> m_active_bulk_transfer_bundles;
    std::vector<IUSBBulkTransferBundle *> m_canceled_bulk_transfer_bundles;
    int m_active_control_transfers;
	int m_active_interrupt_transfers;
    int m_active_bulk_transfers;

    // Main thread state
	bool m_transfers_enabled;
    bool m_thread_started;
    std::thread m_worker_thread;
    std::vector<USBDeviceFilter> m_device_whitelist;
	t_usb_device_map m_device_state_map;
	t_usb_device_handle m_next_usb_device_handle;
};

//-- public interface -----
USBDeviceManager *USBDeviceManager::m_instance = NULL;

USBDeviceManager::USBDeviceManager()
    : m_cfg()
	, m_implementation_ptr(new USBDeviceManagerImpl())
{
	m_cfg.load();

	// Save the config back out in case it doesn't exist
	m_cfg.save();
}

USBDeviceManager::~USBDeviceManager()
{
    if (m_instance != NULL)
    {
        PSVR_LOG_ERROR("~USBAsyncRequestManager()") << "USB Async Request Manager deleted without shutdown() getting called first";
    }

    if (m_implementation_ptr != nullptr)
    {
        delete m_implementation_ptr;
        m_implementation_ptr = nullptr;
    }
}

IUSBApi *USBDeviceManager::getUSBApiInterface()
{
	return USBDeviceManager::getInstance()->m_implementation_ptr->getUSBApi();
}

bool USBDeviceManager::startup()
{
    m_instance = this;
    return m_implementation_ptr->startup(m_cfg);
}

void USBDeviceManager::update()
{
    m_implementation_ptr->update();
}

void USBDeviceManager::shutdown()
{
    m_implementation_ptr->shutdown();
    m_instance = NULL;
}

// -- Device Enumeration ----
USBDeviceEnumerator* usb_device_enumerator_allocate()
{
	return USBDeviceManager::getUSBApiInterface()->device_enumerator_create();
}

bool usb_device_enumerator_is_valid(struct USBDeviceEnumerator* enumerator)
{
	return USBDeviceManager::getUSBApiInterface()->device_enumerator_is_valid(enumerator);
}

bool usb_device_enumerator_get_filter(struct USBDeviceEnumerator* enumerator, USBDeviceFilter &outDeviceInfo)
{
	return USBDeviceManager::getUSBApiInterface()->device_enumerator_get_filter(enumerator, &outDeviceInfo);
}

void usb_device_enumerator_next(struct USBDeviceEnumerator* enumerator)
{
	USBDeviceManager::getUSBApiInterface()->device_enumerator_next(enumerator);
}

void usb_device_enumerator_free(struct USBDeviceEnumerator* enumerator)
{
	USBDeviceManager::getUSBApiInterface()->device_enumerator_dispose(enumerator);
}

bool usb_device_enumerator_get_path(struct USBDeviceEnumerator* enumerator, char *outBuffer, size_t bufferSize)
{
	return USBDeviceManager::getUSBApiInterface()->device_enumerator_get_path(enumerator, outBuffer, bufferSize);
}

bool usb_device_enumerator_get_unique_identifier(struct USBDeviceEnumerator* enumerator, char *outBuffer, size_t bufferSize)
{
	return USBDeviceManager::getUSBApiInterface()->device_enumerator_get_unique_identifier(enumerator, outBuffer, bufferSize);
}

eUSBApiType usb_device_enumerator_get_driver_type(struct USBDeviceEnumerator* enumerator)
{
	return USBDeviceManager::getUSBApiInterface()->getRuntimeUSBApiType();
}

// -- Device Actions ----
t_usb_device_handle usb_device_open(
    struct USBDeviceEnumerator* enumerator, 
    int interface_index,
    int configuration_index,
    bool reset_device)
{
	return USBDeviceManager::getInstance()->getImplementation()->openUSBDevice(
        enumerator, interface_index, configuration_index, reset_device);
}

void usb_device_close(t_usb_device_handle usb_device_handle)
{
	USBDeviceManager::getInstance()->getImplementation()->closeUSBDevice(usb_device_handle);
}

bool usb_device_can_be_opened(struct USBDeviceEnumerator* enumerator, char *outReason, size_t bufferSize)
{
	return USBDeviceManager::getInstance()->getImplementation()->canUSBDeviceBeOpened(enumerator, outReason, bufferSize);
}

bool usb_device_submit_transfer_request_async(
	const USBTransferRequest &request,
	std::function<void(USBTransferResult&)> callback)
{
	return USBDeviceManager::getInstance()->getImplementation()->submitTransferRequest(request, callback);
}

// Send the transfer request to the worker thread and block until it completes
USBTransferResult usb_device_submit_transfer_request_blocking(const USBTransferRequest &request)
{
	USBDeviceManagerImpl *deviceManagerImpl= USBDeviceManager::getInstance()->getImplementation();

	USBTransferResult result;
	bool bIsPending = true;

	// Submit the async usb control transfer request to the worker thread
	deviceManagerImpl->submitTransferRequest(
		request,
		[&result, &bIsPending](USBTransferResult &r)
		{
			result = r;
			bIsPending = false;
		}
	);

	// Spin until the transfer completes
	while (bIsPending)
	{
		// Give the worker thread a chance to do work
		Utility::sleep_ms(1);

		// Poll to see if the transfer completed
		// (will execute the callback on completion)
		deviceManagerImpl->update();
	}

	return result;
}

// -- Device Queries ----
bool usb_device_get_filter(t_usb_device_handle handle, USBDeviceFilter &outDeviceInfo)
{
	return USBDeviceManager::getInstance()->getImplementation()->getUsbDeviceFilter(handle, outDeviceInfo);
}

bool usb_device_get_full_path(t_usb_device_handle handle, char *outBuffer, size_t bufferSize)
{
	return USBDeviceManager::getInstance()->getImplementation()->getUsbDeviceFullPath(handle, outBuffer, bufferSize);
}

bool usb_device_get_port_path(t_usb_device_handle handle, char *outBuffer, size_t bufferSize)
{
	return USBDeviceManager::getInstance()->getImplementation()->getUsbDevicePortPath(handle, outBuffer, bufferSize);
}

bool usb_device_get_is_open(t_usb_device_handle handle)
{
	return USBDeviceManager::getInstance()->getImplementation()->getUsbDeviceIsOpen(handle);
}

const char *usb_device_get_error_string(eUSBResultCode result_code)
{
	const char *result = "UNKNOWN USB ERROR";

	switch (result_code)
	{
	case _USBResultCode_Started:
		result = "Transfer Started";
		break;
	case _USBResultCode_Canceled:
		result = "Transfer Cancelled";
		break;
	case _USBResultCode_Completed:
		result = "Transfer Completed";
		break;
	case _USBResultCode_GeneralError:
		result = "General USB Error";
		break;
	case _USBResultCode_BadHandle:
		result = "Bad USB handle";
		break;
	case _USBResultCode_NoMemory:
		result = "Out of Memory";
		break;
	case _USBResultCode_SubmitFailed:
		result = "Transfer Submit Failed";
		break;
	case _USBResultCode_DeviceNotOpen:
		result = "USB Device Not Open";
		break;
	case _USBResultCode_TransferNotActive:
		result = "Transfer Not Active";
		break;
	case _USBResultCode_TransferAlreadyStarted:
		result = "Transfer Already Active";
		break;
	case _USBResultCode_Overflow:
		result = "Overflow Error";
		break;
	case _USBResultCode_Pipe:
		result = "Pipe Error";
		break;
	case _USBResultCode_TimedOut:
		break;
	}

	return result;
}

// -- Notifications ----
void usb_device_post_transfer_result(const USBTransferResult &result, std::function<void(USBTransferResult&)> callback)
{
	return USBDeviceManager::getInstance()->getImplementation()->postUSBTransferResult(result, callback);
}
