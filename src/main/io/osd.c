/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 Created by Marcin Baliniak
 some functions based on MinimOSD

 OSD-CMS separation by jflyper
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "platform.h"

#ifdef OSD

// XXX Must review what's included

#include "build/debug.h"
#include "build/version.h"

#include "cms/cms.h"
#include "cms/cms_types.h"
#include "cms/cms_menu_osd.h"

#include "common/axis.h"
#include "common/printf.h"
#include "common/utils.h"

#include "config/feature.h"
#include "config/parameter_group.h"
#include "config/parameter_group_ids.h"

#include "drivers/display.h"
#include "drivers/max7456.h"
#include "drivers/max7456_symbols.h"
#include "drivers/time.h"

#include "io/displayport_max7456.h"
#include "io/flashfs.h"
#include "io/gimbal.h"
#include "io/gps.h"
#include "io/osd.h"

#include "fc/config.h"
#include "fc/rc_controls.h"
#include "fc/runtime_config.h"

#include "flight/imu.h"
#include "flight/mixer.h"
#include "flight/pid.h"
#include "flight/servos.h"

#include "navigation/navigation.h"

#include "rx/rx.h"

#include "sensors/battery.h"
#include "sensors/sensors.h"

#ifdef USE_HARDWARE_REVISION_DETECTION
#include "hardware_revision.h"
#endif

// Character coordinate and attributes

#define OSD_POS(x,y)  (x | (y << 5))
#define OSD_X(x)      (x & 0x001F)
#define OSD_Y(x)      ((x >> 5) & 0x001F)

// Things in both OSD and CMS

#define IS_HI(X)  (rcData[X] > 1750)
#define IS_LO(X)  (rcData[X] < 1250)
#define IS_MID(X) (rcData[X] > 1250 && rcData[X] < 1750)

bool blinkState = true;

//extern uint8_t RSSI; // TODO: not used?

static uint16_t flyTime = 0;
static uint8_t statRssi;

typedef struct statistic_s {
    uint16_t max_speed;
    int16_t min_voltage; // /10
    int16_t max_current; // /10
    int16_t min_rssi;
    int32_t max_altitude;
    uint16_t max_distance;
} statistic_t;

static statistic_t stats;

uint16_t refreshTimeout = 0;
#define REFRESH_1S    12

static uint8_t armState;

static displayPort_t *osd7456DisplayPort;


#define AH_MAX_PITCH 200 // Specify maximum AHI pitch value displayed. Default 200 = 20.0 degrees
#define AH_MAX_ROLL 400  // Specify maximum AHI roll value displayed. Default 400 = 40.0 degrees
#define AH_SIDEBAR_WIDTH_POS 7
#define AH_SIDEBAR_HEIGHT_POS 3

PG_REGISTER_WITH_RESET_FN(osdConfig_t, osdConfig, PG_OSD_CONFIG, 0);

/**
 * Converts altitude/distance based on the current unit system (cm or 1/100th of ft).
 * @param alt Raw altitude/distance (i.e. as taken from baro.BaroAlt)
 */
static int32_t osdConvertDistanceToUnit(int32_t dist)
{
    switch (osdConfig()->units) {
        case OSD_UNIT_IMPERIAL:
            return (dist * 328) / 100; // Convert to feet / 100
        default:
            return dist;               // Already in meter / 100
    }
}

/**
 * Converts altitude/distance into a string based on the current unit system.
 * @param alt Raw altitude/distance (i.e. as taken from baro.BaroAlt in centimeters)
 */
static void osdFormatDistanceStr(char* buff, int32_t dist)
{
	int32_t dist_abs = abs(osdConvertDistanceToUnit(dist));

    switch (osdConfig()->units) {
        case OSD_UNIT_IMPERIAL:
	        if (dist < 0)
	            sprintf(buff, "-%d%c", dist_abs / 100, SYM_FT);
	        else
	            sprintf(buff, "%d%c", dist_abs / 100, SYM_FT);
	        break;
        default: // Metric
            if (dist < 0)
                sprintf(buff, "-%d.%01d%c", dist_abs / 100, (dist_abs % 100) / 10, SYM_M);
            else
                sprintf(buff, "%d.%01d%c", dist_abs / 100, (dist_abs % 100) / 10, SYM_M);
    }
}

