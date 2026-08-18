#include <Arduino.h>
#include "Wire.h"
#include <Adafruit_MCP4728.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_AHTX0.h>
#include <DS3231.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <TimeLib.h>
#include <WiFiUdp.h>
#include <GyverPortal.h>
#include <Adafruit_NeoPixel.h>

static const int SDA_PIN = 8;
static const int SCL_PIN = 9;

static const int LED_PIN = 20;
static const int LED_COUNT = 10;

#define WIRE_SPEED 100000

// NTP update interval in seconds
const int NTP_UPDATE_INTERVAL = 3000;

// If we do not have WiFi we wait 60 seconds in the configuration portal.
const int WIFI_MANAGER_TIMEOUT = 60;

// Clock EEPROM data address.
const int eeprom_addr = 0;

Adafruit_BMP280 bmp; // I2C
Adafruit_AHTX0 aht20;
DS3231 myRTC;

// Web configuration UI
GyverPortal ui;

Adafruit_NeoPixel leds(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// LED strip brightness (0-255), persisted to EEPROM and settable via the web UI.
unsigned char led_brightness = 128;

const int LED_ORANGE_COUNT = 6; // Number of LEDs lit by leds_on_orange()

// Turn all LEDs off.
void leds_off()
{
  leds.clear();
  leds.show();
}

// Turn the first LED_ORANGE_COUNT LEDs on in orange, at led_brightness.
void leds_on_orange()
{
  leds.setBrightness(led_brightness);
  leds.clear();
  for (int i = LED_COUNT-LED_ORANGE_COUNT; i < LED_COUNT; i++) {
    leds.setPixelColor(i, leds.Color(255, 80, 0));
  }
  leds.show();
}

// Clock global configuration (persisted to EEPROM).
char ntpServerName[80] = "fi.pool.ntp.org";
signed char clock_tz = 2; // Timezone shift (could be negative)
unsigned char clock_use_ntp = true;  // Use NTP switch
unsigned char clock_use_rtc = true;  // Use RTC switch

// What gauge_3 displays (persisted to EEPROM), selectable via the web UI.
enum Gauge3Mode {
  GAUGE3_SECONDS = 0,
  GAUGE3_TEMPERATURE = 1,
  GAUGE3_HUMIDITY = 2,
  GAUGE3_PRESSURE = 3,
};
unsigned char gauge_3_mode = GAUGE3_SECONDS;

// Per-gauge needle corrections (persisted to EEPROM), passed to
// GaugeDisplay::set_correction(). Compensate for mechanical differences
// between individual gauges. Defaults match the original hard-coded values.
int gauge_correction_1 = 3700;
int gauge_correction_2 = 3450;
int gauge_correction_3 = 4096;

// MCP4728 constants --------------------------------------------------------
static const int GAUGE_MAX = 4095; // Max value accepted by External DAC

const MCP4728_channel_t gauge_1 = MCP4728_CHANNEL_C;  // Upper gauge (Hours)
const MCP4728_channel_t gauge_2 = MCP4728_CHANNEL_A;  // Mid gauge (Minutes)
const MCP4728_channel_t gauge_3 = MCP4728_CHANNEL_B;  // Lowest gauge (Seconds, Temp, Pressure, Humidity)

// EEPROM address of the gauge corrections, placed right after the NTP
// server name field written by read/write_eeprom_data().
const int eeprom_correction_addr = eeprom_addr + 3 + sizeof(ntpServerName);

// EEPROM address of the LED brightness, placed right after the gauge
// corrections written by read/write_eeprom_data().
const int eeprom_led_brightness_addr = eeprom_correction_addr + 3*sizeof(gauge_correction_1);

// EEPROM address of the gauge_3 display mode, placed right after the LED
// brightness written by read/write_eeprom_data().
const int eeprom_gauge_3_mode_addr = eeprom_led_brightness_addr + sizeof(led_brightness);

// Indicator LEDs, one per gauge_3 mode, showing which value gauge_3 currently displays.
const int LED_GAUGE3_SECONDS = 0;
const int LED_GAUGE3_HUMIDITY = 1;
const int LED_GAUGE3_PRESSURE = 2;
const int LED_GAUGE3_TEMPERATURE = 3;

// Light the LED for the current gauge_3_mode white, at led_brightness. Leaves
// other LEDs untouched, so call after leds_on_orange(), which clears the strip.
void leds_indicate_gauge3_mode()
{
  int idx;
  switch (gauge_3_mode) {
    case GAUGE3_HUMIDITY:    idx = LED_GAUGE3_HUMIDITY;    break;
    case GAUGE3_PRESSURE:    idx = LED_GAUGE3_PRESSURE;    break;
    case GAUGE3_TEMPERATURE: idx = LED_GAUGE3_TEMPERATURE; break;
    case GAUGE3_SECONDS:
    default:                 idx = LED_GAUGE3_SECONDS;     break;
  }
  leds.setBrightness(led_brightness);
  leds.setPixelColor(idx, leds.Color(255, 255, 255));
  leds.show();
}

// Our fake TZ.
// It does not really work in ESP32 environment, but still needed for the standard
// functions as a parameter.
struct timezone tz = {0, 0};

class GaugeDisplay {
  Adafruit_MCP4728 mcp;

  int low_value[4];
  int higher_value[4];
  int correction[4] = {GAUGE_MAX+1, GAUGE_MAX+1, GAUGE_MAX+1, GAUGE_MAX+1};
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

  void set_correction(int gauge_number, int correction = GAUGE_MAX+1)
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
    } else if (count>=GAUGE_MAX) {
      count = GAUGE_MAX;
    }

    // ramp up
    while (current_value[g] + step <= count) {
      current_value[g] += step;
      mcp.setChannelValue(g, current_value[g], MCP4728_VREF_INTERNAL);
      delay(1);
    }
  
    // ramp down
    while (current_value[g] - step >= count) {
      current_value[g] -= step;
      mcp.setChannelValue(g, current_value[g], MCP4728_VREF_INTERNAL);
      delay(1);
    }
  
    // final set to exact value
    current_value[g] = count;
    mcp.setChannelValue(g, current_value[g], MCP4728_VREF_INTERNAL);

    return count;
  }
};

