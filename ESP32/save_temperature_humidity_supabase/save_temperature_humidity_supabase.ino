/*
  ESP32 + BME280 (I2C) + Supabase REST
  - On wake: connect WiFi, sync NTP, read BME280 and POST to Supabase 
  - Then: deep sleep until the next 00/30 minute slot

  Notes:
  - Uses client.setInsecure() for TLS simplicity.
    For production, validate TLS using the correct Root CA.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <time.h>

#include <esp_netif.h>
#include <esp_sntp.h>

// ---------------------- User configuration ----------------------

// WiFi credentials
static const char* WIFI_SSID = "YOUR_WIFI_SSID";
static const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// Supabase
static const char* SUPABASE_HOST   = "YOUR_SUPABASE_HOST_URL";
static const int   SUPABASE_PORT   = 443;
static const char* SUPABASE_PATH   = "YOUR_SUPABASE_PATH";
static const char* SUPABASE_APIKEY = "YOUR_SUPABASE_APIKEY";

// NTP
static const char* NTP_SERVER_1 = "pool.ntp.org";
static const char* NTP_SERVER_2 = "time.nist.gov";

// Europe/Lisbon timezone with DST rules (WET/WEST).
// If you want to avoid DST, use "UTC0".
static const char* TZ_INFO = "WET0WEST,M3.5.0/1,M10.5.0/2";

// Timeouts
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
static const uint32_t HTTP_TIMEOUT_MS         = 8000;
const unsigned long NTP_TIMEOUT_MS = 20000;

// BME280 pins and address
const uint8_t PIN_BME_SDA = 17;
const uint8_t PIN_BME_SCL = 16;
const uint8_t PIN_BME_PWR = 4;    // Irrelevant if sensor powered directly from 3.3V
const uint8_t BME_ADDRESS = 0x76; // Try 0x77 if needed

Adafruit_BME280 bme;

const int SLEEP_DURATION = 30;
const int WAKE_TIME = 06;

// ---------------------- Helpers: WiFi ----------------------

static bool connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  Serial.printf("[WIFI] Connecting to SSID: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - start) < WIFI_CONNECT_TIMEOUT_MS)
  {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.printf("[WIFI] Failed to connect. status=%d\n", (int)WiFi.status());
    return false;
  }

  Serial.printf("[WIFI] Connected. IP=%s\n", WiFi.localIP().toString().c_str());
  return true;
}

void killWiFi()
{
  WiFi.disconnect();
  WiFi.mode(WIFI_MODE_NULL);
}

// ---------------------- Helpers: NTP time sync ----------------------

/* Waits for NTP server time sync, adjusted for the time zone specified in
 * config.cpp.
 *
 * Returns true if time was set successfully, otherwise false.
 *
 * Note: Must be connected to WiFi to get time from NTP server.
 */
bool waitForSNTPSync(tm *timeInfo)
{
  // Wait for SNTP synchronization to complete
  unsigned long timeout = millis() + NTP_TIMEOUT_MS;
  if ((esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET)
      && (millis() < timeout))
  {
    Serial.print("Waiting for SNTP synchronization.");
    delay(100); // ms
    while ((esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET)
        && (millis() < timeout))
    {
      Serial.print(".");
      delay(100); // ms
    }
    Serial.println();
  }
  return printLocalTime(timeInfo);
} // Credits: https://github.com/lmarzen/esp32-weather-epd

/*
 * configTzTime
 * sntp setup using TZ environment variable
 * */
void configTzTime(const char* tz, const char* server1, const char* server2, const char* server3)
{
    //tcpip_adapter_init();  // Should not hurt anything if already inited
    esp_netif_init();
    if(esp_sntp_enabled()){
        esp_sntp_stop();
    }
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, (char*)server1);
    sntp_setservername(1, (char*)server2);
    sntp_setservername(2, (char*)server3);
    sntp_init();
    setenv("TZ", tz, 1);
    tzset();
} // Credits: https://github.com/lmarzen/esp32-weather-epd

// ---------------------- Helpers: Time ----------------------