static void osdDrawSingleElement(uint8_t item)
{
    if (!VISIBLE(osdConfig()->item_pos[item]) || BLINK(osdConfig()->item_pos[item]))
        return;

    uint8_t elemPosX = OSD_X(osdConfig()->item_pos[item]);
    uint8_t elemPosY = OSD_Y(osdConfig()->item_pos[item]);
    char buff[32];

    switch(item) {
        case OSD_RSSI_VALUE:
        {
            uint16_t osdRssi = rssi * 100 / 1024; // change range
            if (osdRssi >= 100)
                osdRssi = 99;

            buff[0] = SYM_RSSI;
            sprintf(buff + 1, "%d", osdRssi);
            break;
        }

        case OSD_MAIN_BATT_VOLTAGE:
        {
            buff[0] = SYM_BATT_5;
            sprintf(buff + 1, "%d.%1dV", vbat / 10, vbat % 10);
            break;
        }

        case OSD_CURRENT_DRAW:
        {
            buff[0] = SYM_AMP;
            sprintf(buff + 1, "%d.%02d", abs(amperage) / 100, abs(amperage) % 100);
            break;
        }

        case OSD_MAH_DRAWN:
        {
            buff[0] = SYM_MAH;
            sprintf(buff + 1, "%d", abs(mAhDrawn));
            break;
        }

#ifdef GPS
        case OSD_GPS_SATS:
        {
            buff[0] = 0x1e;
            buff[1] = 0x1f;
            sprintf(buff + 2, "%d", gpsSol.numSat);
            break;
        }

        case OSD_GPS_SPEED:
        {
            sprintf(buff, "%d%c", gpsSol.groundSpeed * 36 / 1000, 0xA1);
            break;
        }

        case OSD_GPS_LAT:
        case OSD_GPS_LON:
        {
            int32_t val;

            if (item == OSD_GPS_LAT)
            {
                buff[0] = 0xA6;
                val = gpsSol.llh.lat;
            }
            else
            {
                buff[0] = 0xA7;
                val = gpsSol.llh.lon;
            }

            if (val >= 0)
            {
                itoa(1000000000 + val, &buff[1], 10);
                buff[1] = buff[2];
                buff[2] = buff[3];
                buff[3] = '.';
            }
            else
            {
                itoa(-1000000000 + val, &buff[1], 10);
                buff[2] = buff[3];
                buff[3] = buff[4];
                buff[4] = '.';
            }
            break;
        }

        case OSD_HOME_DIR:
        {
            int16_t h = GPS_directionToHome - DECIDEGREES_TO_DEGREES(attitude.values.yaw);

            if (h < 0)
                h += 360;
            if (h >= 360)
                h -= 360;

            h = h*2/45;

            buff[0] = SYM_ARROW_UP + h;
            buff[1] = 0;
            break;
        }

        case OSD_HOME_DIST:
        {
            buff[0] = 0xA0;
            osdFormatDistanceStr(&buff[1], GPS_distanceToHome * 100);
            break;
        }

        case OSD_HEADING:
        {
            int16_t h = DECIDEGREES_TO_DEGREES(attitude.values.yaw);
            if (h < 0) h+=360;

            buff[0] = 0xA9;
            sprintf(&buff[1], "%d%c", h , 0xA8 );
            break;
        }
#endif // GPS

        case OSD_ALTITUDE:
        {
            buff[0] = SYM_ALT;
#ifdef NAV
            osdFormatDistanceStr(&buff[1], getEstimatedActualPosition(Z));
#else
            osdFormatDistanceStr(&buff[1], baro.BaroAlt));
#endif
            break;
        }

        case OSD_ONTIME:
        {
            uint32_t seconds = micros() / 1000000;
            buff[0] = SYM_ON_M;
            sprintf(buff + 1, "%02d:%02d", seconds / 60, seconds % 60);
            break;
        }

        case OSD_FLYTIME:
        {
            buff[0] = SYM_FLY_M;
            sprintf(buff + 1, "%02d:%02d", flyTime / 60, flyTime % 60);
            break;
        }

        case OSD_FLYMODE:
        {
            char *p = "ACRO";

#if 0
            if (isAirmodeActive())
                p = "AIR";
#endif

            if (FLIGHT_MODE(PASSTHRU_MODE))
                p="PASS";
            else if (FLIGHT_MODE(FAILSAFE_MODE))
                p="!FS!";
            else if (FLIGHT_MODE(HEADFREE_MODE))
                p="!HF!";
            else if (FLIGHT_MODE(NAV_RTH_MODE))
                p="RTL ";
            else if (FLIGHT_MODE(NAV_POSHOLD_MODE))
                p=" PH ";
            else if (FLIGHT_MODE(NAV_WP_MODE))
                p=" WP ";
            else if (FLIGHT_MODE(NAV_ALTHOLD_MODE))
                p=" AH ";
            else if (FLIGHT_MODE(ANGLE_MODE))
                p="STAB";
            else if (FLIGHT_MODE(HORIZON_MODE))
                p="HOR";


            max7456Write(elemPosX, elemPosY, p);
            return;
        }

        case OSD_CRAFT_NAME:
        {
            if (strlen(systemConfig()->name) == 0)
                strcpy(buff, "CRAFT_NAME");
            else {
                for (uint8_t i = 0; i < MAX_NAME_LENGTH; i++) {
                    buff[i] = toupper((unsigned char)systemConfig()->name[i]);
                    if (systemConfig()->name[i] == 0)
                        break;
                }
            }

            break;
        }

        case OSD_THROTTLE_POS:
        {
            buff[0] = SYM_THR;
            buff[1] = SYM_THR1;
            sprintf(buff + 2, "%d", (constrain(rcData[THROTTLE], PWM_RANGE_MIN, PWM_RANGE_MAX) - PWM_RANGE_MIN) * 100 / (PWM_RANGE_MAX - PWM_RANGE_MIN));
            break;
        }

#ifdef VTX
        case OSD_VTX_CHANNEL:
        {
            sprintf(buff, "CH:%d", current_vtx_channel % CHANNELS_PER_BAND + 1);
            break;
        }
#endif // VTX

        case OSD_CROSSHAIRS:
            elemPosX = 14 - 1; // Offset for 1 char to the left
            elemPosY = 6;
            if (maxScreenSize == VIDEO_BUFFER_CHARS_PAL)
                ++elemPosY;
            buff[0] = SYM_AH_CENTER_LINE;
            buff[1] = SYM_AH_CENTER;
            buff[2] = SYM_AH_CENTER_LINE_RIGHT;
            buff[3] = 0;
            break;

        case OSD_ARTIFICIAL_HORIZON:
        {
            elemPosX = 14;
            elemPosY = 6 - 4; // Top center of the AH area

            int rollAngle = -attitude.values.roll;
            int pitchAngle = attitude.values.pitch;

            if (maxScreenSize == VIDEO_BUFFER_CHARS_PAL)
                ++elemPosY;

            if (pitchAngle > AH_MAX_PITCH)
                pitchAngle = AH_MAX_PITCH;
            if (pitchAngle < -AH_MAX_PITCH)
                pitchAngle = -AH_MAX_PITCH;
            if (rollAngle > AH_MAX_ROLL)
                rollAngle = AH_MAX_ROLL;
            if (rollAngle < -AH_MAX_ROLL)
                rollAngle = -AH_MAX_ROLL;

            // Convert pitchAngle to y compensation value
            pitchAngle = (pitchAngle / 8) - 41; // 41 = 4 * 9 + 5

            for (int8_t x = -4; x <= 4; x++) {
                int y = (rollAngle * x) / 64;
                y -= pitchAngle;
                // y += 41; // == 4 * 9 + 5
                if (y >= 0 && y <= 81) {
                    max7456WriteChar(elemPosX + x, elemPosY + (y / 9), (SYM_AH_BAR9_0 + (y % 9)));
                }
            }

            osdDrawSingleElement(OSD_HORIZON_SIDEBARS);

            return;
        }

        case OSD_HORIZON_SIDEBARS:
        {
            elemPosX = 14;
            elemPosY = 6;

            if (maxScreenSize == VIDEO_BUFFER_CHARS_PAL)
                ++elemPosY;

            // Draw AH sides
            int8_t hudwidth = AH_SIDEBAR_WIDTH_POS;
            int8_t hudheight = AH_SIDEBAR_HEIGHT_POS;
            for (int8_t y = -hudheight; y <= hudheight; y++) {
                max7456WriteChar(elemPosX - hudwidth, elemPosY + y, SYM_AH_DECORATION);
                max7456WriteChar(elemPosX + hudwidth, elemPosY + y, SYM_AH_DECORATION);
            }

            // AH level indicators
            max7456WriteChar(elemPosX - hudwidth + 1, elemPosY, SYM_AH_LEFT);
            max7456WriteChar(elemPosX + hudwidth - 1, elemPosY, SYM_AH_RIGHT);

            return;
        }

        case OSD_VARIO:
        {
            int16_t v = getEstimatedActualVelocity(Z) / 50; //50cm = 1 arrow
            uint8_t vchars[] = {0x20,0x20,0x20,0x20,0x20};

            if (v >= 6)
                vchars[0] = 0xA2;
            else if (v == 5)
                vchars[0] = 0xA3;
            if (v >=4)
                vchars[1] = 0xA2;
            else if (v == 3)
                vchars[1] = 0xA3;
            if (v >=2)
                vchars[2] = 0xA2;
            else if (v == 1)
                vchars[2] = 0xA3;
            if (v <= -2)
                vchars[2] = 0xA5;
            else if (v == -1)
                vchars[2] = 0xA4;
            if (v <= -4)
                vchars[3] = 0xA5;
            else if (v == -3)
                vchars[3] = 0xA4;
            if (v <= -6)
                vchars[4] = 0xA5;
            else if (v == -5)
                vchars[4] = 0xA4;

            max7456WriteChar(elemPosX, elemPosY, vchars[0]);
            max7456WriteChar(elemPosX, elemPosY+1, vchars[1]);
            max7456WriteChar(elemPosX, elemPosY+2, vchars[2]);
            max7456WriteChar(elemPosX, elemPosY+3, vchars[3]);
            max7456WriteChar(elemPosX, elemPosY+4, vchars[4]);
            return;
        }

        case OSD_VARIO_NUM:
        {
            int16_t value = getEstimatedActualVelocity(Z) / 10; //limit precision to 10cm

            sprintf(buff, "%c%d.%01d%c", value < 0 ? '-' : ' ', abs(value / 10), abs((value % 10)), 0x9F);
            break;
        }

        case OSD_ROLL_PIDS:
        {
            sprintf(buff, "ROL %3d %3d %3d", pidBank()->pid[PID_ROLL].P, pidBank()->pid[PID_ROLL].I, pidBank()->pid[PID_ROLL].D);
            break;
        }

        case OSD_PITCH_PIDS:
        {
            sprintf(buff, "PIT %3d %3d %3d", pidBank()->pid[PID_PITCH].P, pidBank()->pid[PID_PITCH].I, pidBank()->pid[PID_PITCH].D);
            break;
        }

        case OSD_YAW_PIDS:
        {
            sprintf(buff, "YAW %3d %3d %3d", pidBank()->pid[PID_YAW].P, pidBank()->pid[PID_YAW].I, pidBank()->pid[PID_YAW].D);
            break;
        }

        case OSD_POWER:
        {
            sprintf(buff, "%dW", amperage * vbat / 1000);
            break;
        }

        default:
            return;
    }

    max7456Write(elemPosX, elemPosY, buff);
}

