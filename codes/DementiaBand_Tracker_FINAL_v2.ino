/*
  ================================================================================
  DEMENTIA SAFETY BAND — FINAL FIRMWARE
  LilyGO T-SIM7000G (ESP32 + SIM7000G LTE/GPS modem) — Step 2 of 2
  ================================================================================

  ONLY UPLOAD THIS AFTER DementiaBand_Diagnostics.ino HAS PASSED:
    - Network registration: PASS
    - GPS: PASS (at least once, outdoors)
    - SMS: PASS (you received the test text)
  If any of those failed, fix that first — this firmware assumes the
  hardware already works, so problems here will be much harder to debug.

  WHAT THIS FIRMWARE DOES
  - On first boot, gets a GPS fix and saves it as "home" (kept in flash,
    survives power loss/reboots).
  - Every CHECK_INTERVAL_MS, checks the current distance from home.
  - If the wearer leaves the geofence radius, texts every caregiver number
    below with a Google Maps link. Keeps re-texting every REALERT_INTERVAL_MS
    for as long as they remain outside the zone, and texts once more when
    they return.
  - If GPS is temporarily unavailable, falls back to the last known
    location and says so clearly in the alert.
  - A physical SOS button sends an immediate alert with location on demand.
  - Warns caregivers by SMS if the band's battery gets low.
  - Retries SMS sending a few times if the network is briefly unavailable.

  HOW TO SET UP
  1. Wire a push-button between SOS_BUTTON_PIN and GND (see project guide
     for wiring). Not required to flash this — you can add it later —
     but without it the SOS feature just won't trigger.
  2. Edit the CONFIGURATION block below: caregiver number(s), patient
     label, geofence radius, and check interval.
  3. Upload. Watch Serial Monitor at 115200 baud the first time, so you
     can see the home location get set and confirm the first SMS arrives.
  4. To reset the saved home location later (e.g. after moving house),
     hold the SOS button while powering on/resetting the board for
     3+ seconds.

  IMPORTANT SAFETY NOTE
  This is a DIY assistive device, not a certified medical/safety product.
  Test it thoroughly in real conditions (battery life, GPS accuracy near
  home, SMS delivery time) before relying on it, and keep using whatever
  other safety measures are already in place for the wearer.
  ================================================================================
*/

#include <math.h>
#include <Preferences.h>

// ================================================================================
// *** EDIT THIS BLOCK BEFORE UPLOADING ***
// ================================================================================
const char* CAREGIVER_NUMBERS[] = {
  "+919909005723",         // primary caregiver - REQUIRED, edit this
  // "+91YYYYYYYYYY",      // uncomment and edit to add a second caregiver
};
const int NUM_CAREGIVERS = sizeof(CAREGIVER_NUMBERS) / sizeof(CAREGIVER_NUMBERS[0]);

#define PATIENT_LABEL          "Dad"     // used in every SMS, e.g. "Dad has left the safe zone"
#define GEOFENCE_RADIUS_M      100       // meters - how far from home before it's an "alert"
#define CHECK_INTERVAL_MS      10000UL   // how often to check position (10 sec, per spec)
#define REALERT_INTERVAL_MS    300000UL  // resend the alert every 5 min while still outside
#define LOW_BATTERY_MV         3500      // send a low-battery warning below this (millivolts)
#define SOS_BUTTON_PIN         33        // change if your board breaks out a different free pin

// ================================================================================
// BOARD / MODEM DEFINITIONS
// Verified from LilyGO-T-SIM7000G/examples/Arduino_TinyGSM/AllFunctions/AllFunctions.ino
// ================================================================================
#define TINY_GSM_MODEM_SIM7000
#define TINY_GSM_RX_BUFFER   1024
#define SerialAT              Serial1

#define UART_BAUD    115200
#define PIN_DTR      25
#define PIN_TX       27
#define PIN_RX       26
#define PWR_PIN      4
#define LED_PIN      12

#include <TinyGsmClient.h>
TinyGsm modem(SerialAT);
Preferences prefs;

// --------------------------------------------------------------------------------
// State
// --------------------------------------------------------------------------------
float homeLat = 0, homeLon = 0;
float lastGoodLat = 0, lastGoodLon = 0;
unsigned long lastGoodFixMillis = 0;   // 0 means "no fix yet since boot"
unsigned long lastCheckMillis = 0;
unsigned long lastAlertMillis = 0;
bool isOutsideZone = false;
bool lowBatteryAlerted = false;

// --------------------------------------------------------------------------------
// Power on the SIM7000 - HIGH then LOW, verified against LilyGO's own examples.
// --------------------------------------------------------------------------------
void modemPowerOn() {
  pinMode(PWR_PIN, OUTPUT);
  digitalWrite(PWR_PIN, HIGH);
  delay(1000);
  digitalWrite(PWR_PIN, LOW);
  delay(3000);
}

