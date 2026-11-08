/*---------------------------------------------------------*\
| KMP6Plugin.cpp                                            |
|                                                           |
|   OpenRGB 1.1 plugin for the Aukey KM-P6 RGB mouse pad    |
|   (USB 04D9:A1FA, Holtek), Linux only.                    |
|                                                           |
|   OpenRGB's own "Holtek Mousemat" controller finds the    |
|   pad through hidapi on interface 2. On Linux that        |
|   interface has no hidraw node (it has no endpoint, so    |
|   usbhid refuses it), so the plugin sends the reports as  |
|   control transfers through usbfs instead.                |
|                                                           |
|   The device has the effects of the pad in the order its  |
|   button steps through them (static, wave, breathing,     |
|   neon) plus off, with the brightness (10 levels), speed  |
|   (5 levels) and wave direction of G-aim, and one LED     |
|   (edge and logo always share the colour). Static and     |
|   breathing always show that colour; the pad's "cycle     |
|   through all colours" is left out on purpose.            |
|   The pad keeps its setting without power, so it          |
|   presumably writes every setting to its flash: there is  |
|   no direct mode, and a setting is only sent once it      |
|   stayed unchanged for half a second.                     |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "KMP6Plugin.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

#include <QCoreApplication>
#include <QEventLoop>
#include <QVBoxLayout>

#include "LogManager.h"
#include "filesystem.h"

#define KMP6_PLUGIN_NAME        "KMP6"
#define KMP6_PLUGIN_VERSION     "1.0.0"
#define KMP6_PLUGIN_URL         "https://github.com/Tom-8192/OpenRGB-KMP6-Plugin"
#define KMP6_DEVICE_NAME        "Aukey KM-P6"

#define KMP6_LOG(level, ...)    do { if(api) api->LogEntry(__FILE__, __LINE__, level, "[" KMP6_PLUGIN_NAME "] " __VA_ARGS__); } while(0)

/*---------------------------------------------------------*\
| Worker timing                                             |
\*---------------------------------------------------------*/
static const std::chrono::milliseconds  POLL_IDLE(2000);
static const std::chrono::milliseconds  POLL_WAITING(100);
/*---------------------------------------------------------*\
| Settings must stay unchanged this long before they are    |
| sent (colour picker and slider dragging)                  |
\*---------------------------------------------------------*/
static const std::chrono::milliseconds  SETTLE_TIME(500);

/*---------------------------------------------------------*\
| The controller's modes; index = OpenRGB mode index        |
\*---------------------------------------------------------*/
struct ModeInfo
{
    kmp6::Mode      mode;
    const char*     name;
    unsigned int    flags;
};

static const ModeInfo MODES[] =
{
    { kmp6::Mode::Static,  "Static",    MODE_FLAG_HAS_PER_LED_COLOR | MODE_FLAG_HAS_BRIGHTNESS },
    { kmp6::Mode::Wave,    "Wave",      MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_HAS_DIRECTION_LR },
    { kmp6::Mode::Breathe, "Breathing", MODE_FLAG_HAS_PER_LED_COLOR | MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_BRIGHTNESS },
    { kmp6::Mode::Neon,    "Neon",      MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_BRIGHTNESS },
    { kmp6::Mode::Off,     "Off",       0 },
};

static const unsigned int MODE_COUNT    = sizeof(MODES) / sizeof(MODES[0]);
/*---------------------------------------------------------*\
| Where a value is taken from when the active mode has none |
\*---------------------------------------------------------*/
static const unsigned int MODE_STATIC   = 0;
static const unsigned int MODE_WAVE     = 1;
static const unsigned int MODE_BREATHE  = 2;

static unsigned int ModeIndex(kmp6::Mode mode)
{
    for(unsigned int idx = 0; idx < MODE_COUNT; idx++)
    {
        if(MODES[idx].mode == mode)
        {
            return idx;
        }
    }

    return MODE_STATIC;
}

static unsigned int OpenRGBDirection(kmp6::Direction direction)
{
    return direction == kmp6::Direction::Left ? MODE_DIRECTION_LEFT : MODE_DIRECTION_RIGHT;
}

static kmp6::Direction PadDirection(unsigned int direction)
{
    return direction == MODE_DIRECTION_RIGHT ? kmp6::Direction::Right : kmp6::Direction::Left;
}

