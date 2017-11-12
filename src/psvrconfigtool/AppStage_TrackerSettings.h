#ifndef APP_STAGE_TRACKER_SETTINGS_H
#define APP_STAGE_TRACKER_SETTINGS_H

//-- includes -----
#include "AppStage.h"
#include "PSVRClient_CAPI.h"

#include <vector>

//-- definitions -----
class AppStage_TrackerSettings : public AppStage
{
public:
    struct ControllerInfo
    {
        int ControllerID;
		PSVRTrackingColorType TrackingColorType;
        PSVRControllerType ControllerType;
    };

	struct HMDInfo
	{
		int HmdID;
		PSVRTrackingColorType TrackingColorType;
		PSVRHmdType HmdType;
	};

    AppStage_TrackerSettings(class App *app);

    const PSVRClientTrackerInfo *getSelectedTrackerInfo() const;
	void set_selectedTrackerIndex(int index);

	void set_selectedControllerIndex(int index);

	int get_tracker_count() const;
	int get_tracker_Index() const;

	int get_controller_count() const;
	const ControllerInfo * get_controller_info(int index) const;
	const ControllerInfo *get_selected_controller();

	const HMDInfo *get_selected_hmd();


    virtual void enter() override;
    virtual void exit() override;
    virtual void update() override;
    virtual void render() override;

    virtual void renderUI() override;

    static const char *APP_STAGE_NAME;

	void gotoControllerColorCalib(bool value = false) { m_gotoControllerColorCalib = value; }
    void gotoHMDColorCalib(bool value = false) { m_gotoHMDColorCalib = value; }
	void gotoTestControllerTracking(bool value = false) { m_gotoTestControllerTracking = value; }
	void gotoTrackingControllerVideo(bool value = false) { m_gotoTrackingControllerVideo = value; }
	void gotoTestHMDTracking(bool value = false) { m_gotoTestHmdTracking = value; }
    void gotoTrackingHMDVideo(bool value = false) { m_gotoTrackingHmdVideo = value; }
	void gotoTrackingVideoALL(bool value = false) { m_gotoTrackingVideoALL = value; }

protected:
    virtual bool onClientAPIEvent(
        PSVREventMessage::eEventType event, 
        PSVREventDataHandle opaque_event_handle) override;

    void request_tracker_list();
    static void handle_tracker_list_response(
        const PSVRResponseMessage *response,
        void *userdata);

    void request_controller_list();
    static void handle_controller_list_response(
        const PSVRResponseMessage *response_message,
        void *userdata);

	void request_hmd_list();
	static void handle_hmd_list_response(
		const PSVRResponseMessage *response_message,
		void *userdata);

    void request_search_for_new_trackers();
    static void handle_search_for_new_trackers_response(
        const PSVRResponseMessage *response,
        void *userdata);

protected:
    enum eTrackerMenuState
    {
        inactive,
        idle,

        pendingTrackerListRequest,
        failedTrackerListRequest,
		pendingControllerListRequest,
		failedControllerListRequest,
		pendingHmdListRequest,
		failedHmdListRequest,
        pendingSearchForNewTrackersRequest,
    };
    eTrackerMenuState m_menuState;

    std::vector<PSVRClientTrackerInfo> m_trackerInfos;
	std::vector<ControllerInfo> m_controllerInfos;
	std::vector<HMDInfo> m_hmdInfos;

    int m_selectedTrackerIndex;
	int m_selectedControllerIndex;
	int m_selectedHmdIndex;

	bool m_gotoControllerColorCalib;
    bool m_gotoHMDColorCalib;
	bool m_gotoTestControllerTracking;
	bool m_gotoTrackingControllerVideo;
	bool m_gotoTestHmdTracking;
    bool m_gotoTrackingHmdVideo;
	bool m_gotoTrackingVideoALL;
};

#endif // APP_STAGE_TRACKER_SETTINGS_H