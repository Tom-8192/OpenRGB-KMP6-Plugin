/*---------------------------------------------------------*\
| protocol_test.cpp                                         |
|                                                           |
|   Tests of the reports and the sending against a          |
|   simulated pad (no Qt, no OpenRGB, no device needed).    |
|   Expected bytes as in kmp6-core protocol.rs/device.rs    |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "KMP6Protocol.h"

using namespace kmp6;

static int failed = 0;

#define CHECK(cond)                                                             \
    do                                                                          \
    {                                                                           \
        if(!(cond))                                                             \
        {                                                                       \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            failed++;                                                           \
        }                                                                       \
    } while(0)

/*---------------------------------------------------------*\
| Simulated pad: records reports, keeps the last effect     |
| and slot colours, can refuse reports or the lock          |
\*---------------------------------------------------------*/
class FakePad : public Transport
{
public:
    std::vector<Report>     sent;
    Report                  effect      = {};
    Rgb                     colors[SLOTS];
    unsigned int            refuse      = 0;    /* reports refused first   */
    bool                    fail        = false;
    bool                    busy        = false;
    bool                    locked      = false;
    unsigned int            locks       = 0;

    FakePad()
    {
        for(unsigned int idx = 0; idx < SLOTS; idx++)
        {
            colors[idx] = {{ 0, 0, 0 }};
        }
    }

    TransportError SetFeature(const Report& data) override
    {
        if(fail)
        {
            return TransportError::Failed;
        }

        if(refuse > 0)
        {
            refuse--;
            return TransportError::Failed;
        }

        sent.push_back(data);

        if(data[0] == CMD_EFFECT)
        {
            effect      = data;
            colors[0]   = {{ data[5], data[6], data[7] }};
        }
        else if(data[0] >= 0x09 && data[0] <= 0x0B)
        {
            unsigned int first = 1 + 2 * (data[0] - 0x09);
            colors[first]       = {{ data[1], data[2], data[3] }};
            colors[first + 1]   = {{ data[4], data[5], data[6] }};
        }

        return TransportError::None;
    }

    TransportError Lock() override
    {
        if(busy)
        {
            return TransportError::Busy;
        }

        locked = true;
        locks++;
        return TransportError::None;
    }

    void Unlock() override
    {
        locked = false;
    }
};

static Pad Quick(Transport& transport)
{
    Pad pad(transport);
    pad.gap         = std::chrono::milliseconds(0);
    pad.retry_delay = std::chrono::milliseconds(0);
    return pad;
}

static bool Same(const Report& a, std::initializer_list<unsigned char> b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
}

static void TestFactoryDefaults()
{
    Settings s;
    CHECK(s.mode == Mode::Static);
    CHECK(s.brightness == 9);
    CHECK(s.speed == 4);
    CHECK(s.direction == Direction::Left);
    CHECK(s.cycle);
    CHECK(s.ColorByte() == 0x70);
    CHECK(s.colors[2] == (Rgb{{ 0, 0, 255 }}));
}

static void TestStaticPackets()
{
    Settings s;
    s.cycle     = false;
    s.colors[0] = {{ 0x12, 0x34, 0x56 }};

    std::vector<Report> p = s.Packets();
    CHECK(p.size() == 8);
    CHECK(Same(p[0], { 0x09, 0, 255, 0, 0, 0, 255, 0 }));
    CHECK(Same(p[1], { 0x0A, 255, 255, 0, 0, 255, 255, 0 }));
    CHECK(Same(p[2], { 0x0B, 255, 0, 255, 255, 255, 255, 0 }));
    /*-----------------------------------------------------*\
    | Static: speed byte 0 like G-aim                       |
    \*-----------------------------------------------------*/
    CHECK(Same(p[3], { 0x08, 0, 32, 0, 0x00, 0x12, 0x34, 0x56 }));
    CHECK(p[4] == p[0] && p[7] == p[3]);
}

static void TestBreatheSlot()
{
    Settings s;
    s.mode  = Mode::Breathe;
    s.cycle = false;
    s.slot  = 2;

    std::vector<Report> p = s.Packets();
    CHECK(p.size() == 8);
    CHECK(Same(p[3], { 0x08, 1, 32, 5, 0x20, 255, 0, 0 }));
}

