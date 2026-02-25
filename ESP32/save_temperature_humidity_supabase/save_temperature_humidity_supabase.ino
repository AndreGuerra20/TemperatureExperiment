/*
  ESP32 + BME280 (I2C) + Supabase REST
  - On wake: connect WiFi, sync NTP, check if current minute is 00 or 30
  - If minute is 00 or 30: read BME280 and POST to Supabase
  - Otherwise: deep sleep until the next 00/30 minute slot
  - Seconds are ignored (no strict boundary requirement)

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

// BME280 pins and address
const uint8_t PIN_BME_SDA = 17;
const uint8_t PIN_BME_SCL = 16;
const uint8_t PIN_BME_PWR = 4;    // Irrelevant if sensor powered directly from 3.3V
const uint8_t BME_ADDRESS = 0x76; // Try 0x77 if needed

Adafruit_BME280 bme;

// ---------------------- Helpers: deep sleep ----------------------

static void deepSleepSeconds(uint64_t seconds)
{
  if (seconds < 10) seconds = 10; // avoid rapid wake loops
  esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
  Serial.printf("[SLEEP] Deep sleeping for %llu seconds\n", seconds);
  Serial.flush();
  esp_deep_sleep_start();
}

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

// ---------------------- Helpers: NTP time sync ----------------------

static bool syncTime()
{
  setenv("TZ", TZ_INFO, 1);
  tzset();

  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);

  Serial.println("[TIME] Syncing via NTP...");
  time_t now = 0;

  // Wait up to ~10 seconds for a valid epoch
  for (int i = 0; i < 40; i++)
  {
    time(&now);
    if (now > 1700000000) // sanity threshold
    {
      struct tm t;
      localtime_r(&now, &t);
      Serial.printf("[TIME] Synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                    t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                    t.tm_hour, t.tm_min, t.tm_sec);
      return true;
    }
    delay(250);
  }

  Serial.println("[TIME] NTP sync failed.");
  return false;
}

// Returns seconds until the next minute slot where minute is 0 or 30.
// Seconds are ignored in the sense that we do not require landing exactly at second 0.
// However, we do account for current seconds to avoid waking too early.
static uint64_t secondsUntilNextHalfHourSlot(time_t nowLocal)
{
  struct tm t;
  localtime_r(&nowLocal, &t);

  int targetMin = 0;
  int addHours = 0;

  if (t.tm_min < 30)
  {
    targetMin = 30;
  }
  else
  {
    targetMin = 0;
    addHours = 1;
  }

  struct tm target = t;
  target.tm_sec = 0;
  target.tm_min = targetMin;
  target.tm_hour = t.tm_hour + addHours;

  // mktime normalizes hour/day/month/year (handles 24->next day and DST changes)
  time_t targetEpoch = mktime(&target);

  // If for some reason target is not in the future, add 60 seconds as a fallback
  if (targetEpoch <= nowLocal) targetEpoch = nowLocal + 60;
  return (uint64_t)(targetEpoch - nowLocal);
}

// Returns the epoch (UTC seconds) of the next half-hour slot boundary.
// A half-hour boundary is any time where epoch % 1800 == 0.
static time_t nextHalfHourBoundary(time_t now)
{
  const time_t period = 1800; // 30 minutes
  return ((now / period) + 1) * period;
}

// Returns how many seconds to sleep until the next half-hour boundary.
// Includes current seconds automatically.
static uint64_t secondsUntilNextHalfHourBoundary(time_t now)
{
  time_t next = nextHalfHourBoundary(now);
  if (next <= now) next = now + 60; // safety
  return (uint64_t)(next - now);
}

static bool isMinuteSlot(time_t nowLocal)
{
  struct tm t;
  localtime_r(&nowLocal, &t);
  return (t.tm_min == 0 || t.tm_min == 30);
}

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

// ---------------------- Supabase POST ----------------------

static int postToSupabase(float temperatureC, float humidityPct)
{
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
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_APIKEY);
  http.addHeader("content-type", "application/json");
  http.addHeader("prefer", "return=minimal");

  String payload = "{";
  payload += "\"temperature\":";
  payload += String(temperatureC, 2);
  payload += ",";
  payload += "\"humidity\":";
  payload += String(humidityPct, 2);
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
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("=== ESP32 BME280 -> Supabase (NTP minute check, deep sleep) ===");

  if (!connectWiFi())
  {
    // If WiFi fails, try again soon
    deepSleepSeconds(60);
  }

  if (!syncTime())
  {
    // If NTP fails, try again soon
    deepSleepSeconds(60);
  }

  time_t nowLocal;
  time(&nowLocal);

  if (!isMinuteSlot(nowLocal))
  {
    uint64_t waitSec = secondsUntilNextHalfHourBoundary(nowLocal);
    Serial.printf("[SCHEDULE] Not at minute 00/30. Sleeping %llu seconds until next slot\n", waitSec);
    deepSleepSeconds(waitSec);
  }

  // We are at minute 00 or 30 (seconds do not matter).
  if (!initBME())
  {
    time(&nowLocal);
    uint64_t waitSec = secondsUntilNextHalfHourSlot(nowLocal);
    Serial.printf("[BME] Init failed. Sleeping %llu seconds\n", waitSec);
    deepSleepSeconds(waitSec);
  }

  float temperatureC = bme.readTemperature();
  float humidityPct  = bme.readHumidity();

  Serial.printf("[BME] Temperature: %.2f C\n", temperatureC);
  Serial.printf("[BME] Humidity:    %.2f %%\n", humidityPct);

  powerDownBME();

  postToSupabase(temperatureC, humidityPct);

  // Sleep to the next 00/30 slot
  time(&nowLocal);
  uint64_t waitSec = secondsUntilNextHalfHourBoundary(nowLocal);
  Serial.printf("[SCHEDULE] Done. Sleeping %llu seconds\n", waitSec);
  deepSleepSeconds(waitSec);
}

void loop()
{
  // Not used. Device sleeps at end of setup().
}