GaugeDisplay disp;

// Latest sensor readings, refreshed by read_sensors() and used to drive
// gauge_3 when it is not showing seconds.
float sensor_temperature = 0; // Celsius, from the AHT20
float sensor_humidity = 0;    // %RH, from the AHT20
float sensor_pressure = 0;    // mmHg, from the BMP280

// Set the value range for gauge_3 to match what it currently displays.
void set_gauge3_range()
{
  switch (gauge_3_mode) {
    case GAUGE3_TEMPERATURE:
      disp.set_range(gauge_3, 15, 35); // Celsius
      break;
    case GAUGE3_HUMIDITY:
      disp.set_range(gauge_3, 0, 100); // %RH
      break;
    case GAUGE3_PRESSURE:
      disp.set_range(gauge_3, 735, 780); // mmHg
      break;
    case GAUGE3_SECONDS:
    default:
      disp.set_range(gauge_3, 0, 60);
      break;
  }
}

void set_clock_time(unsigned int h, unsigned int m, unsigned int s)
{
  log_printf("set time: %02u:%02u:%02u\n", h, m, s);

  // Check time values sanity. Uninitialized RTC or broken GPS read
  // seen to produce nonsense time.
  if (h >= 24 || m >= 60 || s >= 60) {
    return;
  }

  time_t now = time(nullptr);
  struct tm tm_now;
  if (localtime_r(&now, &tm_now) == nullptr) {
     log_printf("mktime() failed when reding time\n");
     return;
  }
  tm_now.tm_hour = h;
  tm_now.tm_min  = m;
  tm_now.tm_sec  = s;
  time_t newt = mktime(&tm_now);
  if (newt == (time_t)-1) {
    log_printf("mktime() failed when setting time\n");
    return;
  }
  struct timeval tv;
  tv.tv_sec = newt;
  tv.tv_usec = 0;
  settimeofday(&tv, &tz);
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

// Initialize the network (Wi-Fi connection).
bool initialize_network()
{
  WiFiManager wm;
  // wm.resetSettings();

  // Automatically connect using saved credentials,
  // if connection fails, it starts an access point with the name "GaugeClock".
  wm.setConfigPortalTimeout(WIFI_MANAGER_TIMEOUT);
  bool res = wm.autoConnect("GaugeClock");
  wm.stopWebPortal();

  return res;
}

// Read the config data from EEPROM.
void read_eeprom_data()
{
  clock_tz = (signed char)EEPROM.read(eeprom_addr);
  clock_use_ntp = EEPROM.read(eeprom_addr+1);
  clock_use_rtc = EEPROM.read(eeprom_addr+2);
  EEPROM.readString(eeprom_addr+3, ntpServerName, sizeof(ntpServerName)-1);

  int c1, c2, c3;
  EEPROM.get(eeprom_correction_addr, c1);
  EEPROM.get(eeprom_correction_addr+sizeof(c1), c2);
  EEPROM.get(eeprom_correction_addr+2*sizeof(c1), c3);

  // Corrections must be positive and fit the 12-bit DAC range; reject
  // uninitialized (erased) EEPROM contents and keep the code defaults.
  if (c1>0 && c1<=GAUGE_MAX) gauge_correction_1 = c1;
  if (c2>0 && c2<=GAUGE_MAX) gauge_correction_2 = c2;
  if (c3>0 && c3<=GAUGE_MAX) gauge_correction_3 = c3;

  led_brightness = EEPROM.read(eeprom_led_brightness_addr);

  unsigned char m = EEPROM.read(eeprom_gauge_3_mode_addr);
  if (m<=GAUGE3_PRESSURE) gauge_3_mode = m;

  EEPROM.commit();
}

// Write the config data to EEPROM.
void write_eeprom_data()
{
  EEPROM.write(eeprom_addr, clock_tz);
  EEPROM.write(eeprom_addr+1, clock_use_ntp);
  EEPROM.write(eeprom_addr+2, clock_use_rtc);
  EEPROM.writeString(eeprom_addr+3, ntpServerName);

  EEPROM.put(eeprom_correction_addr, gauge_correction_1);
  EEPROM.put(eeprom_correction_addr+sizeof(gauge_correction_1), gauge_correction_2);
  EEPROM.put(eeprom_correction_addr+2*sizeof(gauge_correction_1), gauge_correction_3);

  EEPROM.write(eeprom_led_brightness_addr, led_brightness);

  EEPROM.write(eeprom_gauge_3_mode_addr, gauge_3_mode);

  EEPROM.commit();
}

// NTP support code (adapted from TimeNTP sample)
WiFiUDP Udp;
unsigned int localPort = 8888;  // local port to listen for UDP packets
const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets

// Send an NTP request to the time server at the given address
void sendNTPpacket(IPAddress &address)
{
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  Udp.beginPacket(address, 123); //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}

// Get current time from NTP server
time_t getNtpTime()
{
  IPAddress ntpServerIP; // NTP server's IP address

  while (Udp.parsePacket() > 0) ; // discard any previously received packets
  WiFi.hostByName(ntpServerName, ntpServerIP);
  log_printf("Transmit NTP Request: %s\n", ntpServerName);
  sendNTPpacket(ntpServerIP);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = Udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
      time_t secsSince1900;
      // convert four bytes starting at location 40 to a long integer
      secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];
      log_printf("Receive NTP Response %lu\n", (unsigned long)secsSince1900);

      // Convert NTP time to UNIX time and apply timezone offset
      time_t secsSinceEpoch = secsSince1900 - 2208988800UL + clock_tz * SECS_PER_HOUR;

      // Update RTC and system time with received NTP time
      myRTC.setEpoch(secsSinceEpoch, true);

      tm *ttm = localtime(&secsSinceEpoch);
      set_clock_time(ttm->tm_hour, ttm->tm_min, ttm->tm_sec);

      return secsSinceEpoch;
    }
  }
  log_printf("No NTP Response :-(\n");
  return 0;
}