static void TestNeonWaveOff()
{
    Settings s;
    s.brightness    = 3;
    s.speed         = 2;
    s.direction     = Direction::Right;

    s.mode = Mode::Neon;
    std::vector<Report> p = s.Packets();
    CHECK(p.size() == 1 && Same(p[0], { 0x08, 2, 10, 3, 0, 0, 0, 0 }));

    s.mode = Mode::Wave;
    p = s.Packets();
    CHECK(p.size() == 1 && Same(p[0], { 0x08, 3, 10, 3, 0, 0, 0, 0 }));

    s.direction = Direction::Left;
    p = s.Packets();
    CHECK(Same(p[0], { 0x08, 3, 10, 3, 1, 0, 0, 0 }));

    /*-----------------------------------------------------*\
    | Off = static with brightness 0                        |
    \*-----------------------------------------------------*/
    s.mode = Mode::Off;
    p = s.Packets();
    CHECK(p.size() == 1 && Same(p[0], { 0x08, 0, 0, 3, 1, 0, 0, 0 }));
}

static void TestInvalid()
{
    Settings s;
    s.brightness = 10;
    CHECK(s.Packets().empty());

    FakePad fake;
    Pad     pad = Quick(fake);
    CHECK(pad.Apply(s) == Status::Invalid);
    CHECK(fake.sent.empty());
}

static void TestApply()
{
    FakePad     fake;
    Pad         pad = Quick(fake);
    Settings    s;
    s.mode  = Mode::Breathe;
    s.cycle = false;
    s.slot  = 2;

    CHECK(pad.Apply(s) == Status::Ok);
    CHECK(fake.sent.size() == 8);
    CHECK(fake.locks == 1);
    CHECK(!fake.locked);
    CHECK(Same(fake.effect, { 0x08, 1, 32, 5, 0x20, 255, 0, 0 }));

    for(unsigned int idx = 0; idx < SLOTS; idx++)
    {
        CHECK(fake.colors[idx] == FACTORY_COLORS[idx]);
    }
}

static void TestErrorsReleaseTheLock()
{
    FakePad fake;
    fake.fail = true;
    Pad pad = Quick(fake);

    CHECK(pad.Apply(Settings()) == Status::SendFailed);
    CHECK(!fake.locked);

    FakePad busy;
    busy.busy = true;
    Pad pad2 = Quick(busy);
    CHECK(pad2.Apply(Settings()) == Status::Locked);
    CHECK(busy.sent.empty());
}

static void TestRetries()
{
    Settings s;
    s.mode = Mode::Neon;

    FakePad fake;
    fake.refuse = 3;
    Pad pad = Quick(fake);
    CHECK(pad.Apply(s) == Status::Ok);
    CHECK(fake.sent.size() == 1);

    FakePad never;
    never.refuse = 4;
    Pad pad2 = Quick(never);
    CHECK(pad2.Apply(s) == Status::SendFailed);
    CHECK(never.sent.empty());
}

static void TestModeIds()
{
    Mode m = Mode::Static;
    CHECK(ModeFromId("wave", m) && m == Mode::Wave);
    CHECK(ModeFromId("off", m) && m == Mode::Off);
    CHECK(!ModeFromId("rainbow", m));
    CHECK(std::string(ModeId(Mode::Breathe)) == "breathe");
}

static void TestEquality()
{
    Settings a;
    Settings b;
    CHECK(a == b);
    b.colors[6] = {{ 1, 2, 3 }};
    CHECK(a != b);
}

int main()
{
    TestFactoryDefaults();
    TestStaticPackets();
    TestBreatheSlot();
    TestNeonWaveOff();
    TestInvalid();
    TestApply();
    TestErrorsReleaseTheLock();
    TestRetries();
    TestModeIds();
    TestEquality();

    if(failed == 0)
    {
        printf("all tests passed\n");
        return 0;
    }

    printf("%d check(s) failed\n", failed);
    return 1;
}
