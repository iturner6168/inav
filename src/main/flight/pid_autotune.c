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

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include <platform.h>

#include "build/build_config.h"
#include "build/debug.h"

#include "common/axis.h"
#include "common/filter.h"
#include "common/maths.h"

#include "drivers/time.h"

#include "config/parameter_group.h"
#include "config/parameter_group_ids.h"

#include "fc/config.h"
#include "fc/controlrate_profile.h"
#include "fc/rc_controls.h"
#include "fc/runtime_config.h"

#include "flight/pid.h"

typedef enum {
    DEMAND_TOO_LOW,
    DEMAND_UNDERSHOOT,
    DEMAND_OVERSHOOT,
} pidAutotuneState_e;

typedef struct {
    pidAutotuneState_e  state;
    timeMs_t            stateEnterTime;

    bool    pidSaturated;
    float   gainP;
    float   gainI;
    float   gainD;
} pidAutotuneData_t;

#define AUTOTUNE_SAVE_PERIOD        5000        // Save interval is 5 seconds - when we turn off autotune we'll restore values from previous update at most 5 sec ago

#if defined(AUTOTUNE_FIXED_WING) || defined(AUTOTUNE_MULTIROTOR)

static pidAutotuneData_t    tuneCurrent[XYZ_AXIS_COUNT];
static pidAutotuneData_t    tuneSaved[XYZ_AXIS_COUNT];
static timeMs_t             lastGainsUpdateTime;

void autotuneUpdateGains(pidAutotuneData_t * data)
{
    for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
        pidBankMutable()->pid[axis].P = lrintf(data[axis].gainP);
        pidBankMutable()->pid[axis].I = lrintf(data[axis].gainI);
        pidBankMutable()->pid[axis].D = lrintf(data[axis].gainD);
    }
    schedulePidGainsUpdate();
}

void autotuneCheckUpdateGains(void)
{
    const timeMs_t currentTimeMs = millis();

    if ((currentTimeMs - lastGainsUpdateTime) < AUTOTUNE_SAVE_PERIOD) {
        return;
    }

    // If pilot will exit autotune we'll restore values we are flying now
    memcpy(tuneSaved, tuneCurrent, sizeof(pidAutotuneData_t) * XYZ_AXIS_COUNT);
    autotuneUpdateGains(tuneSaved);
    lastGainsUpdateTime = currentTimeMs;
}

void autotuneStart(void)
{
    for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
        tuneCurrent[axis].gainP = pidBank()->pid[axis].P;
        tuneCurrent[axis].gainI = pidBank()->pid[axis].I;
        tuneCurrent[axis].gainD = pidBank()->pid[axis].D;
        tuneCurrent[axis].pidSaturated = false;
        tuneCurrent[axis].stateEnterTime = millis();
        tuneCurrent[axis].state = DEMAND_TOO_LOW;
    }

    memcpy(tuneSaved, tuneCurrent, sizeof(pidAutotuneData_t) * XYZ_AXIS_COUNT);
    lastGainsUpdateTime = millis();
}

void autotuneUpdateState(void)
{
    if (IS_RC_MODE_ACTIVE(BOXAUTOTUNE) && ARMING_FLAG(ARMED)) {
        if (!FLIGHT_MODE(AUTO_TUNE)) {
            autotuneStart();
            ENABLE_FLIGHT_MODE(AUTO_TUNE);
        }
        else {
            autotuneCheckUpdateGains();
        }
    } else {
        if (FLIGHT_MODE(AUTO_TUNE)) {
            autotuneUpdateGains(tuneSaved);
        }

        DISABLE_FLIGHT_MODE(AUTO_TUNE);
    }
}
#endif

#if defined(AUTOTUNE_FIXED_WING)
#define AUTOTUNE_FIXED_WING_OVERSHOOT_TIME      100
#define AUTOTUNE_FIXED_WING_UNDERSHOOT_TIME     200
#define AUTOTUNE_FIXED_WING_DECREASE_STEP       8       // 8%
#define AUTOTUNE_FIXED_WING_INCREASE_STEP       5       // 5%
#define AUTOTUNE_FIXED_WING_MIN_FF              10
#define AUTOTUNE_FIXED_WING_MAX_FF              200
   
