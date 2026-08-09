#include <Arduino.h>
#include "Wire.h"
#include <Adafruit_MCP4728.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_AHTX0.h>

static const int SDA_PIN = 8;
static const int SCL_PIN = 9;

Adafruit_BMP280 bmp; // I2C
Adafruit_AHTX0 aht20;

class VoltmeterDisplay {
  Adafruit_MCP4728 mcp;

  int low_value[3];
  int higher_value[3];
  int correction[3];
  int current_value[3];

public:
  bool begin() {
    if (!mcp.begin(0x64)) {
      return false;
    }

    mcp.setChannelValue(MCP4728_CHANNEL_A, 1024, MCP4728_VREF_INTERNAL);
    mcp.setChannelValue(MCP4728_CHANNEL_B, 2048, MCP4728_VREF_INTERNAL);
    mcp.setChannelValue(MCP4728_CHANNEL_C, 1024, MCP4728_VREF_INTERNAL);
    mcp.setChannelValue(MCP4728_CHANNEL_D, 0, MCP4728_VREF_INTERNAL);
    mcp.saveToEEPROM();
    return true;
  }

  void set_range(MCP4728_channel_t d, int low, int high)
  {
    low_value[d] = low;
    higher_value[d] = high;
  }

  int set_value(MCP4728_channel_t d, float v)
  {
    // Calculate a new value to set.
    double v0 = (v-low_value[d])/(higher_value[d]-low_value[d]);
    int count = v0*correction[d];
    if (count<0) {
      count = 0;
    } else if (count>4094) {
      count = 4096;
    }

    // Old value was high, and the new value is low.    
    if (current_value[d] > 3000 && count < 10) {
      // Set the new value and let some time for the voltmeter hand to
      // travel to the new value.
      mcp.setChannelValue(d, count, MCP4728_VREF_INTERNAL);
      delay(100);

      // Set higher current impuls to brake the hand, that had got some momentum.
      mcp.setChannelValue(d, 4000, MCP4728_VREF_INTERNAL);
      delay(3);
    }

    mcp.setChannelValue(d, count, MCP4728_VREF_INTERNAL);
    current_value[d] = count;

    return count;
  }
};

VoltmeterDisplay disp;

void setup() {
  delay(1000);
  Wire.begin(SDA_PIN, SCL_PIN);

  log_printf("Setup start\n");

  // Try to initialize!
  if (!disp.begin()) {
    Serial.println("Failed to find MCP4728 chip");
    while (1) {
      delay(10);
    }
  }

  disp.set_range(MCP4728_CHANNEL_A, 0, 4096);
  disp.set_range(MCP4728_CHANNEL_B, 0, 4096);
  disp.set_range(MCP4728_CHANNEL_C, 0, 4096);

  if (!aht20.begin()) {
    Serial.println("AHT20 not detected. Please check wiring. Freezing.");
    while(true);
  }

  if (!bmp.begin()) {
    Serial.println("BMP not detected. Please check wiring. Freezing.");
    while(true); 
  }

  log_printf("DAC enabled\n");
}

void loop()
{
  static int count = 0;
  if (++count > 4000) { 
    for (; count>0; count-=3) {
        disp.set_value(MCP4728_CHANNEL_A, count);
        delay(1);
    }
    sensors_event_t humidity, temp;
    aht20.getEvent(&humidity, &temp); // populate temp and humidity objects with fresh data
    log_printf("T: %f\n", temp.temperature);
    log_printf("H: %f\n", humidity.relative_humidity);

    float temperature = bmp.readTemperature();
    float pressure = bmp.readPressure();

    log_printf("T: %f\n", temperature);
    log_printf("P: %f\n", pressure);
  }

  delay(10);
}