// Create a configuration form for the web UI
void build()
{
  log_printf("BUILD\n");

  GP.BUILD_BEGIN();
  GP.PAGE_TITLE("Gauge clock");

  GP.THEME(GP_DARK);
  GP.FORM_BEGIN("/update");

  GP_MAKE_BLOCK_TAB(
    "Clock config",
    GP_MAKE_BOX(GP.LABEL("TimeZone shift:"); GP.NUMBER("clock_tz", "", clock_tz););
    GP_MAKE_BOX(GP.LABEL("Use NTP"); GP.SWITCH("clock_use_ntp", clock_use_ntp ? true: false););
    GP_MAKE_BOX(GP.LABEL("Use RTC"); GP.SWITCH("clock_use_rtc", clock_use_rtc ? true: false, 0););
    GP_MAKE_BOX(GP.LABEL("NTP Server name: "); GP.TEXT("clock_ntp_server", "local NTP server if you have", ntpServerName, "", sizeof(ntpServerName)-1););
  );

  GP_MAKE_BLOCK_TAB(
    "Gauge corrections",
    GP_MAKE_BOX(GP.LABEL("Gauge 1 (Hours):"); GP.NUMBER("gauge_correction_1", "", gauge_correction_1););
    GP_MAKE_BOX(GP.LABEL("Gauge 2 (Minutes):"); GP.NUMBER("gauge_correction_2", "", gauge_correction_2););
    GP_MAKE_BOX(GP.LABEL("Gauge 3:"); GP.NUMBER("gauge_correction_3", "", gauge_correction_3););
    GP_MAKE_BOX(GP.LABEL("Gauge 3 shows:"); GP.SELECT("gauge_3_mode", "Seconds,Temperature,Humidity,Pressure", gauge_3_mode););
  );

  GP_MAKE_BLOCK_TAB(
    "LEDs",
    GP_MAKE_BOX(GP.LABEL("LED brightness (0-255):"); GP.NUMBER("led_brightness", "", led_brightness););
  );

  GP.SUBMIT("UPDATE");

  GP.FORM_END();

  // Time setting form
  GP.FORM_BEGIN("/settime");

  // Get current time for the form
  time_t t = time(NULL);
  tm *ttm = localtime(&t);
  myRTC.setSecond(ttm->tm_sec);
  myRTC.setMinute(ttm->tm_min);
  myRTC.setHour(ttm->tm_hour);

  GPtime gptime (ttm->tm_hour, ttm->tm_min, ttm->tm_sec);

  GP_MAKE_BLOCK_TAB(
    "Time",
    GP_MAKE_BOX(GP.LABEL("Time :"); GP.TIME("time", gptime););
  );
  GP.SUBMIT("SET TIME");

  GP.FORM_END();

  GP.BUILD_END();
}

