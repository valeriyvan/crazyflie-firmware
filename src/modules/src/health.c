/**
 *    ||          ____  _ __
 * +------+      / __ )(_) /_______________ _____  ___
 * | 0xBC |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * +------+    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *  ||  ||    /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * Crazyflie Firmware
 *
 * Copyright (C) 2011-2016 Bitcraze AB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, in version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * Health modules that is trying to find problems such as unbalanced
 * propellers or a bad power path/battery.
 *
 * Propeller test is done by spinning the each propeller after one another
 * while measuring the vibrations with the accelerometer.
 *
 * The battery test is done by doing a quick burst of all the motors while
 * measuring the maximum voltage sag. The sag is pretty constant over the
 * battery voltage range but usually a tiny bit higher at full voltage. The
 * result is heavily dependent on what components are mounted so needs
 * returning if anything is different/changed.
 *
 */
#define DEBUG_MODULE "HEALTH"

#include "config.h"
#include "log.h"
#include "param.h"
#include "debug.h"
#include "motors.h"
#include "sensors.h"
#include "pm.h"
#include "autoconf.h"

#include "static_mem.h"

#include "platform_defaults.h"

#define PROPTEST_NBR_OF_VARIANCE_VALUES   100

static bool startPropTest = false;
static bool startBatTest = false;

static float propTestThreshold = HEALTH_PROPELLER_TEST_THRESHOLD;

static uint16_t propTestPWMRatio = CONFIG_MOTORS_DEFAULT_PROP_TEST_PWM_RATIO;
static uint16_t batTestPWMRatio = CONFIG_MOTORS_DEFAULT_BAT_TEST_PWM_RATIO;

static uint32_t tick = 0;
NO_DMA_CCM_SAFE_ZERO_INIT static float accX[PROPTEST_NBR_OF_VARIANCE_VALUES];
NO_DMA_CCM_SAFE_ZERO_INIT static float accY[PROPTEST_NBR_OF_VARIANCE_VALUES];
NO_DMA_CCM_SAFE_ZERO_INIT static float accZ[PROPTEST_NBR_OF_VARIANCE_VALUES];
static float accVarXnf;
static float accVarYnf;
static float accVarZnf;
static int motorToTest = 0;
static uint8_t nrFailedTests = 0;
static float idleVoltage;
static float minSingleLoadedVoltage[NBR_OF_MOTORS];
static float minLoadedVoltage;

static float accVarX[NBR_OF_MOTORS];
static float accVarY[NBR_OF_MOTORS];
static float accVarZ[NBR_OF_MOTORS];
// Bit field indicating if the motors passed the motor test.
// Bit 0 - 1 = M1 passed
// Bit 1 - 1 = M2 passed
// Bit 2 - 1 = M3 passed
// Bit 3 - 1 = M4 passed
static uint8_t motorPass = 0;
static uint16_t motorTestCount = 0;
static uint8_t batteryPass = 0;
static float batterySag = 0;

typedef enum { configureAcc, measureNoiseFloor, measureProp, testBattery, restartBatTest,
               evaluatePropResult, evaluateBatResult, testDone } TestState;

#ifdef RUN_PROP_TEST_AT_STARTUP
  static TestState testState = configureAcc;
#else
  static TestState testState = testDone;
#endif

static float variance(float *buffer, uint32_t length)
{
  uint32_t i;
  float sum = 0;
  float sumSq = 0;

  for (i = 0; i < length; i++)
  {
    sum += buffer[i];
    sumSq += buffer[i] * buffer[i];
  }

  return sumSq - (sum * sum) / length;
}

/** Evaluate the values from the propeller test
 * @param low The low limit of the self test
 * @param high The high limit of the self test
 * @param value The value to compare with.
 * @param motor The motor to evaluate
 * @return True if self test within low - high limit, false otherwise
 */
