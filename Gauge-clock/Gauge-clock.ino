#include <Arduino.h>
#include "Wire.h"
#include <Adafruit_MCP4728.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_AHTX0.h>
#include <DS3231.h>

static const int SDA_PIN = 8;
static const int SCL_PIN = 9;

Adafruit_BMP280 bmp; // I2C
Adafruit_AHTX0 aht20;
DS3231 myRTC;

const MCP4728_channel_t gauge_1 = MCP4728_CHANNEL_C;  // Upper gauge (Hours)
const MCP4728_channel_t gauge_2 = MCP4728_CHANNEL_A;  // Mid gauge (Minutes)
const MCP4728_channel_t gauge_3 = MCP4728_CHANNEL_B;  // Lowest gague (Seconds, Temp, Pressure, Humidity)

// Our fake TZ.
// It does not relly work in ESP32 environment, but still needed for the standard
// functions as a parameter.
struct timezone tz = {0, 0};

class GaugeDisplay {
  Adafruit_MCP4728 mcp;

  int low_value[4];
  int higher_value[4];
  int correction[4] = {4096, 4096, 4096, 4096};
  int current_value[4];

public:
  bool begin() {
    if (!mcp.begin(0x64)) {
      return false;
    }

#if 0
    mcp.setChannelValue(MCP4728_CHANNEL_A, 1000, MCP4728_VREF_INTERNAL);
    mcp.setChannelValue(MCP4728_CHANNEL_B, 200, MCP4728_VREF_INTERNAL);
    mcp.setChannelValue(MCP4728_CHANNEL_C, 300, MCP4728_VREF_INTERNAL);
    mcp.setChannelValue(MCP4728_CHANNEL_D, 4000, MCP4728_VREF_INTERNAL);
#endif
    mcp.saveToEEPROM();
    return true;
  }

  void set_range(int g, int low, int high)
  {
    low_value[g] = low;
    higher_value[g] = high;
  }

  void set_correction(int gauge_number, int correction = 4096)
  {
    this->correction[gauge_number] = correction;
  }

  int set_value(MCP4728_channel_t g, float v)
  {
    // Calculate a new value to set.
    double v0 = (v-low_value[g])/(higher_value[g]-low_value[g]);
    int count = v0*correction[g];
    const int step = 3;

    if (count<0) {
      count = 0;
    } else if (count>=4094) {
       count = 4095;
    }

    while (count+step>=current_value[g]) {
      current_value[g] += step;
      mcp.setChannelValue(g, current_value[g], MCP4728_VREF_INTERNAL);
      delay(1);
    }
  
    while (count<current_value[g]-step) {
      current_value[g] -= step;
      mcp.setChannelValue(g, current_value[g], MCP4728_VREF_INTERNAL);
      delay(1);
    }
  
    mcp.setChannelValue(g, count, MCP4728_VREF_INTERNAL);
    current_value[g] = count;

    return count;
  }
};

GaugeDisplay disp;

void set_clock_time(unsigned int h, unsigned int m, unsigned int s)
{
  log_printf("set time: %02u:%02u:%02u\n", h, m, s);

  // Check time sanity. Uninitialized RTC might give strange values.
  if (h<24 && m<60 && s<60) {
    struct timeval tv = {0};
    tv.tv_sec = h*60*60+m*60+s;
    // Set current time
    settimeofday(&tv, &tz);
  }
}

void get_time_from_rtc()
{
  bool h12Flag;
  bool pmFlag;
  unsigned int h = myRTC.getHour(h12Flag, pmFlag);
  unsigned int m = myRTC.getMinute();
  unsigned int s = myRTC.getSecond();

  log_printf("set time from RTC: %02u:%02u:%02u\n", h, m, s);

  set_clock_time(h, m, s);
}

void setup() {
  delay(1000);
  Wire.begin(SDA_PIN, SCL_PIN);

  log_printf("Setup start\n");

  // Set time from RTC
  get_time_from_rtc();

  // Try to initialize!
  if (!disp.begin()) {
    Serial.println("Failed to find MCP4728 chip");
    while (1) {
      delay(10);
    }
  }

  disp.set_range(gauge_1, 0, 12); // Scale 12 hours
  disp.set_range(gauge_2, 0, 60); // Scale 60 minutes
  disp.set_range(gauge_3, 0, 60); // Scale 60 seconds

  if (!aht20.begin()) {
    Serial.println("AHT20 not detected. Please check wiring. Freezing.");
    while(true);
  }

  if (!bmp.begin()) {
    Serial.println("BMP not detected. Please check wiring. Freezing.");
    while(true); 
  }

  log_printf("DAC enabled\n");

  disp.set_correction(gauge_1, 3700);
  disp.set_correction(gauge_2, 3450);
  disp.set_correction(gauge_3, 4096);
}

void loop()
{
  static int count = 0;
  struct timeval tv;
  gettimeofday(&tv, &tz);
  tm *ttm = localtime(&tv.tv_sec);

  double s = (ttm->tm_sec%60)+double(tv.tv_usec)/1000000;
  double m = ttm->tm_min+s/60;
  double h = (ttm->tm_hour%12)+m/60;

  disp.set_value(gauge_1, h);
  disp.set_value(gauge_2, m);
  disp.set_value(gauge_3, s);
  delay(100);

  if (count++ == 100) {
    sensors_event_t humidity, temp;
    aht20.getEvent(&humidity, &temp); // populate temp and humidity objects with fresh data
    log_printf("T: %f\n", temp.temperature);
    log_printf("H: %f\n", humidity.relative_humidity);

    float temperature = bmp.readTemperature();
    float pressure = bmp.readPressure();

    log_printf("T: %f\n", temperature);
    log_printf("P: %f\n", pressure);
    count = 0;
    log_printf("Time: %f %f %f\n", h, m, s);
  }
}
