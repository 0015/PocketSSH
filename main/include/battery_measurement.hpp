#ifndef BATTERY_MEASUREMENT_HPP
#define BATTERY_MEASUREMENT_HPP

#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include <vector>

class BatteryMeasurement
{
public:
    BatteryMeasurement();
    ~BatteryMeasurement();

    // Initializes the battery measurement with delayed ADC init
    esp_err_t init();

    // Reads the battery voltage
    float readBatteryVoltage();

    // Converts the battery voltage to percentage
    int voltageToPercentage(float voltage);

    // Deinitializes
    void deinit();

private:
    struct BatteryLevel
    {
        float voltage;  // Battery voltage in volts
        int percentage; // Corresponding battery percentage
    };

    static constexpr const char *TAG = "BATTERY_MEASUREMENT";
    static constexpr int BAT_ADC_PIN = 4; // GPIO4 for battery ADC
    static constexpr adc_channel_t ADC_CHANNEL = ADC_CHANNEL_3; // GPIO4 = ADC1_CH3
    static constexpr adc_unit_t ADC_UNIT = ADC_UNIT_1; // Use ADC1 instead of ADC2
    static constexpr adc_atten_t ADC_ATTEN = ADC_ATTEN_DB_12; // 12dB attenuation
    static constexpr adc_bitwidth_t ADC_BITWIDTH = ADC_BITWIDTH_12; // 12-bit resolution
    static constexpr float DIVIDER_RATIO = 2.0f; // Voltage divider correction

    static const std::vector<BatteryLevel> batteryCurve;

    adc_oneshot_unit_handle_t adcHandle;
    adc_cali_handle_t caliHandle;
    bool calibrationEnabled;

    // Interpolates the voltage-to-percentage using the lookup table
    int interpolateVoltage(float voltage);
};

#endif // BATTERY_MEASUREMENT_HPP