static bool evaluatePropTest(float low, float high, float value, uint8_t motor)
{
  if (high != 0)
  {
    if (value < low || value > high)
    {
      DEBUG_PRINT("Propeller test on M%d [FAIL]. low: %0.2f, high: %0.2f, measured: %0.2f\n",
                  motor + 1, (double)low, (double)high, (double)value);
      return false;
    }
    else if (value > low && value < high)
    {
      DEBUG_PRINT("Propeller test on M%d [PASS]. low: %0.2f, high: %0.2f, measured: %0.2f\n",
        motor + 1, (double)low, (double)high, (double)value);
      return true;
    }
  }
  else
  {
    DEBUG_PRINT("Propeller test on M%d. No threshold set. measured: %0.2f\n",
      motor + 1, (double)value);
  }
  motorPass |= (1 << motor);

  return true;
}

bool healthShallWeRunTest(void)
{

  if (startPropTest != false) {
    testState = configureAcc;
    startPropTest = false;
  } else if (startBatTest != false) {
    testState = testBattery;
    startBatTest = false;
    tick = 0;
  }

  return (testState != testDone);
}

void healthRunTests(sensorData_t *sensors)
{
  const MotorHealthTestDef* healthTestSettings;
  int32_t sampleIndex;

  /* Propeller test */
  if (testState == configureAcc)
  {
    motorPass = 0;
    sensorsSetAccMode(ACC_MODE_PROPTEST);
    testState = measureNoiseFloor;
    minLoadedVoltage = idleVoltage = pmGetBatteryVoltage();
    minSingleLoadedVoltage[MOTOR_M1] = minLoadedVoltage;
    minSingleLoadedVoltage[MOTOR_M2] = minLoadedVoltage;
    minSingleLoadedVoltage[MOTOR_M3] = minLoadedVoltage;
    minSingleLoadedVoltage[MOTOR_M4] = minLoadedVoltage;
    // Make sure motors are stopped first.
    motorsStop();
  }
  if (testState == measureNoiseFloor)
  {
    accX[tick] = sensors->acc.x;
    accY[tick] = sensors->acc.y;
    accZ[tick] = sensors->acc.z;

    if (++tick >= PROPTEST_NBR_OF_VARIANCE_VALUES)
    {
      tick = 0;
      accVarXnf = variance(accX, PROPTEST_NBR_OF_VARIANCE_VALUES);
      accVarYnf = variance(accY, PROPTEST_NBR_OF_VARIANCE_VALUES);
      accVarZnf = variance(accZ, PROPTEST_NBR_OF_VARIANCE_VALUES);
      DEBUG_PRINT("Acc noise floor variance X+Y:%f, (Z:%f)\n",
                  (double)accVarXnf + (double)accVarYnf, (double)accVarZnf);
      testState = measureProp;
    }
  }
  else if (testState == measureProp)
  {
    healthTestSettings = motorsGetHealthTestSettings(motorToTest);

    sampleIndex = ((int32_t) tick) - healthTestSettings->varianceMeasurementStartMsec;
    if (sampleIndex >= 0 && sampleIndex < PROPTEST_NBR_OF_VARIANCE_VALUES)
    {
      accX[sampleIndex] = sensors->acc.x;
      accY[sampleIndex] = sensors->acc.y;
      accZ[sampleIndex] = sensors->acc.z;
      if (pmGetBatteryVoltage() < minSingleLoadedVoltage[motorToTest])
      {
        minSingleLoadedVoltage[motorToTest] = pmGetBatteryVoltage();
      }
    }
    tick++;

    if (sampleIndex == PROPTEST_NBR_OF_VARIANCE_VALUES)
    {
      accVarX[motorToTest] = variance(accX, PROPTEST_NBR_OF_VARIANCE_VALUES);
      accVarY[motorToTest] = variance(accY, PROPTEST_NBR_OF_VARIANCE_VALUES);
      accVarZ[motorToTest] = variance(accZ, PROPTEST_NBR_OF_VARIANCE_VALUES);
      DEBUG_PRINT("Motor M%d variance X+Y: %.2f (Z:%.2f), voltage sag:%.2f\n",
                   motorToTest+1,
                   (double)accVarX[motorToTest] + (double)accVarY[motorToTest],
                   (double)accVarZ[motorToTest],
                   (double)(idleVoltage - minSingleLoadedVoltage[motorToTest]));
    }

    if (tick == 1 && healthTestSettings->onPeriodMsec > 0)
    {
      motorsSetRatio(motorToTest, propTestPWMRatio > 0 ? propTestPWMRatio : healthTestSettings->onPeriodPWMRatioProp);
    }
    else if (tick == healthTestSettings->onPeriodMsec)
    {
      motorsSetRatio(motorToTest, 0);
    }
    else if (tick >= healthTestSettings->onPeriodMsec + healthTestSettings->offPeriodMsec)
    {
      tick = 0;
      motorToTest++;
      if (motorToTest >= NBR_OF_MOTORS)
      {
        tick = 0;
        motorToTest = 0;
        testState = evaluatePropResult;
        sensorsSetAccMode(ACC_MODE_FLIGHT);
      }
    }
  }
  /* Experimental battery test, tick should count up each ms */
  else if (testState == testBattery)
  {
    healthTestSettings = motorsGetHealthTestSettings(0);

    if (tick == 0)
    {
      batteryPass = 0;
      minLoadedVoltage = idleVoltage = pmGetBatteryVoltage();
    }
    if (tick == 1)
    {
      motorsSetRatio(MOTOR_M1, batTestPWMRatio > 0 ? batTestPWMRatio : healthTestSettings->onPeriodPWMRatioBat);
      motorsSetRatio(MOTOR_M2, batTestPWMRatio > 0 ? batTestPWMRatio : healthTestSettings->onPeriodPWMRatioBat);
      motorsSetRatio(MOTOR_M3, batTestPWMRatio > 0 ? batTestPWMRatio : healthTestSettings->onPeriodPWMRatioBat);
      motorsSetRatio(MOTOR_M4, batTestPWMRatio > 0 ? batTestPWMRatio : healthTestSettings->onPeriodPWMRatioBat);
    }
    else if (tick < 50)
    {
      if (pmGetBatteryVoltage() < minLoadedVoltage)
        minLoadedVoltage = pmGetBatteryVoltage();
    }
    else if (tick == 50)
    {
      motorsStop();
      testState = evaluateBatResult;
    }
    tick++;
  }
  else if (testState == restartBatTest)
  {
    // Mainly used for testing
    if (tick++ > 2000)
    {
      DEBUG_PRINT("Idle:%.2f sag: %.2f\n", (double)idleVoltage,
                  (double)(idleVoltage - minLoadedVoltage));
      testState = testBattery;
      tick = 0;
    }
  }
  else if (testState == evaluateBatResult)
  {
    batterySag = idleVoltage - minLoadedVoltage;
    if (batterySag > BAT_LOADING_SAG_THRESHOLD)
    {
      DEBUG_PRINT("Battery sag during load test (%.2f > %.2f) [FAILED].\n", (double)batterySag,
                  (double)(BAT_LOADING_SAG_THRESHOLD));
      batteryPass = 0;
    }
    else
    {
      DEBUG_PRINT("Idle:%.2fV sag: %.2fV (< %.2fV) [OK]\n", (double)idleVoltage,
                  (double)(batterySag), (double)(BAT_LOADING_SAG_THRESHOLD));
      batteryPass = 1;
    }
    testState = testDone;
  }
  else if (testState == evaluatePropResult)
  {
    for (int m = 0; m < NBR_OF_MOTORS; m++)
    {
      if (!evaluatePropTest(0, propTestThreshold,  accVarX[m] + accVarY[m], m))
      {
        nrFailedTests++;
        for (int j = 0; j < 3; j++)
        {
          motorsBeep(m, true, testsound[m], (uint16_t)(MOTORS_TIM_BEEP_CLK_FREQ / A4)/ 20);
          vTaskDelay(M2T(MOTORS_TEST_ON_TIME_MS));
          motorsBeep(m, false, 0, 0);
          vTaskDelay(M2T(100));
        }
      }
    }
#ifdef PLAY_STARTUP_MELODY_ON_MOTORS
    if (nrFailedTests == 0)
    {
      for (int m = 0; m < NBR_OF_MOTORS; m++)
      {
        motorsBeep(m, true, testsound[m], (uint16_t)(MOTORS_TIM_BEEP_CLK_FREQ / A4)/ 20);
        vTaskDelay(M2T(MOTORS_TEST_ON_TIME_MS));
        motorsBeep(m, false, 0, 0);
        vTaskDelay(M2T(MOTORS_TEST_DELAY_TIME_MS));
      }
    }
#endif
    motorTestCount++;
    testState = testDone;
  }
}

