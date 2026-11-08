/*---------------------------------------------------------*\
| KMP6Protocol.h                                            |
|                                                           |
|   Lighting settings of the Aukey KM-P6 and the reports    |
|   that carry them. Port of kmp6-core/src/protocol.rs and  |
|   device.rs; evidence for every byte: aukey-km-p6-rgb-    |
|   control docs/PROTOCOL.md                                |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <array>
#include <chrono>
#include <string>
#include <vector>

#include "UsbTransport.h"

namespace kmp6
{

typedef std::array<unsigned char, 3> Rgb;

constexpr unsigned int      SLOTS           = 7;
constexpr unsigned char     CMD_EFFECT      = 0x08;
constexpr unsigned char     CMD_COLORS[3]   = { 0x09, 0x0A, 0x0B };
/*---------------------------------------------------------*\
| Colour byte "cycle through all slots" instead of slot<<4  |
\*---------------------------------------------------------*/
constexpr unsigned char     CYCLE           = 0x70;
/*---------------------------------------------------------*\
| Device values of the G-aim levels                         |
\*---------------------------------------------------------*/
constexpr unsigned char     BRIGHTNESS_CODES[10]    = { 0, 3, 7, 10, 14, 17, 21, 24, 28, 32 };
constexpr unsigned char     SPEED_CODES[5]          = { 0, 1, 3, 4, 5 };
constexpr unsigned char     MAX_BRIGHTNESS          = 9;
constexpr unsigned char     MAX_SPEED               = 4;

extern const Rgb            FACTORY_COLORS[SLOTS];

/*---------------------------------------------------------*\
| Effects; the value is the device code (off is sent as     |
| static with brightness 0)                                 |
\*---------------------------------------------------------*/
enum class Mode
{
    Static  = 0,
    Breathe = 1,
    Neon    = 2,
    Wave    = 3,
    Off     = 4,
};

/*---------------------------------------------------------*\
| Wave direction; the value is the device code              |
\*---------------------------------------------------------*/
enum class Direction
{
    Right   = 0,
    Left    = 1,
};

const char* ModeId(Mode mode);
bool        ModeFromId(const std::string& id, Mode& mode);

/*---------------------------------------------------------*\
| Everything the pad shows. Levels are those of G-aim       |
| (brightness 0-9, speed 0-4), slots count from 0. The      |
| defaults are the factory state of G-aim                   |
\*---------------------------------------------------------*/
struct Settings
{
    Mode            mode        = Mode::Static;
    unsigned char   brightness  = MAX_BRIGHTNESS;
    unsigned char   speed       = MAX_SPEED;
    Direction       direction   = Direction::Left;
    bool            cycle       = true;
    unsigned char   slot        = 0;
    Rgb             colors[SLOTS];

    Settings();

    bool            Valid() const;
    unsigned char   ColorByte() const;
    /*-----------------------------------------------------*\
    | Reports in the order G-aim sends them; static and     |
    | breathe twice. Empty if the settings are invalid      |
    \*-----------------------------------------------------*/
    std::vector<Report> Packets() const;

    bool            operator==(const Settings& other) const;
    bool            operator!=(const Settings& other) const { return !(*this == other); }
};

enum class Status
{
    Ok,
    Locked,         /* another program used the pad for 5 s     */
    SendFailed,     /* SET_REPORT failed after all attempts     */
    Invalid,        /* settings out of range                    */
};

const char* StatusText(Status status);

/*---------------------------------------------------------*\
| The pad behind a transport (device.rs)                    |
\*---------------------------------------------------------*/
class Pad
{
public:
    explicit Pad(Transport& transport);

    /*-----------------------------------------------------*\
    | 10 ms between reports, 4 attempts per report with     |
    | 40/80/120 ms pauses; tests set the delays to zero     |
    \*-----------------------------------------------------*/
    std::chrono::milliseconds   gap;
    std::chrono::milliseconds   retry_delay;
    unsigned int                attempts;

    Status          Apply(const Settings& settings);
    Status          SendAll(const std::vector<Report>& packets);

private:
    TransportError  SendOne(const Report& packet);

    Transport&      transport;
};

}
