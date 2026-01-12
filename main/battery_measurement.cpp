#include "battery_measurement.hpp"

const std::vector<BatteryMeasurement::BatteryLevel> BatteryMeasurement::batteryCurve = {
    {4.20f, 100},
    {4.00f, 90},
    {3.85f, 75},
    {3.70f, 50},
    {3.60f, 25},
    {3.50f, 10},
    {3.30f, 0},
};

BatteryMeasurement::BatteryMeasurement()
    : adcHandle(nullptr), caliHandle(nullptr), calibrationEnabled(false) {}

BatteryMeasurement::~BatteryMeasurement()
{
    deinit();
}

esp_err_t BatteryMeasurement::init()
{
    esp_err_t ret;

    // Using ADC1_CH3 (GPIO4) instead of ADC2 to avoid conflicts with WiFi
    adc_oneshot_unit_init_cfg_t initConfig = {
        .unit_id = ADC_UNIT,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ret = adc_oneshot_new_unit(&initConfig, &adcHandle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize ADC unit: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure ADC channel
    adc_oneshot_chan_cfg_t channelConfig = {
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH,
    };
    ret = adc_oneshot_config_channel(adcHandle, ADC_CHANNEL, &channelConfig);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure ADC channel: %s", esp_err_to_name(ret));
        adc_oneshot_del_unit(adcHandle);
        return ret;
    }

    // Initialize calibration
    adc_cali_curve_fitting_config_t caliConfig = {
        .unit_id = ADC_UNIT,
        .chan = ADC_CHANNEL,
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH,
    };
    if (adc_cali_create_scheme_curve_fitting(&caliConfig, &caliHandle) == ESP_OK)
    {
        calibrationEnabled = true;
        ESP_LOGI(TAG, "ADC calibration enabled using curve fitting (ADC1_CH3/GPIO4)");
    }
    else
    {
        ESP_LOGW(TAG, "ADC calibration not supported, using raw values");
    }

    ESP_LOGI(TAG, "Battery measurement initialized (ADC1_CH3/GPIO4)");
    return ESP_OK;
}

float BatteryMeasurement::readBatteryVoltage()
{
    int rawReading = 0;
    int voltage_mv = 0;

    // Read raw ADC value
    esp_err_t ret = adc_oneshot_read(adcHandle, ADC_CHANNEL, &rawReading);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "ADC read failed: %s", esp_err_to_name(ret));
        return 0.0f;
    }

    // Convert to millivolts
    if (calibrationEnabled)
    {
        adc_cali_raw_to_voltage(caliHandle, rawReading, &voltage_mv);
    }
    else
    {
        // Fallback calculation without calibration
        voltage_mv = (rawReading * 3300) / 4095; // Approximate for 12-bit ADC
    }

    // Apply voltage divider correction (GPIO4 has 2x divider)
    float batteryVoltage = (voltage_mv * DIVIDER_RATIO) / 1000.0f;
    
    ESP_LOGI(TAG, "ADC Raw: %d, Voltage: %d mV, Battery: %.2f V", rawReading, voltage_mv, batteryVoltage);
    
    return batteryVoltage;
}

int BatteryMeasurement::voltageToPercentage(float voltage)
{
    if (voltage >= batteryCurve.front().voltage)
    {
        return 100; // Above max voltage
    }
    if (voltage <= batteryCurve.back().voltage)
    {
        return 0; // Below min voltage
    }
    return interpolateVoltage(voltage);
}

int BatteryMeasurement::interpolateVoltage(float voltage)
{
    for (size_t i = 0; i < batteryCurve.size() - 1; ++i)
    {
        const auto &p1 = batteryCurve[i];
        const auto &p2 = batteryCurve[i + 1];
        if (voltage <= p1.voltage && voltage > p2.voltage)
        {
            // Linear interpolation
            return p1.percentage + static_cast<int>((voltage - p1.voltage) * (p2.percentage - p1.percentage) / (p2.voltage - p1.voltage));
        }
    }
    // Should never reach here, but return 0 for safety
    ESP_LOGW(TAG, "Voltage interpolation fallback");
    return 0;
}

void BatteryMeasurement::deinit()
{
    if (calibrationEnabled && caliHandle != nullptr)
    {
        adc_cali_delete_scheme_curve_fitting(caliHandle);
        caliHandle = nullptr;
    }
    if (adcHandle != nullptr)
    {
        adc_oneshot_del_unit(adcHandle);
        adcHandle = nullptr;
    }
    ESP_LOGI(TAG, "Battery measurement deinitialized");
}