/**
 * Health modules that is trying to find problems such as unbalanced
 * propellers or a bad power path/battery.
 */
PARAM_GROUP_START(health)

/**
 * @brief Set nonzero to initiate test of propellers
 */
PARAM_ADD_CORE(PARAM_UINT8, startPropTest, &startPropTest)

/**
 * @brief Set nonzero to initiate test of battery
 */
PARAM_ADD_CORE(PARAM_UINT8, startBatTest, &startBatTest)

/**
 * @brief Set nonzero to create a threshold ([0 - propTestThreshold]) for the propeller test.
 */
PARAM_ADD_CORE(PARAM_FLOAT | PARAM_PERSISTENT, propTestThreshold, &propTestThreshold)

/**
 * @brief PWM ratio to use when testing propellers. Required for brushless motors. [0 - UINT16_MAX]
 */
PARAM_ADD_CORE(PARAM_UINT16 | PARAM_PERSISTENT, propTestPWMRatio, &propTestPWMRatio)

/**
 * @brief PWM ratio to use when testing the battery. [0 - UINT16_MAX]
 */
PARAM_ADD_CORE(PARAM_UINT16 | PARAM_PERSISTENT, batTestPWMRatio, &batTestPWMRatio)

PARAM_GROUP_STOP(health)

/**
 * Logging of the result from the health checks.
 */
