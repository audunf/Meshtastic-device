#include "configuration.h"
#include "power.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "main.h"
#include "sleep.h"
#include "utils.h"

#ifdef TBEAM_V10
// FIXME. nasty hack cleanup how we load axp192
#undef AXP192_SLAVE_ADDRESS
#include "axp20x.h"

AXP20X_Class axp;
#else
// Copy of the base class defined in axp20x.h. 
// I'd rather not inlude axp20x.h as it brings Wire dependency.
class HasBatteryLevel {
public:
  /**
   * Battery state of charge, from 0 to 100 or -1 for unknown
   */
  virtual int getBattPercentage() { return -1; }

  /**
   * The raw voltage of the battery or NAN if unknown
   */
  virtual float getBattVoltage() { return NAN; }

  /**
   * return true if there is a battery installed in this unit
   */
  virtual bool isBatteryConnect() { return false; }

  virtual bool isVBUSPlug() { return false; }
  virtual bool isChargeing() { return false; }
};
#endif

bool pmu_irq = false;

Power *power;

using namespace meshtastic;

#ifndef AREF_VOLTAGE
#if defined(NRF52_SERIES)
/*
 * Internal Reference is +/-0.6V, with an adjustable gain of 1/6, 1/5, 1/4,
 * 1/3, 1/2 or 1, meaning 3.6, 3.0, 2.4, 1.8, 1.2 or 0.6V for the ADC levels.
 *
 * External Reference is VDD/4, with an adjustable gain of 1, 2 or 4, meaning
 * VDD/4, VDD/2 or VDD for the ADC levels.
 *
 * Default settings are internal reference with 1/6 gain (GND..3.6V ADC range)
 */
#define AREF_VOLTAGE 3.6
#else
#define AREF_VOLTAGE 3.3
#endif
#endif

/**
 * If this board has a battery level sensor, set this to a valid implementation
 */
static HasBatteryLevel *batteryLevel; // Default to NULL for no battery level sensor

/**
 * A simple battery level sensor that assumes the battery voltage is attached via a voltage-divider to an analog input
 */
class AnalogBatteryLevel : public HasBatteryLevel
{
    /**
     * Battery state of charge, from 0 to 100 or -1 for unknown
     *
     * FIXME - use a lipo lookup table, the current % full is super wrong
     */
    virtual int getBattPercentage()
    {
        float v = getBattVoltage();

        if (v < noBatVolt)
            return -1; // If voltage is super low assume no battery installed

        if (v > chargingVolt)
            return 0; // While charging we can't report % full on the battery

        return 100 * (v - emptyVolt) / (fullVolt - emptyVolt);
    }

    /**
     * The raw voltage of the batteryin millivolts or NAN if unknown
     */
    virtual float getBattVoltage()
    {

#ifndef ADC_MULTIPLIER
#define ADC_MULTIPLIER 2.0
#endif 

#ifdef BATTERY_PIN
        // Do not call analogRead() often. 
        const uint32_t min_read_interval = 5000;
        if (millis() - last_read_time_ms > min_read_interval) {
            last_read_time_ms = millis();
            uint32_t raw = analogRead(BATTERY_PIN);
            float scaled;
            #ifndef VBAT_RAW_TO_SCALED
            scaled = 1000.0 * ADC_MULTIPLIER * (AREF_VOLTAGE / 1024.0) * raw;
            #else
            scaled = VBAT_RAW_TO_SCALED(raw); //defined in variant.h
            #endif
            // DEBUG_MSG("battery gpio %d raw val=%u scaled=%u\n", BATTERY_PIN, raw, (uint32_t)(scaled));
            last_read_value = scaled;
            return scaled;
        } else {
            return last_read_value;
        }
#else
        return NAN;
#endif
    }

    /**
     * return true if there is a battery installed in this unit
     */
    virtual bool isBatteryConnect() { return getBattPercentage() != -1; }

    /// If we see a battery voltage higher than physics allows - assume charger is pumping
    /// in power
    virtual bool isVBUSPlug() { return getBattVoltage() > chargingVolt; }

    /// Assume charging if we have a battery and external power is connected.
    /// we can't be smart enough to say 'full'?
    virtual bool isChargeing() { return isBatteryConnect() && isVBUSPlug(); }

  private:
    /// If we see a battery voltage higher than physics allows - assume charger is pumping
    /// in power