void osdDrawElements(void)
{
    max7456ClearScreen();

#if 0
    if (currentElement)
        osdDrawElementPositioningHelp();
#else
    if (false)
        ;
#endif
#ifdef CMS
    else if (sensors(SENSOR_ACC) || displayIsGrabbed(osd7456DisplayPort))
#else
    else if (sensors(SENSOR_ACC))
#endif
    {
        osdDrawSingleElement(OSD_ARTIFICIAL_HORIZON);
        osdDrawSingleElement(OSD_CROSSHAIRS);
    }

    osdDrawSingleElement(OSD_MAIN_BATT_VOLTAGE);
    osdDrawSingleElement(OSD_RSSI_VALUE);
    osdDrawSingleElement(OSD_FLYTIME);
    osdDrawSingleElement(OSD_ONTIME);
    osdDrawSingleElement(OSD_FLYMODE);
    osdDrawSingleElement(OSD_THROTTLE_POS);
    osdDrawSingleElement(OSD_VTX_CHANNEL);
    if (feature(FEATURE_CURRENT_METER))
    {
        osdDrawSingleElement(OSD_CURRENT_DRAW);
        osdDrawSingleElement(OSD_MAH_DRAWN);
    }
    osdDrawSingleElement(OSD_CRAFT_NAME);
    osdDrawSingleElement(OSD_ALTITUDE);
    osdDrawSingleElement(OSD_ROLL_PIDS);
    osdDrawSingleElement(OSD_PITCH_PIDS);
    osdDrawSingleElement(OSD_YAW_PIDS);
    osdDrawSingleElement(OSD_POWER);

#ifdef GPS
#ifdef CMS
    if (sensors(SENSOR_GPS) || displayIsGrabbed(osd7456DisplayPort))
#else
    if (sensors(SENSOR_GPS))
#endif
    {
        osdDrawSingleElement(OSD_GPS_SATS);
        osdDrawSingleElement(OSD_GPS_SPEED);
        osdDrawSingleElement(OSD_GPS_LAT);
        osdDrawSingleElement(OSD_GPS_LON);
        osdDrawSingleElement(OSD_HOME_DIR);
        osdDrawSingleElement(OSD_HOME_DIST);
        osdDrawSingleElement(OSD_HEADING);
    }
#endif // GPS

#if defined(BARO) || defined(GPS)
    osdDrawSingleElement(OSD_VARIO);
    osdDrawSingleElement(OSD_VARIO_NUM);
#endif // defined

}