// --------------------------------------------------------------------------------
// Try a range of baud rates until the modem responds "OK" to a plain "AT".
// This modem has been observed booting at either 57600 or 115200 depending on
// the run, so a fixed baud isn't reliable - detect it every boot instead.
// --------------------------------------------------------------------------------
uint32_t autoBaud() {
  static uint32_t rates[] = {115200, 57600, 38400, 19200, 9600,
                              74400, 74880, 230400, 460800, 2400, 4800, 14400, 28800};
  for (uint8_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
    uint32_t rate = rates[i];
    SerialAT.updateBaudRate(rate);
    delay(10);
    for (int j = 0; j < 3; j++) {
      SerialAT.print("AT\r\n");
      delay(300);
      String resp = SerialAT.readString();
      if (resp.indexOf("OK") >= 0) return rate;
    }
  }
  return 0;
}

String buildMapsLink(float lat, float lon) {
  return "https://maps.google.com/?q=" + String(lat, 6) + "," + String(lon, 6);
}

// Distance between two lat/lon points in meters (Haversine formula)
float haversineMeters(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371000.0;
  float phi1 = lat1 * DEG_TO_RAD;
  float phi2 = lat2 * DEG_TO_RAD;
  float dPhi = (lat2 - lat1) * DEG_TO_RAD;
  float dLambda = (lon2 - lon1) * DEG_TO_RAD;
  float a = sin(dPhi / 2) * sin(dPhi / 2) +
            cos(phi1) * cos(phi2) * sin(dLambda / 2) * sin(dLambda / 2);
  float c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return R * c;
}

// Sends a message to every caregiver number, retrying a few times each if the
// network is temporarily unavailable, per the original spec.
bool sendToAllCaregivers(const String& message) {
  bool allOK = true;
  for (int i = 0; i < NUM_CAREGIVERS; i++) {
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
      if (attempt > 0) {
        if (!modem.isNetworkConnected()) modem.waitForNetwork(15000L);
        delay(2000);
      }
      ok = modem.sendSMS(CAREGIVER_NUMBERS[i], message);
    }
    Serial.print(F("SMS to "));
    Serial.print(CAREGIVER_NUMBERS[i]);
    Serial.println(ok ? F(" sent.") : F(" FAILED after 3 tries."));
    if (!ok) allOK = false;
  }
  return allOK;
}

// --------------------------------------------------------------------------------
// SOS button - debounced, ignores repeat presses within 5 seconds
// --------------------------------------------------------------------------------
void checkSosButton() {
  static unsigned long lastPress = 0;
  if (digitalRead(SOS_BUTTON_PIN) == LOW && (millis() - lastPress) > 5000UL) {
    lastPress = millis();
    Serial.println(F("SOS button pressed!"));
    String msg = "SOS: " + String(PATIENT_LABEL) + " pressed the help button.\n";
    if (lastGoodFixMillis != 0) {
      msg += "Location: " + buildMapsLink(lastGoodLat, lastGoodLon);
    } else {
      msg += "Location: not available yet.";
    }
    sendToAllCaregivers(msg);
  }
}

// --------------------------------------------------------------------------------
// Main geofence logic - runs once every CHECK_INTERVAL_MS
// --------------------------------------------------------------------------------
void performGeofenceCheck() {
  float curLat, curLon;
  bool freshFix = modem.getGPS(&curLat, &curLon);

  if (freshFix) {
    lastGoodLat = curLat;
    lastGoodLon = curLon;
    lastGoodFixMillis = millis();
  }

  if (lastGoodFixMillis == 0) {
    Serial.println(F("No GPS fix yet since boot - skipping geofence check."));
    return;
  }

  float dist = haversineMeters(homeLat, homeLon, lastGoodLat, lastGoodLon);
  Serial.printf("Distance from home: %.0f m (fix is %s)\n", dist, freshFix ? "fresh" : "last known");

  bool nowOutside = dist > GEOFENCE_RADIUS_M;

  if (nowOutside) {
    bool isNewEvent = !isOutsideZone;
    bool timeForRealert = (millis() - lastAlertMillis) >= REALERT_INTERVAL_MS;

    if (isNewEvent || timeForRealert) {
      String msg;
      if (freshFix) {
        msg = "ALERT: " + String(PATIENT_LABEL) + " has left the safe zone.\n" +
              "Location: " + buildMapsLink(lastGoodLat, lastGoodLon) + "\n" +
              "Distance: " + String((int)dist) + "m";
      } else {
        unsigned long minsAgo = (millis() - lastGoodFixMillis) / 60000UL;
        msg = "ALERT: " + String(PATIENT_LABEL) + " may have left the safe zone.\n" +
              "GPS unavailable - last known location (" + String(minsAgo) + " min ago):\n" +
              buildMapsLink(lastGoodLat, lastGoodLon);
      }
      sendToAllCaregivers(msg);
      lastAlertMillis = millis();
    }
    isOutsideZone = true;
  } else {
    if (isOutsideZone) {
      sendToAllCaregivers(String(PATIENT_LABEL) + " is back in the safe zone.");
    }
    isOutsideZone = false;
  }
}