    /// For heltecs with no battery connected, the measured voltage is 2204, so raising to 2230 from 2100
    const float fullVolt = 4200, emptyVolt = 3270, chargingVolt = 4210, noBatVolt = 2230;
    float last_read_value = 0.0;
    uint32_t last_read_time_ms = 0;
};

AnalogBatteryLevel analogLevel;

Power::Power() : OSThread("Power") {}

bool Power::analogInit()
{
#ifdef BATTERY_PIN
    DEBUG_MSG("Using analog input %d for battery level\n", BATTERY_PIN);

    // disable any internal pullups
    pinMode(BATTERY_PIN, INPUT);

#ifndef NO_ESP32
    // ESP32 needs special analog stuff
    adcAttachPin(BATTERY_PIN);
#endif
#ifdef NRF52_SERIES
#ifdef VBAT_AR_INTERNAL
     analogReference(VBAT_AR_INTERNAL);
#else
    analogReference(AR_INTERNAL); // 3.6V
#endif
#endif

#ifndef BATTERY_SENSE_RESOLUTION_BITS
#define BATTERY_SENSE_RESOLUTION_BITS 10
#endif
    
    // adcStart(BATTERY_PIN);
    analogReadResolution(BATTERY_SENSE_RESOLUTION_BITS); // Default of 12 is not very linear. Recommended to use 10 or 11 depending on needed resolution.
    batteryLevel = &analogLevel;
    return true;
#else
    return false;
#endif
}

bool Power::setup()
{
    bool found = axp192Init();

    if (!found) {
        found = analogInit();
    }
    enabled = found;

    return found;
}

void Power::shutdown()
{
#ifdef TBEAM_V10
    DEBUG_MSG("Shutting down\n");
    axp.shutdown();
#elif NRF52_SERIES
    doDeepSleep(DELAY_FOREVER);
#endif
}

/// Reads power status to powerStatus singleton.
//
// TODO(girts): move this and other axp stuff to power.h/power.cpp.
void Power::readPowerStatus()
{
    if (batteryLevel) {
        bool hasBattery = batteryLevel->isBatteryConnect();
        int batteryVoltageMv = 0;
        int8_t batteryChargePercent = 0;
        if (hasBattery) {
            batteryVoltageMv = batteryLevel->getBattVoltage();
            // If the AXP192 returns a valid battery percentage, use it
            if (batteryLevel->getBattPercentage() >= 0) {
                batteryChargePercent = batteryLevel->getBattPercentage();
            } else {
                // If the AXP192 returns a percentage less than 0, the feature is either not supported or there is an error
                // In that case, we compute an estimate of the charge percent based on maximum and minimum voltages defined in
                // power.h
                batteryChargePercent =
                    clamp((int)(((batteryVoltageMv - BAT_MILLIVOLTS_EMPTY) * 1e2) / (BAT_MILLIVOLTS_FULL - BAT_MILLIVOLTS_EMPTY)),
                          0, 100);
            }
        }

        // Notify any status instances that are observing us
        const PowerStatus powerStatus =
            PowerStatus(hasBattery ? OptTrue : OptFalse, batteryLevel->isVBUSPlug() ? OptTrue : OptFalse,
                        batteryLevel->isChargeing() ? OptTrue : OptFalse, batteryVoltageMv, batteryChargePercent);
        DEBUG_MSG("Battery: usbPower=%d, isCharging=%d, batMv=%d, batPct=%d\n", powerStatus.getHasUSB(),
                  powerStatus.getIsCharging(), powerStatus.getBatteryVoltageMv(), powerStatus.getBatteryChargePercent());
        newStatus.notifyObservers(&powerStatus);

        // If we have a battery at all and it is less than 10% full, force deep sleep
        if (powerStatus.getHasBattery() && !powerStatus.getHasUSB() && batteryLevel->getBattVoltage() < MIN_BAT_MILLIVOLTS)
            powerFSM.trigger(EVENT_LOW_BATTERY);
    } else {
        // No power sensing on this board - tell everyone else we have no idea what is happening
        const PowerStatus powerStatus = PowerStatus(OptUnknown, OptUnknown, OptUnknown, -1, -1);
        newStatus.notifyObservers(&powerStatus);
    }
}