void pgResetFn_osdConfig(osdConfig_t *instance)
{
    instance->item_pos[OSD_ALTITUDE] = OSD_POS(1, 0) | VISIBLE_FLAG;
    instance->item_pos[OSD_MAIN_BATT_VOLTAGE] = OSD_POS(12, 0) | VISIBLE_FLAG;
    instance->item_pos[OSD_RSSI_VALUE] = OSD_POS(23, 0) | VISIBLE_FLAG;
    //line 2
    instance->item_pos[OSD_HOME_DIST] = OSD_POS(1, 1);  
    instance->item_pos[OSD_HEADING] = OSD_POS(12, 1);
    instance->item_pos[OSD_GPS_SPEED] = OSD_POS(23, 1);
    
    instance->item_pos[OSD_THROTTLE_POS] = OSD_POS(1, 2) | VISIBLE_FLAG;    
    instance->item_pos[OSD_CURRENT_DRAW] = OSD_POS(1, 3) | VISIBLE_FLAG;
    instance->item_pos[OSD_MAH_DRAWN] = OSD_POS(1, 4) | VISIBLE_FLAG;

    instance->item_pos[OSD_VARIO] = OSD_POS(22,5);
    instance->item_pos[OSD_VARIO_NUM] = OSD_POS(23,7);
    instance->item_pos[OSD_HOME_DIR] = OSD_POS(14, 11);
    instance->item_pos[OSD_ARTIFICIAL_HORIZON] = OSD_POS(8, 6) | VISIBLE_FLAG;
    instance->item_pos[OSD_HORIZON_SIDEBARS] = OSD_POS(8, 6) | VISIBLE_FLAG;
    
    instance->item_pos[OSD_CRAFT_NAME] = OSD_POS(20, 2);
    instance->item_pos[OSD_VTX_CHANNEL] = OSD_POS(8, 6);

    instance->item_pos[OSD_ONTIME] = OSD_POS(23, 10) | VISIBLE_FLAG;
    instance->item_pos[OSD_FLYTIME] = OSD_POS(23, 11) | VISIBLE_FLAG;
    instance->item_pos[OSD_GPS_SATS] = OSD_POS(0, 11) | VISIBLE_FLAG;
    
    instance->item_pos[OSD_GPS_LAT] = OSD_POS(0, 12);
    instance->item_pos[OSD_FLYMODE] = OSD_POS(12, 12) | VISIBLE_FLAG;
    instance->item_pos[OSD_GPS_LON] = OSD_POS(18, 12);    
    
    instance->item_pos[OSD_ROLL_PIDS] = OSD_POS(2, 10);
    instance->item_pos[OSD_PITCH_PIDS] = OSD_POS(2, 11);
    instance->item_pos[OSD_YAW_PIDS] = OSD_POS(2, 12);
    instance->item_pos[OSD_POWER] = OSD_POS(15, 1);

    instance->rssi_alarm = 20;
    instance->cap_alarm = 2200;
    instance->time_alarm = 10; // in minutes
    instance->alt_alarm = 100; // meters or feet depend on configuration

    instance->video_system = 0;
}