static std::string HexColor(const kmp6::Rgb& rgb)
{
    char text[8];
    snprintf(text, sizeof(text), "#%02X%02X%02X", rgb[0], rgb[1], rgb[2]);
    return text;
}

static bool ParseHexColor(const std::string& text, kmp6::Rgb& rgb)
{
    unsigned int r, g, b;

    if(text.size() != 7 || text[0] != '#' || sscanf(text.c_str() + 1, "%02x%02x%02x", &r, &g, &b) != 3)
    {
        return false;
    }

    rgb = {{ (unsigned char)r, (unsigned char)g, (unsigned char)b }};
    return true;
}

/*---------------------------------------------------------*\
| JSON of a setting: stored file and OpenRGB profiles       |
\*---------------------------------------------------------*/
static nlohmann::json SettingsToJson(const kmp6::Settings& s)
{
    nlohmann::json data;

    data["mode"]        = kmp6::ModeId(s.mode);
    data["brightness"]  = s.brightness;
    data["speed"]       = s.speed;
    data["direction"]   = s.direction == kmp6::Direction::Left ? "left" : "right";
    data["color"]       = HexColor(s.colors[0]);

    return data;
}

static bool SettingsFromJson(const nlohmann::json& data, kmp6::Settings& s)
{
    if(!data.is_object() || !data.contains("mode") || !data["mode"].is_string())
    {
        return false;
    }

    kmp6::Settings result;
    result.cycle = false;

    if(!kmp6::ModeFromId(data["mode"].get<std::string>(), result.mode))
    {
        return false;
    }

    if(data.contains("brightness") && data["brightness"].is_number_unsigned())
    {
        result.brightness = (unsigned char)std::min<unsigned int>(data["brightness"].get<unsigned int>(), kmp6::MAX_BRIGHTNESS);
    }

    if(data.contains("speed") && data["speed"].is_number_unsigned())
    {
        result.speed = (unsigned char)std::min<unsigned int>(data["speed"].get<unsigned int>(), kmp6::MAX_SPEED);
    }

    if(data.contains("direction") && data["direction"].is_string())
    {
        result.direction = data["direction"] == "right" ? kmp6::Direction::Right : kmp6::Direction::Left;
    }

    if(data.contains("color") && data["color"].is_string())
    {
        ParseHexColor(data["color"].get<std::string>(), result.colors[0]);
    }

    s = result;
    return true;
}

static std::string Describe(const kmp6::Settings& s)
{
    char text[160];

    snprintf(text, sizeof(text), "%s, brightness %u/9, speed %u/4, %s, colour %s",
             MODES[ModeIndex(s.mode)].name, s.brightness, s.speed,
             s.direction == kmp6::Direction::Left ? "left" : "right",
             HexColor(s.colors[0]).c_str());

    return text;
}

KMP6Plugin::~KMP6Plugin()
{
    if(worker.joinable())
    {
        Unload();
    }
}

OpenRGBPluginInfo KMP6Plugin::GetPluginInfo()
{
    OpenRGBPluginInfo info;

    info.Name               = KMP6_PLUGIN_NAME;
    info.Description        = "Lighting of the Aukey KM-P6 RGB mouse pad (USB 04D9:A1FA) on Linux";
    info.Version            = KMP6_PLUGIN_VERSION;
    info.Commit             = "";
    info.URL                = KMP6_PLUGIN_URL;
    info.Location           = OPENRGB_PLUGIN_LOCATION_SETTINGS;
    info.Label              = KMP6_PLUGIN_NAME;
    info.ProtocolVersion    = OPENRGB_PLUGIN_API_VERSION;

    return info;
}

unsigned int KMP6Plugin::GetPluginAPIVersion()
{
    return OPENRGB_PLUGIN_API_VERSION;
}

void KMP6Plugin::Load(OpenRGBPluginAPIInterface* plugin_api_ptr)
{
    api             = plugin_api_ptr;
    stopping        = false;
    wake_pending    = false;
    worker_done     = false;

    stored_path     = (api->GetConfigurationDirectory() / "plugins" / "settings" / "KMP6.json").string();
    /*-----------------------------------------------------*\
    | Factory state of G-aim, but showing slot 1 instead of |
    | cycling through the colours                           |
    \*-----------------------------------------------------*/
    applied         = kmp6::Settings();
    applied.cycle   = false;
    LoadStored();

    SetStatus("Looking for the mouse pad...");

    worker = std::thread(&KMP6Plugin::WorkerThread, this);
}

