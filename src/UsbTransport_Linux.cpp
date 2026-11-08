/*---------------------------------------------------------*\
| UsbTransport_Linux.cpp                                    |
|                                                           |
|   Linux: SET_REPORT as a control transfer through usbfs   |
|   (/dev/bus/usb/BBB/DDD). Interface 2 of the pad has no   |
|   endpoint, so usbhid refuses it and there is no hidraw   |
|   node for it; the keyboard interface 0 keeps its driver. |
|   Same as kmp6-core/src/transport/linux_usbfs.rs: flock   |
|   on the usbfs node as the lock between processes, and    |
|   interface 2 is claimed only while a command runs.       |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#ifdef __linux__

#include "UsbTransport.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace kmp6
{

namespace
{

/*---------------------------------------------------------*\
| Host to device, class request, recipient interface;       |
| HID SET_REPORT; feature report (3) with report ID 0       |
\*---------------------------------------------------------*/
constexpr unsigned char     REQUEST_TYPE        = 0x21;
constexpr unsigned char     HID_SET_REPORT      = 0x09;
constexpr unsigned short    FEATURE_REPORT_ID_0 = 0x0300;
constexpr unsigned int      TRANSFER_TIMEOUT_MS = 1000;

class UsbfsTransport : public Transport
{
public:
    UsbfsTransport(int fd, const std::string& path)
        : fd(fd), path(path), locked(false), claimed(false)
    {
    }

    ~UsbfsTransport() override
    {
        Unlock();
        close(fd);
    }

    TransportError SetFeature(const Report& data) override
    {
        /*-------------------------------------------------*\
        | usbfs would claim the interface by itself and     |
        | keep it until the file is closed; claim it only   |
        | for this transfer                                 |
        \*-------------------------------------------------*/
        bool            was_claimed = claimed;
        TransportError  result      = Claim();

        if(result != TransportError::None)
        {
            return result;
        }

        unsigned char buf[REPORT_LEN];
        memcpy(buf, data.data(), REPORT_LEN);

        struct usbdevfs_ctrltransfer ctrl = {};
        ctrl.bRequestType   = REQUEST_TYPE;
        ctrl.bRequest       = HID_SET_REPORT;
        ctrl.wValue         = FEATURE_REPORT_ID_0;
        ctrl.wIndex         = INTERFACE;
        ctrl.wLength        = REPORT_LEN;
        ctrl.timeout        = TRANSFER_TIMEOUT_MS;
        ctrl.data           = buf;

        int n = ioctl(fd, USBDEVFS_CONTROL, &ctrl);

        if(n < 0)
        {
            last_error = std::string("control transfer: ") + strerror(errno);
            result     = TransportError::Failed;
        }
        else if(n != (int)REPORT_LEN)
        {
            last_error = "control transfer: " + std::to_string(n) + " of 8 bytes sent";
            result     = TransportError::Failed;
        }

        if(!was_claimed)
        {
            Release();
        }

        return result;
    }