void osdInit(void)
{
    BUILD_BUG_ON(OSD_POS_MAX != OSD_POS(31,31));

    char string_buffer[30];

    armState = ARMING_FLAG(ARMED);

    max7456Init(osdConfig()->video_system);

    max7456ClearScreen();

    // display logo and help
#ifdef notdef
    // Logo is disabled.
    // May be a smaller one; probably needs more symbols than BF does.
    char x = 160;
    for (int i = 1; i < 5; i++) {
        for (int j = 3; j < 27; j++) {
            if (x != 255)
                max7456WriteChar(j, i, x++);
        }
    }
#endif

    sprintf(string_buffer, "INAV VERSION: %s", FC_VERSION_STRING);
    max7456Write(5, 6, string_buffer);
#ifdef CMS
    max7456Write(7, 7,  CMS_STARTUP_HELP_TEXT1);
    max7456Write(11, 8, CMS_STARTUP_HELP_TEXT2);
    max7456Write(11, 9, CMS_STARTUP_HELP_TEXT3);
#endif

    max7456RefreshAll();

    refreshTimeout = 4 * REFRESH_1S;

    osd7456DisplayPort = max7456DisplayPortInit();
#ifdef CMS
    cmsDisplayPortRegister(osd7456DisplayPort);
#endif
}