LOG_GROUP_START(health)
/**
 * @brief Variance test result of accel. axis X on motor 1
 */
LOG_ADD(LOG_FLOAT, motorVarXM1, &accVarX[0])
/**
 * @brief Variance test result of accel. axis Y on motor 1
 */
LOG_ADD(LOG_FLOAT, motorVarYM1, &accVarY[0])
/**
 * @brief Variance test result of accel. axis X on motor 2
 */
LOG_ADD(LOG_FLOAT, motorVarXM2, &accVarX[1])
/**
 * @brief Variance test result of accel. axis Y on motor 2
 */
LOG_ADD(LOG_FLOAT, motorVarYM2, &accVarY[1])
/**
 * @brief Variance test result of accel. axis X on motor 3
 */
LOG_ADD(LOG_FLOAT, motorVarXM3, &accVarX[2])
/**
 * @brief Variance test result of accel. axis Y on motor 3
 */
LOG_ADD(LOG_FLOAT, motorVarYM3, &accVarY[2])
/**
 * @brief Variance test result of accel. axis X on motor 4
 */
LOG_ADD(LOG_FLOAT, motorVarXM4, &accVarX[3])
/**
 * @brief Variance test result of accel. axis Y on motor 4
 */
LOG_ADD(LOG_FLOAT, motorVarYM4, &accVarY[3])
/**
 * @brief Propeller test result, bit is one if OK. [Bit0=M1 Bit1=M2 ...]
 */
LOG_ADD_CORE(LOG_UINT8, motorPass, &motorPass)
/**
 * @brief Battery voltage sag test result. [V]
 */
LOG_ADD(LOG_FLOAT, batterySag, &batterySag)
/**
 * @brief Battery test result. Nonzero if OK.
 */
LOG_ADD_CORE(LOG_UINT8, batteryPass, &batteryPass)
// Not useful other then for debugging. Remove
LOG_ADD(LOG_UINT16, motorTestCount, &motorTestCount)
LOG_GROUP_STOP(health)
