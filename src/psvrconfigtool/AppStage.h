#ifndef APP_STAGE_H
#define APP_STAGE_H

#include "PSVRClient_CAPI.h"

class AppStage
{
public:
    AppStage(class App *app) 
        : m_app(app)
    { }

    virtual bool init(int argc, char** argv) { return true; }
    virtual void destroy() {}

    virtual void enter() {}
    virtual void exit() {}
    virtual void update() {}
    virtual void render() {};
    virtual void renderUI() {}

    virtual void onKeyDown(int keyCode) {}
    virtual bool onClientAPIEvent(
        PSVREventMessage::eEventType event, 
        PSVREventDataHandle opaque_event_handle) 
    { return false; }
    virtual void onServiceResponse(PSVRResult ResultCode) {}

protected:
    class App *m_app;
};

#endif // APP_STAGE_H