QWidget* KMP6Plugin::GetWidget()
{
    if(widget == nullptr)
    {
        widget                  = new QWidget();
        QVBoxLayout* layout     = new QVBoxLayout(widget);
        status_label            = new QLabel(widget);
        QLabel* note            = new QLabel(widget);

        status_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        status_label->setWordWrap(true);
        note->setWordWrap(true);
        note->setText("Under Devices the pad has the effects Static, Wave, Breathing and Neon (the order of the button "
                      "on the pad) and Off; Static and Breathing show the colour of the LED \"Mouse pad\".\n"
                      "A setting is sent once it stayed unchanged for half a second. The pad cannot be read back: "
                      "after the button on the pad or another program changed it, apply the mode again.\n"
                      "No direct mode on purpose: the pad keeps every setting without power, so it presumably "
                      "writes each one to its flash.");

        layout->addWidget(status_label);
        layout->addWidget(note);
        layout->addStretch();

        status_timer = new QTimer();
        QObject::connect(status_timer, &QTimer::timeout, status_label, [this]()
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            status_label->setText(QString::fromStdString(status_text));
        });
        status_timer->start(500);

        std::lock_guard<std::mutex> lock(status_mutex);
        status_label->setText(QString::fromStdString(status_text));
    }

    return widget;
}

QMenu* KMP6Plugin::GetTrayMenu()
{
    return nullptr;
}

void KMP6Plugin::Unload()
{
    /*-----------------------------------------------------*\
    | The page outlives the plugin (OpenRGB only removes    |
    | the tab), so stop the timer that runs our code        |
    \*-----------------------------------------------------*/
    if(status_timer != nullptr)
    {
        status_timer->stop();
        delete status_timer;
        status_timer = nullptr;
    }

    if(!worker.joinable())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(wake_mutex);
        stopping = true;
    }
    wake.notify_one();

    /*-----------------------------------------------------*\
    | The worker unregisters the controller on its way out. |
    | OpenRGB passes device list changes to other plugins,  |
    | some of which block on the GUI thread, so keep events |
    | flowing while waiting instead of a bare join()        |
    \*-----------------------------------------------------*/
    while(!worker_done)
    {
        if(QCoreApplication::instance() != nullptr)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    worker.join();
}

void KMP6Plugin::OnProfileAboutToLoad()
{
}

void KMP6Plugin::OnProfileLoad(nlohmann::json profile_data)
{
    kmp6::Settings check;

    if(!SettingsFromJson(profile_data, check))
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(request_mutex);
        profile_request     = profile_data;
        has_profile_request = true;
    }

    Wake();
}

nlohmann::json KMP6Plugin::OnProfileSave()
{
    std::lock_guard<std::mutex> lock(controller_mutex);

    if(controller == nullptr)
    {
        return nlohmann::json();
    }

    return SettingsToJson(SettingsFromController());
}

unsigned char* KMP6Plugin::OnSDKCommand(unsigned int /*pkt_id*/, unsigned char* /*pkt_data*/, unsigned int* pkt_size)
{
    if(pkt_size != nullptr)
    {
        *pkt_size = 0;
    }

    return nullptr;
}

void KMP6Plugin::ProfileManagerUpdated(unsigned int /*update_reason*/)
{
}

void KMP6Plugin::ResourceManagerUpdated(unsigned int /*update_reason*/)
{
}

void KMP6Plugin::SettingsManagerUpdated(unsigned int /*update_reason*/)
{
}

/*---------------------------------------------------------*\
| Controller callbacks                                      |
\*---------------------------------------------------------*/
void KMP6Plugin::CallbackUpdateMode(void* object_ptr)
{
    KMP6Plugin* plugin = static_cast<KMP6Plugin*>(object_ptr);
    plugin->mode_dirty = true;
    plugin->Wake();
}

void KMP6Plugin::CallbackUpdateZoneMode(void* object_ptr, int /*zone*/)
{
    CallbackUpdateMode(object_ptr);
}

void KMP6Plugin::CallbackUpdateLEDs(void* object_ptr)
{
    KMP6Plugin* plugin = static_cast<KMP6Plugin*>(object_ptr);
    plugin->leds_dirty = true;
    plugin->Wake();
}

void KMP6Plugin::CallbackUpdateZoneLEDs(void* object_ptr, int /*zone*/)
{
    CallbackUpdateLEDs(object_ptr);
}