    TransportError Lock() override
    {
        std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::milliseconds(LOCK_TIMEOUT_MS);

        while(true)
        {
            if(flock(fd, LOCK_EX | LOCK_NB) == 0)
            {
                locked = true;
                break;
            }

            if(errno != EWOULDBLOCK)
            {
                last_error = std::string("lock: ") + strerror(errno);
                return TransportError::Failed;
            }

            if(std::chrono::steady_clock::now() >= end)
            {
                last_error = "the pad is being used by another program";
                return TransportError::Busy;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        TransportError result = Claim();

        if(result != TransportError::None)
        {
            Unlock();
        }

        return result;
    }

    void Unlock() override
    {
        Release();

        if(locked)
        {
            flock(fd, LOCK_UN);
            locked = false;
        }
    }

    std::string Path() const override
    {
        return path;
    }

    std::string LastError() const override
    {
        return last_error;
    }

private:
    TransportError Claim()
    {
        if(claimed)
        {
            return TransportError::None;
        }

        unsigned int interface = INTERFACE;

        if(ioctl(fd, USBDEVFS_CLAIMINTERFACE, &interface) < 0)
        {
            /*---------------------------------------------*\
            | EBUSY: another program holds the interface    |
            | without the lock (e.g. a libusb script)       |
            \*---------------------------------------------*/
            bool busy   = errno == EBUSY;
            last_error  = std::string("claiming interface 2: ") + strerror(errno);
            return busy ? TransportError::Busy : TransportError::Failed;
        }

        claimed = true;
        return TransportError::None;
    }

    void Release()
    {
        if(claimed)
        {
            unsigned int interface = INTERFACE;
            ioctl(fd, USBDEVFS_RELEASEINTERFACE, &interface);
            claimed = false;
        }
    }

    int             fd;
    std::string     path;
    bool            locked;
    bool            claimed;
    std::string     last_error;
};

bool ReadAttr(const std::string& dir, const char* name, std::string& value)
{
    std::ifstream file(dir + "/" + name);

    if(!file)
    {
        return false;
    }

    std::getline(file, value);
    return true;
}

bool HasInterface(const std::string& dir, const std::string& device)
{
    /*-----------------------------------------------------*\
    | Interface directories are named                       |
    | "<device>:<config>.<ifnum>", e.g. "2-2.2:1.2"         |
    \*-----------------------------------------------------*/
    DIR* d = opendir(dir.c_str());

    if(d == nullptr)
    {
        return false;
    }

    std::string prefix = device + ":";
    std::string suffix = "." + std::to_string(INTERFACE);
    bool        found  = false;

    while(struct dirent* entry = readdir(d))
    {
        std::string name = entry->d_name;

        if(name.compare(0, prefix.size(), prefix) == 0
        && name.size() >= prefix.size() + suffix.size()
        && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0
        && name.find('.', prefix.size()) == name.size() - suffix.size())
        {
            found = true;
            break;
        }
    }

    closedir(d);
    return found;
}

}

std::vector<std::string> FindDevicePaths()
{
    std::vector<std::string>    paths;
    std::vector<std::string>    devices;

    DIR* dir = opendir("/sys/bus/usb/devices");

    if(dir == nullptr)
    {
        return paths;
    }

    while(struct dirent* entry = readdir(dir))
    {
        if(entry->d_name[0] != '.')
        {
            devices.push_back(entry->d_name);
        }
    }

    closedir(dir);
    std::sort(devices.begin(), devices.end());

    for(const std::string& device : devices)
    {
        std::string sys = "/sys/bus/usb/devices/" + device;
        std::string vid;
        std::string pid;
        std::string busnum;
        std::string devnum;

        if(!ReadAttr(sys, "idVendor", vid) || !ReadAttr(sys, "idProduct", pid))
        {
            continue;
        }

        if(strtoul(vid.c_str(), nullptr, 16) != VID || strtoul(pid.c_str(), nullptr, 16) != PID)
        {
            continue;
        }

        if(!HasInterface(sys, device) || !ReadAttr(sys, "busnum", busnum) || !ReadAttr(sys, "devnum", devnum))
        {
            continue;
        }

        char node[64];
        snprintf(node, sizeof(node), "/dev/bus/usb/%03lu/%03lu", strtoul(busnum.c_str(), nullptr, 10), strtoul(devnum.c_str(), nullptr, 10));
        paths.push_back(node);
    }

    return paths;
}

std::unique_ptr<Transport> OpenTransport(const std::string& path, std::string& error)
{
    int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);

    if(fd < 0)
    {
        error = path + ": " + strerror(errno);

        if(errno == EACCES)
        {
            error += " (no access to the USB device, see the udev rule in the README)";
        }

        return nullptr;
    }

    return std::unique_ptr<Transport>(new UsbfsTransport(fd, path));
}

}

#endif
