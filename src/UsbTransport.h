/*---------------------------------------------------------*\
| UsbTransport.h                                            |
|                                                           |
|   Feature report transport for the Aukey KM-P6 RGB mouse  |
|   pad (USB 04D9:A1FA, Holtek, interface 2)                |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace kmp6
{

constexpr unsigned short    VID                 = 0x04D9;
constexpr unsigned short    PID                 = 0xA1FA;
/*---------------------------------------------------------*\
| Vendor interface (usage page FF00) that takes the reports |
\*---------------------------------------------------------*/
constexpr int               INTERFACE           = 2;
/*---------------------------------------------------------*\
| Data bytes of a feature report; report ID 0 not included  |
\*---------------------------------------------------------*/
constexpr std::size_t       REPORT_LEN          = 8;
/*---------------------------------------------------------*\
| Longest wait for another program (e.g. the KM-P6 RGB      |
| Tool) that is talking to the pad                          |
\*---------------------------------------------------------*/
constexpr unsigned int      LOCK_TIMEOUT_MS     = 5000;

typedef std::array<unsigned char, REPORT_LEN> Report;

enum class TransportError
{
    None,
    Busy,           /* another program holds lock or interface  */
    Failed,         /* the transfer failed (errno in the text)  */
};

/*---------------------------------------------------------*\
| The pad only listens: an 8-byte feature report without    |
| report ID to interface 2, never an answer. Every command  |
| runs under a cross-process lock shared with kmp6_rgbtool. |
\*---------------------------------------------------------*/
class Transport
{
public:
    virtual                 ~Transport() {}

    virtual TransportError  SetFeature(const Report& data)  = 0;
    virtual TransportError  Lock()                          { return TransportError::None; }
    virtual void            Unlock()                        {}
    virtual std::string     Path() const                    { return ""; }
    virtual std::string     LastError() const               { return ""; }
};

/*---------------------------------------------------------*\
| Platform functions (UsbTransport_Linux.cpp)               |
\*---------------------------------------------------------*/
std::vector<std::string>    FindDevicePaths();
std::unique_ptr<Transport>  OpenTransport(const std::string& path, std::string& error);

}