void KMP6Plugin::CallbackUpdateSingleLED(void* object_ptr, int /*led*/)
{
    CallbackUpdateLEDs(object_ptr);
}

void KMP6Plugin::Wake()
{
    {
        std::lock_guard<std::mutex> lock(wake_mutex);
        wake_pending = true;
    }
    wake.notify_one();
}

/*---------------------------------------------------------*\
| Controller <-> settings                                   |
\*---------------------------------------------------------*/
kmp6::Settings KMP6Plugin::SettingsFromController()
{
    kmp6::Settings  s;
    int             active  = controller->GetActiveMode();
    unsigned int    idx     = (active >= 0 && active < (int)MODE_COUNT) ? (unsigned int)active : MODE_STATIC;
    unsigned int    flags   = MODES[idx].flags;

    /*-----------------------------------------------------*\
    | Values the active mode does not have come from the    |
    | mode that has them, so they survive a profile         |
    \*-----------------------------------------------------*/
    unsigned int bright_mode    = (flags & MODE_FLAG_HAS_BRIGHTNESS)    ? idx : MODE_STATIC;
    unsigned int speed_mode     = (flags & MODE_FLAG_HAS_SPEED)         ? idx : MODE_BREATHE;

    s.mode          = MODES[idx].mode;
    s.brightness    = (unsigned char)std::min<unsigned int>(controller->GetModeBrightness(bright_mode), kmp6::MAX_BRIGHTNESS);
    s.speed         = (unsigned char)std::min<unsigned int>(controller->GetModeSpeed(speed_mode), kmp6::MAX_SPEED);
    s.direction     = PadDirection(controller->GetModeDirection(MODE_WAVE));
    s.cycle         = false;
    s.slot          = 0;

    RGBColor color  = controller->GetColor(0);
    s.colors[0]     = {{ (unsigned char)RGBGetRValue(color), (unsigned char)RGBGetGValue(color), (unsigned char)RGBGetBValue(color) }};

    return s;
}

void KMP6Plugin::SettingsToController(const kmp6::Settings& s)
{
    for(unsigned int idx = 0; idx < MODE_COUNT; idx++)
    {
        unsigned int flags = MODES[idx].flags;

        if(flags & MODE_FLAG_HAS_BRIGHTNESS)
        {
            controller->SetModeBrightness(idx, s.brightness);
        }

        if(flags & MODE_FLAG_HAS_SPEED)
        {
            controller->SetModeSpeed(idx, s.speed);
        }

        if(flags & MODE_FLAG_HAS_DIRECTION_LR)
        {
            controller->SetModeDirection(idx, OpenRGBDirection(s.direction));
        }

        if(flags & MODE_FLAG_HAS_PER_LED_COLOR)
        {
            controller->SetModeColorMode(idx, MODE_COLORS_PER_LED);
        }
    }

    controller->SetColor(0, ToRGBColor(s.colors[0][0], s.colors[0][1], s.colors[0][2]));
}

/*---------------------------------------------------------*\
| Stored setting                                            |
\*---------------------------------------------------------*/
void KMP6Plugin::LoadStored()
{
    std::ifstream file(stored_path);

    if(!file)
    {
        return;
    }

    nlohmann::json data = nlohmann::json::parse(file, nullptr, false);

    if(!SettingsFromJson(data, applied))
    {
        KMP6_LOG(LL_WARNING, "ignoring damaged %s", stored_path.c_str());
    }
}

void KMP6Plugin::SaveStored()
{
    std::error_code error;
    filesystem::create_directories(filesystem::path(stored_path).parent_path(), error);

    std::ofstream file(stored_path);

    if(!file)
    {
        LogOnce(LL_WARNING, "cannot write " + stored_path);
        return;
    }

    file << SettingsToJson(applied).dump(4) << "\n";
}

/*---------------------------------------------------------*\
| Worker                                                    |
\*---------------------------------------------------------*/
void KMP6Plugin::WorkerThread()
{
    while(true)
    {
        if(controller == nullptr)
        {
            Connect();
        }

        if(controller != nullptr)
        {
            Cycle();
        }

        std::unique_lock<std::mutex> lock(wake_mutex);
        wake.wait_for(lock, (controller != nullptr && waiting) ? POLL_WAITING : POLL_IDLE, [this]() { return stopping || wake_pending; });

        if(stopping)
        {
            break;
        }

        wake_pending = false;
    }

    Disconnect();
    worker_done = true;
}