void osdUpdateAlarms(void)
{
    // This is overdone?
    // uint16_t *itemPos = osdConfig()->item_pos;

#ifdef NAV
    int32_t alt = osdConvertDistanceToUnit(getEstimatedActualPosition(Z)) / 100;
#else
    int32_t alt = osdConvertDistanceToUnit(baro.BaroAlt) / 100;
#endif
    statRssi = rssi * 100 / 1024;

    if (statRssi < osdConfig()->rssi_alarm)
        osdConfigMutable()->item_pos[OSD_RSSI_VALUE] |= BLINK_FLAG;
    else
        osdConfigMutable()->item_pos[OSD_RSSI_VALUE] &= ~BLINK_FLAG;

    if (vbat <= (batteryWarningVoltage - 1))
        osdConfigMutable()->item_pos[OSD_MAIN_BATT_VOLTAGE] |= BLINK_FLAG;
    else
        osdConfigMutable()->item_pos[OSD_MAIN_BATT_VOLTAGE] &= ~BLINK_FLAG;

    if (STATE(GPS_FIX) == 0)
        osdConfigMutable()->item_pos[OSD_GPS_SATS] |= BLINK_FLAG;
    else
        osdConfigMutable()->item_pos[OSD_GPS_SATS] &= ~BLINK_FLAG;

    if (flyTime / 60 >= osdConfig()->time_alarm && ARMING_FLAG(ARMED))
        osdConfigMutable()->item_pos[OSD_FLYTIME] |= BLINK_FLAG;
    else
        osdConfigMutable()->item_pos[OSD_FLYTIME] &= ~BLINK_FLAG;

    if (mAhDrawn >= osdConfig()->cap_alarm)
        osdConfigMutable()->item_pos[OSD_MAH_DRAWN] |= BLINK_FLAG;
    else
        osdConfigMutable()->item_pos[OSD_MAH_DRAWN] &= ~BLINK_FLAG;

    if (alt >= osdConfig()->alt_alarm)
        osdConfigMutable()->item_pos[OSD_ALTITUDE] |= BLINK_FLAG;
    else
        osdConfigMutable()->item_pos[OSD_ALTITUDE] &= ~BLINK_FLAG;
}

void osdResetAlarms(void)
{
    osdConfigMutable()->item_pos[OSD_RSSI_VALUE] &= ~BLINK_FLAG;
    osdConfigMutable()->item_pos[OSD_MAIN_BATT_VOLTAGE] &= ~BLINK_FLAG;
    osdConfigMutable()->item_pos[OSD_GPS_SATS] &= ~BLINK_FLAG;
    osdConfigMutable()->item_pos[OSD_FLYTIME] &= ~BLINK_FLAG;
    osdConfigMutable()->item_pos[OSD_MAH_DRAWN] &= ~BLINK_FLAG;
}

static void osdResetStats(void)
{
    stats.max_current = 0;
    stats.max_speed = 0;
    stats.min_voltage = 500;
    stats.max_current = 0;
    stats.min_rssi = 99;
    stats.max_altitude = 0;
}

static void osdUpdateStats(void)
{
    int16_t value;

    if (feature(FEATURE_GPS))
    {
        value = gpsSol.groundSpeed * 36 / 1000;
        if (stats.max_speed < value)
            stats.max_speed = value;
            
        if (stats.max_distance < GPS_distanceToHome)
            stats.max_distance = GPS_distanceToHome;
    }

    if (stats.min_voltage > vbat)
        stats.min_voltage = vbat;

    value = abs(amperage / 100);
    if (stats.max_current < value)
        stats.max_current = value;

    if (stats.min_rssi > statRssi)
        stats.min_rssi = statRssi;

#ifdef NAV
    if (stats.max_altitude < getEstimatedActualPosition(Z))
        stats.max_altitude = getEstimatedActualPosition(Z);
#else
    if (stats.max_altitude < baro.BaroAlt)
        stats.max_altitude = baro.BaroAlt;
#endif
}