int32_t Power::runOnce()
{
    readPowerStatus();

#ifdef TBEAM_V10
    // WE no longer use the IRQ line to wake the CPU (due to false wakes from sleep), but we do poll
    // the IRQ status by reading the registers over I2C
    axp.readIRQ();

    if (axp.isVbusRemoveIRQ()) {
        DEBUG_MSG("USB unplugged\n");
        powerFSM.trigger(EVENT_POWER_DISCONNECTED);
    }
    if (axp.isVbusPlugInIRQ()) {
        DEBUG_MSG("USB plugged In\n");
        powerFSM.trigger(EVENT_POWER_CONNECTED);
    }
    /*
    Other things we could check if we cared...

    if (axp.isChargingIRQ()) {
        DEBUG_MSG("Battery start charging\n");
    }
    if (axp.isChargingDoneIRQ()) {
        DEBUG_MSG("Battery fully charged\n");
    }
    if (axp.isBattPlugInIRQ()) {
        DEBUG_MSG("Battery inserted\n");
    }
    if (axp.isBattRemoveIRQ()) {
        DEBUG_MSG("Battery removed\n");
    }
    if (axp.isPEKShortPressIRQ()) {
        DEBUG_MSG("PEK short button press\n");
    }
    */
    axp.clearIRQ();
#endif

    // Only read once every 20 seconds once the power status for the app has been initialized
    return (statusHandler && statusHandler->isInitialized()) ? (1000 * 20) : RUN_SAME;
}

/**
 * Init the power manager chip
 *
 * axp192 power
    DCDC1 0.7-3.5V @ 1200mA max -> OLED // If you turn this off you'll lose comms to the axp192 because the OLED and the axp192
 share the same i2c bus, instead use ssd1306 sleep mode DCDC2 -> unused DCDC3 0.7-3.5V @ 700mA max -> ESP32 (keep this on!) LDO1
 30mA -> charges GPS backup battery // charges the tiny J13 battery by the GPS to power the GPS ram (for a couple of days), can
 not be turned off LDO2 200mA -> LORA LDO3 200mA -> GPS
 */
