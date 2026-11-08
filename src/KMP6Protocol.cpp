/*---------------------------------------------------------*\
| KMP6Protocol.cpp                                          |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "KMP6Protocol.h"

#include <thread>

namespace kmp6
{

/*---------------------------------------------------------*\
| Factory colours of G-aim (setting/pD.bin): red, green,    |
| blue, yellow, cyan, magenta, white                        |
\*---------------------------------------------------------*/
const Rgb FACTORY_COLORS[SLOTS] =
{
    {{ 255,   0,   0 }},
    {{   0, 255,   0 }},
    {{   0,   0, 255 }},
    {{ 255, 255,   0 }},
    {{   0, 255, 255 }},
    {{ 255,   0, 255 }},
    {{ 255, 255, 255 }},
};

const char* ModeId(Mode mode)
{
    switch(mode)
    {
        case Mode::Static:  return "static";
        case Mode::Breathe: return "breathe";
        case Mode::Neon:    return "neon";
        case Mode::Wave:    return "wave";
        case Mode::Off:     return "off";
    }

    return "static";
}

bool ModeFromId(const std::string& id, Mode& mode)
{
    const Mode all[] = { Mode::Static, Mode::Breathe, Mode::Neon, Mode::Wave, Mode::Off };

    for(Mode m : all)
    {
        if(id == ModeId(m))
        {
            mode = m;
            return true;
        }
    }

    return false;
}

Settings::Settings()
{
    for(unsigned int idx = 0; idx < SLOTS; idx++)
    {
        colors[idx] = FACTORY_COLORS[idx];
    }
}

bool Settings::Valid() const
{
    return brightness <= MAX_BRIGHTNESS && speed <= MAX_SPEED && slot < SLOTS;
}

unsigned char Settings::ColorByte() const
{
    return cycle ? CYCLE : (unsigned char)(slot << 4);
}

std::vector<Report> Settings::Packets() const
{
    std::vector<Report> packets;

    if(!Valid())
    {
        return packets;
    }

    unsigned char bright    = BRIGHTNESS_CODES[brightness];
    unsigned char spd       = SPEED_CODES[speed];
    unsigned char dir       = (unsigned char)direction;
    const Rgb&    first     = colors[0];

    switch(mode)
    {
        case Mode::Static:
        case Mode::Breathe:
        {
            /*---------------------------------------------*\
            | Slots 2-7, then the effect with slot 1; all   |
            | twice like G-aim ("only changes the next      |
            | time")                                        |
            \*---------------------------------------------*/
            bool is_static = mode == Mode::Static;

            for(int round = 0; round < 2; round++)
            {
                for(unsigned int idx = 0; idx < 3; idx++)
                {
                    const Rgb& a = colors[1 + 2 * idx];
                    const Rgb& c = colors[2 + 2 * idx];
                    packets.push_back({{ CMD_COLORS[idx], a[0], a[1], a[2], c[0], c[1], c[2], 0 }});
                }

                packets.push_back({{ CMD_EFFECT, (unsigned char)(is_static ? 0 : 1), bright, (unsigned char)(is_static ? 0 : spd),
                                     ColorByte(), first[0], first[1], first[2] }});
            }
            break;
        }

        case Mode::Neon:
            packets.push_back({{ CMD_EFFECT, 2, bright, spd, 0, 0, 0, 0 }});
            break;

        case Mode::Wave:
            packets.push_back({{ CMD_EFFECT, 3, bright, spd, dir, 0, 0, 0 }});
            break;

        case Mode::Off:
            packets.push_back({{ CMD_EFFECT, 0, 0, spd, dir, 0, 0, 0 }});
            break;
    }

    return packets;
}

bool Settings::operator==(const Settings& other) const
{
    if(mode != other.mode || brightness != other.brightness || speed != other.speed || direction != other.direction
    || cycle != other.cycle || slot != other.slot)
    {
        return false;
    }

    for(unsigned int idx = 0; idx < SLOTS; idx++)
    {
        if(colors[idx] != other.colors[idx])
        {
            return false;
        }
    }

    return true;
}

const char* StatusText(Status status)
{
    switch(status)
    {
        case Status::Ok:            return "ok";
        case Status::Locked:        return "the pad is being used by another program";
        case Status::SendFailed:    return "the pad refused the report";
        case Status::Invalid:       return "setting out of range";
    }

    return "unknown";
}

Pad::Pad(Transport& transport)
    : gap(10), retry_delay(40), attempts(4), transport(transport)
{
}

Status Pad::Apply(const Settings& settings)
{
    std::vector<Report> packets = settings.Packets();

    if(packets.empty())
    {
        return Status::Invalid;
    }

    return SendAll(packets);
}

Status Pad::SendAll(const std::vector<Report>& packets)
{
    TransportError lock = transport.Lock();

    if(lock != TransportError::None)
    {
        return lock == TransportError::Busy ? Status::Locked : Status::SendFailed;
    }

    Status result = Status::Ok;

    for(std::size_t idx = 0; idx < packets.size(); idx++)
    {
        if(idx > 0 && gap.count() > 0)
        {
            std::this_thread::sleep_for(gap);
        }

        TransportError sent = SendOne(packets[idx]);

        if(sent != TransportError::None)
        {
            result = sent == TransportError::Busy ? Status::Locked : Status::SendFailed;
            break;
        }
    }

    transport.Unlock();
    return result;
}

TransportError Pad::SendOne(const Report& packet)
{
    /*-----------------------------------------------------*\
    | The pad now and then refuses a report; the same one a |
    | moment later goes through                             |
    \*-----------------------------------------------------*/
    for(unsigned int attempt = 1; ; attempt++)
    {
        TransportError result = transport.SetFeature(packet);

        if(result == TransportError::None || attempt >= attempts)
        {
            return result;
        }

        if(retry_delay.count() > 0)
        {
            std::this_thread::sleep_for(retry_delay * attempt);
        }
    }
}

}
