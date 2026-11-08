/*---------------------------------------------------------*\
| KMP6Plugin.h                                              |
|                                                           |
|   OpenRGB 1.1 plugin for the Aukey KM-P6 RGB mouse pad    |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <QLabel>
#include <QObject>
#include <QTimer>

#include "OpenRGBPluginInterface.h"
#include "UsbTransport.h"
#include "KMP6Protocol.h"

class KMP6Plugin : public QObject, public OpenRGBPluginInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID OpenRGBPluginInterface_IID FILE "../KMP6Plugin.json")
    Q_INTERFACES(OpenRGBPluginInterface)

public:
    ~KMP6Plugin() override;

    OpenRGBPluginInfo       GetPluginInfo() override;
    unsigned int            GetPluginAPIVersion() override;

    void                    Load(OpenRGBPluginAPIInterface* plugin_api_ptr) override;
    QWidget*                GetWidget() override;
    QMenu*                  GetTrayMenu() override;
    void                    Unload() override;
    void                    OnProfileAboutToLoad() override;
    void                    OnProfileLoad(nlohmann::json profile_data) override;
    nlohmann::json          OnProfileSave() override;
    unsigned char*          OnSDKCommand(unsigned int pkt_id, unsigned char* pkt_data, unsigned int* pkt_size) override;

    void                    ProfileManagerUpdated(unsigned int update_reason) override;
    void                    ResourceManagerUpdated(unsigned int update_reason) override;
    void                    SettingsManagerUpdated(unsigned int update_reason) override;

private:
    /*-----------------------------------------------------*\
    | RGBController_Setup callbacks (called by OpenRGB with |
    | object_ptr = this); they only wake the worker         |
    \*-----------------------------------------------------*/
    static void             CallbackUpdateMode(void* object_ptr);
    static void             CallbackUpdateZoneMode(void* object_ptr, int zone);
    static void             CallbackUpdateLEDs(void* object_ptr);
    static void             CallbackUpdateZoneLEDs(void* object_ptr, int zone);
    static void             CallbackUpdateSingleLED(void* object_ptr, int led);

    void                    Wake();
    void                    WorkerThread();
    void                    Connect();
    void                    Disconnect();
    void                    Cycle();
    bool                    ApplyProfileRequest();
    kmp6::Settings          SettingsFromController();
    void                    SettingsToController(const kmp6::Settings& settings);
    void                    LoadStored();
    void                    SaveStored();
    void                    SetStatus(const std::string& text);
    void                    LogOnce(unsigned int level, const std::string& text);

    OpenRGBPluginAPIInterface*          api                 = nullptr;

    /*-----------------------------------------------------*\
    | Worker thread; all device I/O and (un)registering of  |
    | the controller happen there                           |
    \*-----------------------------------------------------*/
    std::thread                         worker;
    std::mutex                          wake_mutex;
    std::condition_variable             wake;
    bool                                stopping            = false;
    bool                                wake_pending        = false;
    std::atomic<bool>                   worker_done{false};

    /*-----------------------------------------------------*\
    | Set by the controller callbacks. A mode update is an  |
    | explicit "apply" (mode list, sliders, profile load)   |
    | and is sent even if nothing changed, e.g. after the   |
    | button on the pad changed the effect. LED updates are |
    | only sent when the colour really changed, so effects  |
    | and SDK clients repeating a colour write nothing.     |
    \*-----------------------------------------------------*/
    std::atomic<bool>                   mode_dirty{false};
    std::atomic<bool>                   leds_dirty{false};

    /*-----------------------------------------------------*\
    | A loaded OpenRGB profile waits here until the worker  |
    | applies it (also when the pad connects only later)    |
    \*-----------------------------------------------------*/
    std::mutex                          request_mutex;
    nlohmann::json                      profile_request;
    bool                                has_profile_request = false;
    /*-----------------------------------------------------*\
    | Guards the controller pointer against OnProfileSave   |
    | on the GUI thread; only the worker changes it         |
    \*-----------------------------------------------------*/
    std::mutex                          controller_mutex;

    /*-----------------------------------------------------*\
    | Worker-only state                                     |
    \*-----------------------------------------------------*/
    std::unique_ptr<kmp6::Transport>    transport;
    std::unique_ptr<kmp6::Pad>          pad;
    RGBControllerInterface*             controller          = nullptr;
    /*-----------------------------------------------------*\
    | What the plugin sent last (the pad cannot be read     |
    | back), stored in the OpenRGB configuration directory  |
    \*-----------------------------------------------------*/
    kmp6::Settings                      applied;
    std::string                         stored_path;
    /*-----------------------------------------------------*\
    | Settings waiting to become stable                     |
    \*-----------------------------------------------------*/
    bool                                waiting             = false;
    bool                                waiting_explicit    = false;
    kmp6::Settings                      pending;
    std::chrono::steady_clock::time_point pending_since;
    std::chrono::steady_clock::time_point last_presence_check;
    std::string                         last_logged;

    /*-----------------------------------------------------*\
    | Status page (GUI thread reads, worker writes)         |
    \*-----------------------------------------------------*/
    std::mutex                          status_mutex;
    std::string                         status_text;
    QWidget*                            widget              = nullptr;
    QLabel*                             status_label        = nullptr;
    QTimer*                             status_timer        = nullptr;
};