bool Power::axp192Init()
{
#ifdef TBEAM_V10
    if (axp192_found) {
        if (!axp.begin(Wire, AXP192_SLAVE_ADDRESS)) {
            batteryLevel = &axp;

            DEBUG_MSG("AXP192 Begin PASS\n");

            // axp.setChgLEDMode(LED_BLINK_4HZ);
            DEBUG_MSG("DCDC1: %s\n", axp.isDCDC1Enable() ? "ENABLE" : "DISABLE");
            DEBUG_MSG("DCDC2: %s\n", axp.isDCDC2Enable() ? "ENABLE" : "DISABLE");
            DEBUG_MSG("LDO2: %s\n", axp.isLDO2Enable() ? "ENABLE" : "DISABLE");
            DEBUG_MSG("LDO3: %s\n", axp.isLDO3Enable() ? "ENABLE" : "DISABLE");
            DEBUG_MSG("DCDC3: %s\n", axp.isDCDC3Enable() ? "ENABLE" : "DISABLE");
            DEBUG_MSG("Exten: %s\n", axp.isExtenEnable() ? "ENABLE" : "DISABLE");
            DEBUG_MSG("----------------------------------------\n");

            axp.setPowerOutPut(AXP192_LDO2, AXP202_ON); // LORA radio
            // axp.setPowerOutPut(AXP192_LDO3, AXP202_ON); // GPS main power - now turned on in setGpsPower
            axp.setPowerOutPut(AXP192_DCDC2, AXP202_ON);
            axp.setPowerOutPut(AXP192_EXTEN, AXP202_ON);
            axp.setPowerOutPut(AXP192_DCDC1, AXP202_ON);
            axp.setDCDC1Voltage(3300); // for the OLED power

            DEBUG_MSG("DCDC1: %s\n", axp.isDCDC1Enable() ? "ENABLE" : "DISABLE");
            DEBUG_MSG("DCDC2: %s\n", axp.isDCDC2Enable() ? "ENABLE" : "DISABLE");
            DEBUG_MSG("LDO2: %s\n", axp.isLDO2Enable() ? "ENABLE" : "DISABLE");
            DEBUG_MSG("LDO3: %s\n", axp.isLDO3Enable() ? "ENABLE" : "DISABLE");
            DEBUG_MSG("DCDC3: %s\n", axp.isDCDC3Enable() ? "ENABLE" : "DISABLE");
            DEBUG_MSG("Exten: %s\n", axp.isExtenEnable() ? "ENABLE" : "DISABLE");

            if (radioConfig.preferences.charge_current == ChargeCurrent_MAUnset) {
                axp.setChargeControlCur(AXP1XX_CHARGE_CUR_450MA);
            } else if (radioConfig.preferences.charge_current == ChargeCurrent_MA100) {
                axp.setChargeControlCur(AXP1XX_CHARGE_CUR_100MA);
            } else if (radioConfig.preferences.charge_current == ChargeCurrent_MA190) {
                axp.setChargeControlCur(AXP1XX_CHARGE_CUR_190MA);
            } else if (radioConfig.preferences.charge_current == ChargeCurrent_MA280) {
                axp.setChargeControlCur(AXP1XX_CHARGE_CUR_280MA);
            } else if (radioConfig.preferences.charge_current == ChargeCurrent_MA360) {
                axp.setChargeControlCur(AXP1XX_CHARGE_CUR_360MA);
            } else if (radioConfig.preferences.charge_current == ChargeCurrent_MA450) {
                axp.setChargeControlCur(AXP1XX_CHARGE_CUR_450MA);
            } else if (radioConfig.preferences.charge_current == ChargeCurrent_MA550) {
                axp.setChargeControlCur(AXP1XX_CHARGE_CUR_550MA);
            } else if (radioConfig.preferences.charge_current == ChargeCurrent_MA630) {
                axp.setChargeControlCur(AXP1XX_CHARGE_CUR_630MA);
            } else if (radioConfig.preferences.charge_current == ChargeCurrent_MA700) {
                axp.setChargeControlCur(AXP1XX_CHARGE_CUR_700MA);
            } else if (radioConfig.preferences.charge_current == ChargeCurrent_MA780) {
                axp.setChargeControlCur(AXP1XX_CHARGE_CUR_780MA);
            } else if (radioConfig.preferences.charge_current == ChargeCurrent_MA880) {
                axp.setChargeControlCur(AXP1XX_CHARGE_CUR_880MA);
            } else if (radioConfig.preferences.charge_current == ChargeCurrent_MA960) {
                axp.setChargeControlCur(AXP1XX_CHARGE_CUR_960MA);
            } else if (radioConfig.preferences.charge_current == ChargeCurrent_MA1000) {
                axp.setChargeControlCur(AXP1XX_CHARGE_CUR_1000MA);
            } else if (radioConfig.preferences.charge_current == ChargeCurrent_MA1080) {
                axp.setChargeControlCur(AXP1XX_CHARGE_CUR_1080MA);
            } else if (radioConfig.preferences.charge_current == ChargeCurrent_MA1160) {
                axp.setChargeControlCur(AXP1XX_CHARGE_CUR_1160MA);
            } else if (radioConfig.preferences.charge_current == ChargeCurrent_MA1240) {
                axp.setChargeControlCur(AXP1XX_CHARGE_CUR_1240MA);
            } else if (radioConfig.preferences.charge_current == ChargeCurrent_MA1320) {
                axp.setChargeControlCur(AXP1XX_CHARGE_CUR_1320MA);
            }

#if 0

      // Not connected
      //val = 0xfc;
      //axp._writeByte(AXP202_VHTF_CHGSET, 1, &val); // Set temperature protection

      //not used
      //val = 0x46;
      //axp._writeByte(AXP202_OFF_CTL, 1, &val); // enable bat detection
#endif
            axp.debugCharging();

#ifdef PMU_IRQ
            pinMode(PMU_IRQ, INPUT);
            attachInterrupt(
                PMU_IRQ, [] { pmu_irq = true; }, FALLING);

            axp.adc1Enable(AXP202_BATT_CUR_ADC1, 1);
            // we do not look for AXP202_CHARGING_FINISHED_IRQ & AXP202_CHARGING_IRQ because it occurs repeatedly while there is
            // no battery also it could cause inadvertent waking from light sleep just because the battery filled
            // we don't look for AXP202_BATT_REMOVED_IRQ because it occurs repeatedly while no battery installed
            // we don't look at AXP202_VBUS_REMOVED_IRQ because we don't have anything hooked to vbus
            axp.enableIRQ(AXP202_BATT_CONNECT_IRQ | AXP202_VBUS_CONNECT_IRQ | AXP202_PEK_SHORTPRESS_IRQ, 1);

            axp.clearIRQ();
#endif
            readPowerStatus();
        } else {
            DEBUG_MSG("AXP192 Begin FAIL\n");
        }
    } else {
        DEBUG_MSG("AXP192 not found\n");
    }

    return axp192_found;
#else
    return false;
#endif
}