void autotuneFixedWingUpdate(const flight_dynamics_index_t axis, float desiredRateDps, float reachedRateDps, float pidOutput)
{
    const timeMs_t currentTimeMs = millis();
    const float absDesiredRateDps = fabsf(desiredRateDps);
    float maxDesiredRate = currentControlRateProfile->rates[axis] * 10.0f;
    pidAutotuneState_e newState;

    // Use different max desired rate in ANGLE for pitch and roll
    // Maximum reasonable error in ANGLE mode is 200% of angle inclination (control dublet), but we are conservative and tune for control singlet.
    if (axis == FD_PITCH || axis == FD_ROLL) {
        float maxDesiredRateInAngleMode = DECIDEGREES_TO_DEGREES(pidProfile()->max_angle_inclination[axis] * 1.0f) * pidBank()->pid[PID_LEVEL].P / FP_PID_LEVEL_P_MULTIPLIER;
        maxDesiredRate = MIN(maxDesiredRate, maxDesiredRateInAngleMode);
    }

    if (fabsf(pidOutput) >= pidProfile()->pidSumLimit) {
        // If we have saturated the pid output by P+FF don't increase the gain
        tuneCurrent[axis].pidSaturated = true;
    }

    if (absDesiredRateDps < 0.75f * maxDesiredRate) {
        // We can make decisions only when we are demanding at least 75% of max configured rate
        newState = DEMAND_TOO_LOW;
    }
    else if (fabsf(reachedRateDps) > absDesiredRateDps) {
        newState = DEMAND_OVERSHOOT;
    }
    else {
        newState = DEMAND_UNDERSHOOT;
    }

    if (newState != tuneCurrent[axis].state) {
        const timeDelta_t stateTimeMs = currentTimeMs - tuneCurrent[axis].stateEnterTime;
        bool gainsUpdated = false;

        switch(tuneCurrent[axis].state) {
            case DEMAND_TOO_LOW:
                break;
            case DEMAND_OVERSHOOT:
                if (stateTimeMs >= AUTOTUNE_FIXED_WING_OVERSHOOT_TIME) {
                    tuneCurrent[axis].gainD = tuneCurrent[axis].gainD * (100 - AUTOTUNE_FIXED_WING_DECREASE_STEP) / 100.0f;
                    if (tuneCurrent[axis].gainD < AUTOTUNE_FIXED_WING_MIN_FF) {
                        tuneCurrent[axis].gainD = AUTOTUNE_FIXED_WING_MIN_FF;
                    }
                    gainsUpdated = true;
                }
                break;
            case DEMAND_UNDERSHOOT:
                if (stateTimeMs >= AUTOTUNE_FIXED_WING_UNDERSHOOT_TIME && !tuneCurrent[axis].pidSaturated) {
                    tuneCurrent[axis].gainD = tuneCurrent[axis].gainD * (100 + AUTOTUNE_FIXED_WING_INCREASE_STEP) / 100.0f;
                    if (tuneCurrent[axis].gainD > AUTOTUNE_FIXED_WING_MAX_FF) {
                        tuneCurrent[axis].gainD = AUTOTUNE_FIXED_WING_MAX_FF;
                    }
                    gainsUpdated = true;
                }
                break;
        }

        if (gainsUpdated) {
            // Set P-gain to 10% of FF gain (quite agressive - FIXME)
            tuneCurrent[axis].gainP = tuneCurrent[axis].gainD * 0.1f;                       // TODO: Figure out optimal ratio between P and FF

            // Set integrator gain to reach the same response as FF gain in 1 second
            tuneCurrent[axis].gainI = (tuneCurrent[axis].gainD / FP_PID_RATE_FF_MULTIPLIER) * 1.0f * FP_PID_RATE_I_MULTIPLIER;
            tuneCurrent[axis].gainI = constrainf(tuneCurrent[axis].gainI, 1.0f, 50.0f);
            autotuneUpdateGains(tuneCurrent);
        }

        // Change state and reset saturation flag
        tuneCurrent[axis].state = newState;
        tuneCurrent[axis].stateEnterTime = currentTimeMs;
        tuneCurrent[axis].pidSaturated = false;
    }
}
#endif