void checkBattery() {
  int16_t battMv = modem.getBattVoltage();
  if (battMv <= 0) return; // reading unavailable (e.g. USB-only power) - nothing to warn about

  if (battMv <= LOW_BATTERY_MV && !lowBatteryAlerted) {
    sendToAllCaregivers(String(PATIENT_LABEL) + "'s tracker battery is low (" +
                         String(battMv / 1000.0, 2) + "V). Please charge soon.");
    lowBatteryAlerted = true;
  } else if (battMv > LOW_BATTERY_MV + 200) {
    lowBatteryAlerted = false; // re-arm once it's recovered with some hysteresis
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // off
  pinMode(SOS_BUTTON_PIN, INPUT_PULLUP);

  Serial.println(F("\n=== Dementia Safety Band - Final Firmware ==="));

  // Hold the SOS button during boot for 3+ seconds to reset the saved home location.
  bool resetHome = false;
  if (digitalRead(SOS_BUTTON_PIN) == LOW) {
    Serial.println(F("SOS button held at boot - checking for home-reset request..."));
    unsigned long holdStart = millis();
    while (digitalRead(SOS_BUTTON_PIN) == LOW) {
      if (millis() - holdStart > 3000UL) { resetHome = true; break; }
    }
  }

  modemPowerOn();
  SerialAT.begin(9600, SERIAL_8N1, PIN_RX, PIN_TX);
  delay(3000);
  uint32_t baud = autoBaud();
  Serial.print(F("Modem baud: "));
  Serial.println(baud == 0 ? "NOT DETECTED - check power/battery" : String(baud));

  Serial.println(F("Initializing modem..."));
  if (!modem.restart()) {
    Serial.println(F("Restart failed, trying init() instead..."));
    modem.init();
  }
  Serial.print(F("Modem: "));
  Serial.println(modem.getModemInfo());

  Serial.println(F("Waiting for network..."));
  if (modem.waitForNetwork(60000L)) {
    Serial.print(F("Network OK. Operator: "));
    Serial.println(modem.getOperator());
  } else {
    Serial.println(F("WARNING: not registered yet - will keep retrying automatically."));
  }

  Serial.println(F("Enabling GPS..."));
  modem.sendAT("+CGPIO=0,48,1,1"); // power the active GPS antenna
  modem.waitResponse(10000L);
  modem.enableGPS();

  prefs.begin("dband", false);
  if (resetHome) {
    Serial.println(F("Home-reset requested - clearing saved home location."));
    prefs.putBool("homeSet", false);
  }

  if (prefs.getBool("homeSet", false)) {
    homeLat = prefs.getFloat("homeLat", 0);
    homeLon = prefs.getFloat("homeLon", 0);
    Serial.println("Loaded saved home location: " + buildMapsLink(homeLat, homeLon));
    lastGoodLat = homeLat;
    lastGoodLon = homeLon;
    lastGoodFixMillis = millis();
  } else {
    Serial.println(F("No home location saved. Acquiring GPS fix to set home..."));
    Serial.println(F("Go outside with open sky. This can take 30 sec - 3 min."));
    bool gotFix = false;
    unsigned long start = millis();
    while (millis() - start < 180000UL) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN)); // blink while acquiring
      if (modem.getGPS(&homeLat, &homeLon)) { gotFix = true; break; }
      delay(2000);
      Serial.print(".");
    }
    Serial.println();
    if (gotFix) {
      prefs.putFloat("homeLat", homeLat);
      prefs.putFloat("homeLon", homeLon);
      prefs.putBool("homeSet", true);
      lastGoodLat = homeLat;
      lastGoodLon = homeLon;
      lastGoodFixMillis = millis();
      Serial.println("Home location set: " + buildMapsLink(homeLat, homeLon));
    } else {
      Serial.println(F("Could not get a GPS fix to set home."));
      Serial.println(F("Restart this board outdoors/near a window before relying on it."));
      homeLat = 0; homeLon = 0;
    }
  }

  digitalWrite(LED_PIN, HIGH);

  sendToAllCaregivers(String(PATIENT_LABEL) + "'s tracker is online.\nHome zone: " +
                       buildMapsLink(homeLat, homeLon) + "\nRadius: " +
                       String(GEOFENCE_RADIUS_M) + "m");

  lastCheckMillis = millis();
  Serial.println(F("=== Setup complete. Entering monitoring loop. ===\n"));
}

void loop() {
  checkSosButton();

  if (millis() - lastCheckMillis >= CHECK_INTERVAL_MS) {
    lastCheckMillis = millis();
    performGeofenceCheck();
    checkBattery();
  }

  // Status LED: fast blink = currently outside the safe zone, slow blink = normal
  if (isOutsideZone) {
    digitalWrite(LED_PIN, (millis() / 200) % 2);
  } else {
    digitalWrite(LED_PIN, (millis() / 2000) % 2);
  }
}