// Handle actions from GyverPortal web UI
void action(GyverPortal& p)
{
  log_printf("ACTION\n");

  // Handle config update form
  if (p.form("/update")) {
    int n;
    bool update_time = false;

    log_printf("ACTION update\n");

    // Read the new values, and check them for sanity.
    n = ui.getInt("clock_tz");
    if (n>=-12 && n<=12) {
      if (n!=clock_tz) {
        update_time = true;
      }
      clock_tz = n;
    }

    n = ui.getBool("clock_use_ntp");
    if (n>=0 && n<=1) {
      clock_use_ntp = n;
    }

    n = ui.getBool("clock_use_rtc");
    if (n>=0 && n<=1) {
      clock_use_rtc = n;
    }

    String s = ui.getString("clock_ntp_server");
    if (s && strcmp(s.c_str(), ntpServerName) != 0) {
      strncpy(ntpServerName, s.c_str(), sizeof(ntpServerName)-1);
    }

    // Read the new gauge corrections, and check them for sanity.
    n = ui.getInt("gauge_correction_1");
    if (n>0 && n<=GAUGE_MAX) {
      gauge_correction_1 = n;
    }

    n = ui.getInt("gauge_correction_2");
    if (n>0 && n<=GAUGE_MAX) {
      gauge_correction_2 = n;
    }

    n = ui.getInt("gauge_correction_3");
    if (n>0 && n<=GAUGE_MAX) {
      gauge_correction_3 = n;
    }

    n = ui.getInt("led_brightness");
    if (n>=0 && n<=255) {
      led_brightness = n;
    }

    n = ui.getInt("gauge_3_mode");
    if (n>=0 && n<=GAUGE3_PRESSURE) {
      gauge_3_mode = n;
    }

    disp.set_correction(gauge_1, gauge_correction_1);
    disp.set_correction(gauge_2, gauge_correction_2);
    disp.set_correction(gauge_3, gauge_correction_3);
    set_gauge3_range();

    leds_on_orange();
    leds_indicate_gauge3_mode();

    // Save new settings to EEPROM
    write_eeprom_data();

    // If timezone changed, update system time
    if (update_time) {
      if (clock_use_ntp) {
        getNtpTime();
      } else if (clock_use_rtc) {
        get_time_from_rtc();
      }
    }
  }

  // Handle time setting form
  if (p.form("/settime")) {
    GPtime gptime = ui.getTime("time");
    log_printf("Action Settime: %d:%02d:%02d\n", gptime.hour, gptime.minute, gptime.second);

    // Set time to RTC
    myRTC.setSecond(gptime.second);
    myRTC.setMinute(gptime.minute);
    myRTC.setHour(gptime.hour);

    set_clock_time(gptime.hour, gptime.minute, gptime.second);
  }
}

