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

static const int SDA_PIN = 8;
static const int SCL_PIN = 9;

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

// Clock global configuration (persisted to EEPROM).
char ntpServerName[80] = "fi.pool.ntp.org";
signed char clock_tz = 2; // Timezone shift (could be negative)
unsigned char clock_use_ntp = true;  // Use NTP switch
unsigned char clock_use_rtc = true;  // Use RTC switch

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
  EEPROM.commit();
}

// Write the config data to EEPROM.
void write_eeprom_data()
{
  EEPROM.write(eeprom_addr, clock_tz);
  EEPROM.write(eeprom_addr+1, clock_use_ntp);
  EEPROM.write(eeprom_addr+2, clock_use_rtc);
  EEPROM.writeString(eeprom_addr+3, ntpServerName);
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
      // Convert NTP time to UNIX time and apply timezone offset
      secsSince1900 = secsSince1900 - 2208988800UL + clock_tz * SECS_PER_HOUR;

      log_printf("Receive NTP Response %lu\n", (unsigned long)secsSince1900);

      // Update RTC and system time with received NTP time
      tm *ttm = localtime(&secsSince1900);
      myRTC.setSecond(ttm->tm_sec);
      myRTC.setMinute(ttm->tm_min);
      myRTC.setHour(ttm->tm_hour);

      set_clock_time(ttm->tm_hour, ttm->tm_min, ttm->tm_sec);

      return secsSince1900;
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

void setup() {
  delay(1000);
  Wire.begin(SDA_PIN, SCL_PIN);

  log_printf("Setup start\n");

  EEPROM.begin(100);
  read_eeprom_data();

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

void loop()
{
  static int count = 0;
  struct timeval tv;
  gettimeofday(&tv, &tz);
  tm *ttm = localtime(&tv.tv_sec);

  // Periodically resync from NTP, once per NTP_UPDATE_INTERVAL seconds.
  {
    static time_t last_sec;
    if (last_sec != tv.tv_sec) {
      last_sec = tv.tv_sec;
      if (clock_use_ntp && tv.tv_sec%NTP_UPDATE_INTERVAL==0) {
        getNtpTime();
      }
    }
  }

  // Web UI tick.
  ui.tick();

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