void KMP6Plugin::Connect()
{
    std::vector<std::string> paths = kmp6::FindDevicePaths();

    if(paths.empty())
    {
        SetStatus("Mouse pad not found (USB 04D9:A1FA). Waiting for it to be plugged in.");
        LogOnce(LL_INFO, "mouse pad not found, waiting for it");
        return;
    }

    std::string                         error;
    std::unique_ptr<kmp6::Transport>    candidate = kmp6::OpenTransport(paths[0], error);

    if(!candidate)
    {
        SetStatus("Cannot open the mouse pad: " + error);
        LogOnce(LL_WARNING, error);
        return;
    }

    RGBController_Setup setup = {};
    setup.name                  = KMP6_DEVICE_NAME;
    setup.vendor                = "Aukey";
    setup.description           = "Aukey KM-P6 RGB mouse pad (Holtek 04D9:A1FA), via the " KMP6_PLUGIN_NAME " plugin";
    setup.version               = KMP6_PLUGIN_VERSION;
    /*-----------------------------------------------------*\
    | Not the usbfs path: it changes with every replug, and |
    | OpenRGB profiles find devices by their location       |
    \*-----------------------------------------------------*/
    setup.location              = "USB: 04D9:A1FA interface 2";
    setup.type                  = DEVICE_TYPE_MOUSEMAT;
    setup.flags                 = CONTROLLER_FLAG_VIRTUAL;
    setup.active_mode           = (int)ModeIndex(applied.mode);

    for(unsigned int idx = 0; idx < MODE_COUNT; idx++)
    {
        mode m;
        m.name          = MODES[idx].name;
        m.value         = (int)MODES[idx].mode;
        m.flags         = MODES[idx].flags | MODE_FLAG_AUTOMATIC_SAVE;
        m.color_mode    = MODE_COLORS_NONE;

        if(m.flags & MODE_FLAG_HAS_BRIGHTNESS)
        {
            m.brightness_min    = 0;
            m.brightness_max    = kmp6::MAX_BRIGHTNESS;
            m.brightness        = applied.brightness;
        }

        if(m.flags & MODE_FLAG_HAS_SPEED)
        {
            m.speed_min         = 0;
            m.speed_max         = kmp6::MAX_SPEED;
            m.speed             = applied.speed;
        }

        if(m.flags & MODE_FLAG_HAS_DIRECTION_LR)
        {
            m.direction         = OpenRGBDirection(applied.direction);
        }

        if(m.flags & MODE_FLAG_HAS_PER_LED_COLOR)
        {
            m.color_mode        = MODE_COLORS_PER_LED;
        }

        setup.modes.push_back(m);
    }

    /*-----------------------------------------------------*\
    | One LED: edge and logo always show the same colour    |
    \*-----------------------------------------------------*/
    zone pad_zone;
    pad_zone.name       = "Mouse pad";
    pad_zone.type       = ZONE_TYPE_SINGLE;
    pad_zone.leds_count = 1;
    pad_zone.leds_min   = 1;
    pad_zone.leds_max   = 1;
    setup.zones.push_back(pad_zone);

    led pad_led;
    pad_led.name        = "Mouse pad";
    pad_led.value       = 0;
    setup.leds.push_back(pad_led);

    setup.object_ptr            = this;
    setup.DeviceUpdateMode      = &KMP6Plugin::CallbackUpdateMode;
    setup.DeviceSaveMode        = &KMP6Plugin::CallbackUpdateMode;
    setup.DeviceUpdateZoneMode  = &KMP6Plugin::CallbackUpdateZoneMode;
    setup.DeviceUpdateLEDs      = &KMP6Plugin::CallbackUpdateLEDs;
    setup.DeviceUpdateZoneLEDs  = &KMP6Plugin::CallbackUpdateZoneLEDs;
    setup.DeviceUpdateSingleLED = &KMP6Plugin::CallbackUpdateSingleLED;

    transport           = std::move(candidate);
    pad.reset(new kmp6::Pad(*transport));
    waiting             = false;
    waiting_explicit    = false;
    mode_dirty          = false;
    leds_dirty          = false;
    last_presence_check = std::chrono::steady_clock::now();

    RGBControllerInterface* created = api->CreateVirtualRGBController(&setup);
    created->SetColor(0, ToRGBColor(applied.colors[0][0], applied.colors[0][1], applied.colors[0][2]));

    {
        std::lock_guard<std::mutex> lock(controller_mutex);
        controller = created;
    }

    api->RegisterVirtualRGBController(controller);

    std::string text = "Connected (" + transport->Path() + "), last sent: " + Describe(applied);
    SetStatus(text);
    last_logged.clear();
    KMP6_LOG(LL_INFO, "%s", text.c_str());
}