/* Prints the local time to serial monitor.
 *
 * Returns true if getting local time was a success, otherwise false.
 */
bool printLocalTime(tm *timeInfo)
{
  int attempts = 0;
  while (!getLocalTime(timeInfo) && attempts++ < 3)
  {
    Serial.println("Failed to get the time!");
    return false;
  }
  Serial.println(timeInfo, "%A, %B %d, %Y %H:%M:%S");
  return true;
} // Credits: https://github.com/lmarzen/esp32-weather-epd

bool getLocalTime(struct tm * info, uint32_t ms)
{
    uint32_t start = millis();
    time_t now;
    while((millis()-start) <= ms) {
        time(&now);
        localtime_r(&now, info);
        if(info->tm_year > (2016 - 1900)){
            return true;
        }
        delay(10);
    }
    return false;
} // Credits: https://github.com/lmarzen/esp32-weather-epd

// ---------------------- Helpers: Deep Sleep ----------------------

/* Put esp32 into ultra low-power deep sleep (<11μA).
 * Aligns wake time to the minute. Sleep times defined in config.cpp.
 */
void beginDeepSleep(unsigned long startTime, tm *timeInfo)
{
  if (!getLocalTime(timeInfo))
  {
    Serial.println("Failed to synchronize time before deep-sleep, referencing older time.");
  }

  // time is relative to wake time
  int curHour = (timeInfo->tm_hour - WAKE_TIME + 24) % 24;
  const int curMinute = curHour * 60 + timeInfo->tm_min;
  const int curSecond = curHour * 3600
                      + timeInfo->tm_min * 60
                      + timeInfo->tm_sec;
  const int desiredSleepSeconds = SLEEP_DURATION * 60;
  const int offsetMinutes = curMinute % SLEEP_DURATION;
  const int offsetSeconds = curSecond % desiredSleepSeconds;

  // align wake time to nearest multiple of SLEEP_DURATION
  int sleepMinutes = SLEEP_DURATION - offsetMinutes;
  if (desiredSleepSeconds - offsetSeconds < 120
   || offsetSeconds / (float)desiredSleepSeconds > 0.95f)
  { // if we have a sleep time less than 2 minutes OR less 5% SLEEP_DURATION,
    // skip to next alignment
    sleepMinutes += SLEEP_DURATION;
  }

  // estimated wake time, if this falls in a sleep period then sleepDuration
  // must be adjusted
  const int predictedWakeHour = ((curMinute + sleepMinutes) / 60) % 24;

  uint64_t sleepDuration = sleepMinutes * 60 - timeInfo->tm_sec;

  // add extra delay to compensate for esp32's with fast RTCs.
  sleepDuration += 3ULL;
  sleepDuration *= 1.0015f;

  esp_sleep_enable_timer_wakeup(sleepDuration * 1000000ULL);
  Serial.print("Awake for");
  Serial.println(" "  + String((millis() - startTime) / 1000.0, 3) + "s");
  Serial.print("Entering deep sleep for");
  Serial.println(" " + String(sleepDuration) + "s");
  esp_deep_sleep_start();
} // Credits: https://github.com/lmarzen/esp32-weather-epd

// ---------------------- Helpers: BME280 ----------------------

static bool initBME()
{
  pinMode(PIN_BME_PWR, OUTPUT);
  digitalWrite(PIN_BME_PWR, HIGH);
  delay(50);

  Wire.begin(PIN_BME_SDA, PIN_BME_SCL);

  Serial.println("[BME] Initializing BME280...");
  if (!bme.begin(BME_ADDRESS))
  {
    Serial.println("[BME] BME280 not found at given address.");
    return false;
  }

  Serial.println("[BME] BME280 ready.");
  return true;
}

static void powerDownBME()
{
  digitalWrite(PIN_BME_PWR, LOW);
}

// ---------------------- Helpers: timestamp formatting ----------------------

/* Builds a UTC timestamp string in the format:
 *   YYYY-MM-DD HH:MM:SS.ffffff+00
 *
 * Returns true if the timestamp could be generated, otherwise false.
 */