void setup()
{
  delay(1000);
  Wire.begin(SDA_PIN, SCL_PIN, WIRE_SPEED);

  log_printf("Setup start\n");

  EEPROM.begin(100);
  read_eeprom_data();

  leds.begin();
  leds_on_orange();
  leds_indicate_gauge3_mode();

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
  set_gauge3_range();

  if (!aht20.begin()) {
    Serial.println("AHT20 not detected. Please check wiring. Freezing.");
    while(true);
  }

  if (!bmp.begin()) {
    Serial.println("BMP not detected. Please check wiring. Freezing.");
    while(true); 
  }

  log_printf("DAC enabled\n");

  read_sensors(); // Populate initial sensor readings for gauge_3

  disp.set_correction(gauge_1, gauge_correction_1);
  disp.set_correction(gauge_2, gauge_correction_2);
  disp.set_correction(gauge_3, gauge_correction_3);

  // Initialize network and web configuration UI
  if (initialize_network()) {
    IPAddress myIP = WiFi.localIP();
    String ip_addr_str = myIP.toString();
    log_printf("AP IP address: %s\n", ip_addr_str.c_str());

    // Start server portal
    ui.attachBuild(build);
    ui.attach(action);
    ui.start();
    log_printf("setup ui started\n");

    // Fetch NTP time if enabled
    if (clock_use_ntp) {
      getNtpTime();
    }
  }
}

// Read the clock sensors.
void read_sensors()
{
  sensors_event_t humidity, temp;
  aht20.getEvent(&humidity, &temp); // populate temp and humidity objects with fresh data
  log_printf("ATH T: %f\n", temp.temperature);
  log_printf("H: %f\n", humidity.relative_humidity);

  sensor_temperature = temp.temperature;
  sensor_humidity = humidity.relative_humidity;
  sensor_pressure = bmp.readPressure() * 0.00750062; // Pa -> mmHg

  float temperature = bmp.readTemperature();

  log_printf("BMP T: %f\n", temperature);
  log_printf("P: %f\n", sensor_pressure);

  temperature = myRTC.getTemperature();
  bool h12, pm_time;
  log_printf("RTC T: %f\n", temperature);
  log_printf("RCT Time: %d:%02d:%02d\n", myRTC.getHour(h12, pm_time), myRTC.getMinute(), myRTC.getSecond());
}

void loop()
{
  struct timeval tv;
  gettimeofday(&tv, &tz);

  // Periodically resync from NTP, once per NTP_UPDATE_INTERVAL seconds.
  static time_t last_ntp_sync;
  if (clock_use_ntp && (tv.tv_sec - last_ntp_sync) >= NTP_UPDATE_INTERVAL) {
    last_ntp_sync = tv.tv_sec;
    getNtpTime();
  }

  // Web UI tick.
  ui.tick();

  // Read time and set the floating-point time values.
  tm *ttm = localtime(&tv.tv_sec);
  double s = (ttm->tm_sec%60)+double(tv.tv_usec)/1000000;
  double m = ttm->tm_min+s/60;
  double h = (ttm->tm_hour%12)+m/60;

  // Update the display.
  disp.set_value(gauge_1, h);
  disp.set_value(gauge_2, m);

  switch (gauge_3_mode) {
    case GAUGE3_TEMPERATURE:
      disp.set_value(gauge_3, sensor_temperature);
      break;
    case GAUGE3_HUMIDITY:
      disp.set_value(gauge_3, sensor_humidity);
      break;
    case GAUGE3_PRESSURE:
      disp.set_value(gauge_3, sensor_pressure);
      break;
    case GAUGE3_SECONDS:
    default:
      disp.set_value(gauge_3, s);
      break;
  }
  delay(100);

  static int count = 0;
  if (count++ == 100) {
    log_printf("Time: %f %f %f\n", h, m, s);
    read_sensors();
    count = 0;
  }
}