void KMP6Plugin::Disconnect()
{
    RGBControllerInterface* removed = nullptr;

    {
        std::lock_guard<std::mutex> lock(controller_mutex);
        removed     = controller;
        controller  = nullptr;
    }

    if(removed != nullptr)
    {
        api->UnregisterVirtualRGBController(removed);
        api->DeleteVirtualRGBController(removed);
    }

    pad.reset();
    transport.reset();
    waiting             = false;
    waiting_explicit    = false;
}

void KMP6Plugin::Cycle()
{
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

    /*-----------------------------------------------------*\
    | The pad never answers, so unplugging only shows in    |
    | sysfs                                                 |
    \*-----------------------------------------------------*/
    if(now - last_presence_check >= POLL_IDLE)
    {
        last_presence_check = now;
        std::vector<std::string> paths = kmp6::FindDevicePaths();

        if(std::find(paths.begin(), paths.end(), transport->Path()) == paths.end())
        {
            KMP6_LOG(LL_INFO, "mouse pad disconnected");
            SetStatus("Mouse pad disconnected. Waiting for it to be plugged in.");
            Disconnect();
            return;
        }
    }

    ApplyProfileRequest();

    bool explicit_update    = mode_dirty.exchange(false);
    bool led_update         = leds_dirty.exchange(false);

    if(explicit_update || led_update)
    {
        kmp6::Settings current;

        {
            std::lock_guard<std::mutex> lock(controller_mutex);
            current = SettingsFromController();
        }

        if(!waiting || current != pending)
        {
            pending         = current;
            pending_since   = now;
        }

        waiting             = true;
        waiting_explicit    = waiting_explicit || explicit_update;
    }

    if(!waiting || now - pending_since < SETTLE_TIME)
    {
        return;
    }

    waiting = false;

    bool send = waiting_explicit || pending != applied;
    waiting_explicit = false;

    if(!send)
    {
        return;
    }

    kmp6::Status status = pad->Apply(pending);

    if(status == kmp6::Status::Ok)
    {
        applied = pending;
        SaveStored();

        std::string text = "Connected (" + transport->Path() + "), sent: " + Describe(applied);
        SetStatus(text);
        last_logged.clear();
        KMP6_LOG(LL_INFO, "sent %s", Describe(applied).c_str());
        return;
    }

    std::string text = std::string(kmp6::StatusText(status));

    if(!transport->LastError().empty())
    {
        text += " (" + transport->LastError() + ")";
    }

    SetStatus("Sending failed: " + text);
    LogOnce(LL_WARNING, "sending " + Describe(pending) + " failed: " + text);

    /*-----------------------------------------------------*\
    | Another program had the pad: try once more later      |
    \*-----------------------------------------------------*/
    if(status == kmp6::Status::Locked)
    {
        waiting             = true;
        waiting_explicit    = true;
        pending_since       = now;
    }
}

bool KMP6Plugin::ApplyProfileRequest()
{
    nlohmann::json data;

    {
        std::lock_guard<std::mutex> lock(request_mutex);

        if(!has_profile_request)
        {
            return false;
        }

        data                = profile_request;
        has_profile_request = false;
    }

    kmp6::Settings s;

    if(!SettingsFromJson(data, s))
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(controller_mutex);
        SettingsToController(s);
    }

    /*-----------------------------------------------------*\
    | SetActiveMode calls UpdateMode, which marks the mode  |
    | dirty; the setting is sent once it settled            |
    \*-----------------------------------------------------*/
    controller->SetActiveMode((int)ModeIndex(s.mode));
    controller->SignalUpdate(RGBCONTROLLER_UPDATE_REASON_UPDATEMODE);
    mode_dirty = true;

    KMP6_LOG(LL_INFO, "OpenRGB profile loaded: %s", Describe(s).c_str());
    return true;
}

void KMP6Plugin::SetStatus(const std::string& text)
{
    std::lock_guard<std::mutex> lock(status_mutex);
    status_text = text;
}

void KMP6Plugin::LogOnce(unsigned int level, const std::string& text)
{
    if(text != last_logged)
    {
        last_logged = text;
        KMP6_LOG(level, "%s", text.c_str());
    }
}