static bool buildUtcTimestamp(char* out, size_t outSize)
{
  timeval tv{};
  if (gettimeofday(&tv, nullptr) != 0)
  {
    return false;
  }

  time_t seconds = tv.tv_sec;
  tm utc{};
  gmtime_r(&seconds, &utc);

  // Example: 2026-02-25 16:59:21.561343+00
  int n = snprintf(
    out, outSize,
    "%04d-%02d-%02d %02d:%02d:%02d.%06ld+00",
    utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
    utc.tm_hour, utc.tm_min, utc.tm_sec,
    (long)tv.tv_usec
  );

  return (n > 0) && ((size_t)n < outSize);
}

// ---------------------- Helpers: Other ----------------------

void disableBuiltinLED()
{
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  gpio_hold_en(static_cast<gpio_num_t>(LED_BUILTIN));
  gpio_deep_sleep_hold_en();
  return;
} // Credits: https://github.com/lmarzen/esp32-weather-epd

// ---------------------- Supabase POST ----------------------

static int postToSupabase(float temperatureC, float humidityPct)
{
  char createdAt[40] = {0};
  bool timestampIsBuilt = buildUtcTimestamp(createdAt, sizeof(createdAt));
  if (!timestampIsBuilt)
  {
    Serial.println("[TIME] Failed to build UTC timestamp, using empty created_at.");
  }

  WiFiClientSecure client;
  client.setInsecure(); // Replace with CA validation for production

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(client, SUPABASE_HOST, SUPABASE_PORT, SUPABASE_PATH))
  {
    Serial.println("[HTTP] http.begin() failed.");
    return -1;
  }

  http.addHeader("apikey", SUPABASE_APIKEY);
  http.addHeader("content-type", "application/json");
  http.addHeader("prefer", "return=minimal");

  String payload = "{";
  payload += "\"temperature\":";
  payload += String(temperatureC, 2);
  payload += ",";
  payload += "\"humidity\":";
  payload += String(humidityPct, 2);

  if (timestampIsBuilt)
  {
    payload += ",";
    payload += "\"created_at\":\"";
    payload += String(createdAt);
    payload += "\"";
  }

  payload += "}";

  Serial.print("[HTTP] POST payload: ");
  Serial.println(payload);

  int code = http.POST(payload);

  if (code <= 0 || (code != 200 && code != 201 && code != 204))
  {
    Serial.printf("[HTTP] POST failed. Code=%d\n", code);
    String body = http.getString();
    if (body.length())
    {
      Serial.print("[HTTP] Response body: ");
      Serial.println(body);
    }
  }
  else
  {
    Serial.printf("[HTTP] POST success. Code=%d\n", code);
  }

  http.end();
  return code;
}

// ---------------------- Main ----------------------

void setup()
{
  unsigned long startTime = millis();
  Serial.begin(115200);
  
  disableBuiltinLED();

  tm timeInfo = {};

  Serial.println();
  Serial.println("=== ESP32 BME280 -> Supabase (NTP minute check, deep sleep) ===");

  if (!connectWiFi())
  {
    // If WiFi fails, try again soon
    beginDeepSleep(startTime, &timeInfo);
  }

  // Time Synchronization
  configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
  bool timeConfigured = waitForSNTPSync(&timeInfo);
  if (!timeConfigured)
  {
    Serial.println("Time Synchronization Failed");
    killWiFi();
    beginDeepSleep(startTime, &timeInfo);
  }
  
  if (!initBME())
  {
    Serial.printf("[BME] Init failed. Going to sleep.");
    killWiFi();
    beginDeepSleep(startTime, &timeInfo);
  }

  float temperatureC = bme.readTemperature();
  float humidityPct  = bme.readHumidity();

  Serial.printf("[BME] Temperature: %.2f C\n", temperatureC);
  Serial.printf("[BME] Humidity:    %.2f %%\n", humidityPct);

  powerDownBME();

  postToSupabase(temperatureC, humidityPct);

  Serial.printf("[SCHEDULE] Done.Going to sleep\n");
  killWiFi();
  beginDeepSleep(startTime, &timeInfo);
}

void loop()
{
  // Not used. Device sleeps at end of setup().
}