static void osdShowStats(void)
{
    uint8_t top = 2;
    char buff[10];

    max7456ClearScreen();
    max7456Write(2, top++, "  --- STATS ---");

    if (STATE(GPS_FIX)) {
        max7456Write(2, top, "MAX SPEED        :");
        itoa(stats.max_speed, buff, 10);
        max7456Write(22, top++, buff);
        
        max7456Write(2, top, "MAX DISTANCE     :");
        osdFormatDistanceStr(buff, stats.max_distance*100);
        max7456Write(22, top++, buff);

        max7456Write(2, top, "TRAVELED DISTANCE:");
        osdFormatDistanceStr(buff, getTotalTravelDistance());
        max7456Write(22, top++, buff);
    }

    max7456Write(2, top, "MIN BATTERY      :");
    sprintf(buff, "%d.%1dV", stats.min_voltage / 10, stats.min_voltage % 10);
    max7456Write(22, top++, buff);

    max7456Write(2, top, "MIN RSSI         :");
    itoa(stats.min_rssi, buff, 10);
    strcat(buff, "%");
    max7456Write(22, top++, buff);

    if (feature(FEATURE_CURRENT_METER)) {
        max7456Write(2, top, "MAX CURRENT      :");
        itoa(stats.max_current, buff, 10);
        strcat(buff, "A");
        max7456Write(22, top++, buff);

        max7456Write(2, top, "USED MAH         :");
        itoa(mAhDrawn, buff, 10);
        strcat(buff, "\x07");
        max7456Write(22, top++, buff);
    }

    max7456Write(2, top, "MAX ALTITUDE     :");
    osdFormatDistanceStr(buff, stats.max_altitude);
    max7456Write(22, top++, buff);

    refreshTimeout = 60 * REFRESH_1S;
}

// called when motors armed
static void osdArmMotors(void)
{
    max7456ClearScreen();
    max7456Write(12, 7, "ARMED");
    refreshTimeout = REFRESH_1S / 2;
    osdResetStats();
}

static void osdRefresh(timeUs_t currentTimeUs)
{
    static uint8_t lastSec = 0;
    uint8_t sec;

    // detect arm/disarm
    if (armState != ARMING_FLAG(ARMED)) {
        if (ARMING_FLAG(ARMED))
            osdArmMotors(); // reset statistic etc
        else
            osdShowStats(); // show statistic

        armState = ARMING_FLAG(ARMED);
    }

    osdUpdateStats();

    sec = currentTimeUs / 1000000;

    if (ARMING_FLAG(ARMED) && sec != lastSec) {
        flyTime++;
        lastSec = sec;
    }

    if (refreshTimeout) {
        if (IS_HI(THROTTLE) || IS_HI(PITCH)) // hide statistics
            refreshTimeout = 1;
        refreshTimeout--;
        if (!refreshTimeout)
            max7456ClearScreen();
        return;
    }

    blinkState = (currentTimeUs / 200000) % 2;

#ifdef CMS
    if (!displayIsGrabbed(osd7456DisplayPort)) {
        osdUpdateAlarms();
        osdDrawElements();
#ifdef OSD_CALLS_CMS
    } else {
        cmsUpdate(currentTimeUs);
#endif
    }
#endif
}

/*
 * Called periodically by the scheduler
 */
void osdUpdate(timeUs_t currentTimeUs)
{
    static uint32_t counter = 0;
#ifdef MAX7456_DMA_CHANNEL_TX
    // don't touch buffers if DMA transaction is in progress
    if (max7456DmaInProgres())
        return;
#endif // MAX7456_DMA_CHANNEL_TX

    // redraw values in buffer
    if (counter++ % 5 == 0)
        osdRefresh(currentTimeUs);
    else // rest of time redraw screen 10 chars per idle to don't lock the main idle
        max7456DrawScreen();

#ifdef CMS
    // do not allow ARM if we are in menu
    if (displayIsGrabbed(osd7456DisplayPort)) {
        DISABLE_ARMING_FLAG(OK_TO_ARM);
    }
#endif
}


#endif // OSD
