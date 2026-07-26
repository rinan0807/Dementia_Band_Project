/*
  ================================================================================
  LILYGO T-SIM7000G — FULL HARDWARE DIAGNOSTIC
  Dementia Safety Band Project — Step 1 of 2
  ================================================================================

  WHAT THIS DOES
  Tests every subsystem on the board one at a time and prints a clear
  PASS / FAIL for each one, plus the actual data (ICCID, signal strength,
  GPS coordinates, etc). This replaces Trial Code #1 and #2 — it does
  everything they did, plus SIM/network/battery checks, in one sketch.

  WHY YOUR EARLIER TEST SAID "MODEM FAILED"
  Almost every generic SIM800/SIM900 tutorial online pulses the power pin
  LOW-then-HIGH to boot the modem. This exact board (T-SIM7000G) has a
  transistor on the PWRKEY line that INVERTS that signal, so it actually
  needs to be pulsed HIGH-then-LOW. If your old code did it the "normal"
  way, the modem chip never actually turned on — which shows up as
  "modem failed" even though the wiring is fine.

  The sequence below is copied directly from LilyGO's own official example
  repository (github.com/Xinyuan-LilyGO/LilyGO-T-SIM7000G), not guessed,
  so this should match your board exactly.

  HOW TO USE THIS FILE
  1. Install the Arduino core for ESP32 (Boards Manager) if you haven't.
  2. Tools > Board > select "ESP32 Wrover Module" (this board has PSRAM).
  3. Sketch > Include Library > Manage Libraries > install "TinyGSM"
     by vshymanskyy (version 0.11.x or newer).
  4. Plug in a charged 3.7V LiPo battery to the JST connector — see the
     project guide (DementiaBand_Project_Guide.md), section "Hardware
     checklist". USB power alone is a common cause of failures on this
     board because the modem draws current spikes USB can't supply.
  5. Insert an activated SIM card, and connect BOTH antennas:
     the LTE antenna to the port labelled "LTE", the GPS antenna to the
     port labelled "GPS". Mixing these up will make GPS or network fail.
  6. Edit CAREGIVER_TEST_NUMBER below to your own phone number.
  7. Upload this sketch, then open Serial Monitor at 115200 baud.
  8. Read DementiaBand_Project_Guide.md to interpret the results.

  Once every test here passes, move on to DementiaBand_Tracker_FINAL.ino.
  ================================================================================
*/

// ================================================================================
// *** EDIT THIS BEFORE UPLOADING ***
// ================================================================================
#define CAREGIVER_TEST_NUMBER   "+919909002723"   // YOUR phone number, with country code
#define SEND_TEST_SMS            true             // set to false after SMS test passes once,
                                                    // so re-running diagnostics doesn't keep texting you

// ================================================================================
// BOARD / MODEM DEFINITIONS
// Verified from LilyGO-T-SIM7000G/examples/Arduino_TinyGSM/AllFunctions/AllFunctions.ino
// Do not change these unless you have a different board revision.
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

// Uncomment the line below to see every raw AT command and response.
// Extremely useful if something below still fails and you want to see
// exactly what the modem is saying.
// #define DUMP_AT_COMMANDS

#include <TinyGsmClient.h>

#ifdef DUMP_AT_COMMANDS
  #include <StreamDebugger.h>
  StreamDebugger debugger(SerialAT, Serial);
  TinyGsm modem(debugger);
#else
  TinyGsm modem(SerialAT);
#endif

int testNum = 0;
const int TOTAL_TESTS = 10;

// --------------------------------------------------------------------------------
// Small helpers for consistent [n/10] PASS/FAIL formatted output
// --------------------------------------------------------------------------------
void stepStart(const char* label) {
  testNum++;
  Serial.println();
  Serial.printf("[%d/%d] %s\n", testNum, TOTAL_TESTS, label);
}

void pass(const String& detail = "") {
  Serial.print(F("        -> PASS  "));
  Serial.println(detail);
}

void fail(const String& detail) {
  Serial.print(F("        -> FAIL  "));
  Serial.println(detail);
}

// --------------------------------------------------------------------------------
// Power on the SIM7000 using the exact sequence LilyGO uses for this board.
// HIGH-then-LOW is correct here — see the note at the top of this file.
// --------------------------------------------------------------------------------
void modemPowerOn() {
  pinMode(PWR_PIN, OUTPUT);
  digitalWrite(PWR_PIN, HIGH);
  delay(1000);
  digitalWrite(PWR_PIN, LOW);
  delay(3000); // give the module time to boot before we start talking to it
}

// --------------------------------------------------------------------------------
// Try a range of baud rates until the modem responds "OK" to a plain "AT".
// Returns the working baud rate, or 0 if nothing worked.
// --------------------------------------------------------------------------------
uint32_t autoBaud() {
  static uint32_t rates[] = {115200, 57600, 38400, 19200, 9600,
                              74400, 74880, 230400, 460800, 2400, 4800, 14400, 28800};
  for (uint8_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
    uint32_t rate = rates[i];
    Serial.printf("        trying %u baud...\n", rate);
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

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // HIGH = LED off on this board

  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F(" LILYGO T-SIM7000G - FULL DIAGNOSTICS"));
  Serial.println(F(" Dementia Band Project"));
  Serial.println(F("========================================"));

  // ---- [1/10] ESP32 boot ----
  stepStart("ESP32 boot");
  pass();

  // ---- [2/10] Power on SIM7000 ----
  stepStart("Powering SIM7000 (PWRKEY pulse: HIGH -> LOW)");
  modemPowerOn();
  pass();

  // ---- [3/10] UART / baud detect ----
  stepStart("Detecting modem baud rate");
  SerialAT.begin(9600, SERIAL_8N1, PIN_RX, PIN_TX);
  delay(100);
  uint32_t rate = autoBaud();
  if (rate == 0) {
    fail("No response at any baud rate.");
    Serial.println();
    Serial.println(F("   Most likely causes, in order of likelihood on this board:"));
    Serial.println(F("   1. No battery connected. USB alone often cannot supply the current"));
    Serial.println(F("      spikes the modem needs at boot. Plug in a charged 3.7V LiPo."));
    Serial.println(F("   2. The board's physical battery power switch (if present) is OFF."));
    Serial.println(F("   3. Wrong board selected in Arduino IDE (must be ESP32 Wrover Module)."));
    Serial.println(F("   4. A cold module sometimes needs a second try - press RESET and re-run."));
    Serial.println(F("   See DementiaBand_Project_Guide.md, section 'Modem still fails'."));
    while (true) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(150); // fast blink = hard stop, cannot continue further tests
    }
  }
  pass(String(rate) + " baud");

  // ---- [4/10] Modem communication / firmware ----
  stepStart("Modem communication (init)");
  if (!modem.init()) {
    fail("init() returned false - continuing anyway, some modems still work.");
  } else {
    pass();
  }
  Serial.print(F("        Firmware: "));
  Serial.println(modem.getModemInfo());

  // ---- [5/10] SIM card ----
  stepStart("SIM card");
  SimStatus simStatus = modem.getSimStatus();
  if (simStatus == SIM_READY) {
    pass();
    Serial.print(F("        ICCID: "));
    Serial.println(modem.getSimCCID());
  } else {
    fail("SIM not ready (status code " + String((int)simStatus) + ").");
    Serial.println(F("        Check: SIM fully seated in the tray? Correct way round?"));
    Serial.println(F("        Is the SIM active and not expired/out of balance?"));
  }
  Serial.print(F("        IMEI: "));
  Serial.println(modem.getIMEI());

  // ---- [6/10] Signal strength ----
  stepStart("Signal strength (RSSI)");
  int csq = modem.getSignalQuality();
  Serial.printf("        RSSI: %d / 31\n", csq);
  if (csq == 99 || csq <= 0) {
    fail("No signal detected.");
    Serial.println(F("        Check: is the LTE antenna connected to the port labelled 'LTE'"));
    Serial.println(F("        (not the GPS port)? Try moving near a window."));
  } else if (csq < 10) {
    pass("weak, but usable - move to a better signal area if you can");
  } else {
    pass();
  }
  Serial.println(F("        RSSI guide: 0-9 poor, 10-15 usable, 16-25 good, 26-31 excellent"));

  // ---- [7/10] Network registration ----
  stepStart("Network registration");
  Serial.println(F("        Waiting up to 60s..."));
  if (modem.waitForNetwork(60000L)) {
    pass();
    Serial.print(F("        Operator: "));
    Serial.println(modem.getOperator());
  } else {
    fail("Not registered on the network within 60s.");
    Serial.println(F("        Check: SIM active with a plan/balance? Try moving outdoors."));
    Serial.println(F("        Note: SMS only needs basic 2G/LTE registration, not mobile data."));
  }

  // ---- [8/10] GPS ----
  stepStart("GPS");
  Serial.println(F("        Powering GPS antenna and enabling GNSS..."));
  modem.sendAT("+CGPIO=0,48,1,1"); // powers the active GPS antenna - LilyGO-specific, easy to miss
  modem.waitResponse(10000L);
  modem.enableGPS();
  Serial.println(F("        Waiting up to 90s for first fix."));
  Serial.println(F("        Go outside with a clear view of the sky for this test."));
  Serial.println(F("        (Indoors this will usually FAIL - that's expected, not a bug.)"));

  float lat = 0, lon = 0, speed = 0, alt = 0, accuracy = 0;
  int vsat = 0, usat = 0, year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  bool gotFix = false;
  unsigned long gpsStart = millis();
  while (millis() - gpsStart < 90000UL) {
    if (modem.getGPS(&lat, &lon, &speed, &alt, &vsat, &usat, &accuracy,
                      &year, &month, &day, &hour, &minute, &second)) {
      gotFix = true;
      break;
    }
    delay(2000);
    Serial.print(".");
  }
  Serial.println();
  if (gotFix) {
    pass();
    Serial.printf("        Latitude : %.6f\n", lat);
    Serial.printf("        Longitude: %.6f\n", lon);
    Serial.printf("        Satellites used: %d (visible: %d)\n", usat, vsat);
    Serial.printf("        Altitude: %.1f m   Speed: %.1f km/h\n", alt, speed);
    Serial.printf("        UTC time: %04d-%02d-%02d %02d:%02d:%02d\n", year, month, day, hour, minute, second);
  } else {
    fail("No fix within 90s.");
    Serial.println(F("        Normal indoors/near windows. Re-run outdoors with open sky."));
  }

  // ---- [9/10] SMS ----
  stepStart("SMS send test");
  if (!SEND_TEST_SMS) {
    Serial.println(F("        SKIPPED (SEND_TEST_SMS = false)"));
  } else if (String(CAREGIVER_TEST_NUMBER) == "+91XXXXXXXXXX") {
    fail("Set CAREGIVER_TEST_NUMBER at the top of this file to your real number first.");
  } else {
    bool smsOK = modem.sendSMS(CAREGIVER_TEST_NUMBER, "T-SIM7000G DIAGNOSTIC: SMS test OK.");
    if (smsOK) {
      pass("Check your phone for the message.");
    } else {
      fail("sendSMS() returned false. Usually means not registered on network yet (see test 7).");
    }
  }

  // ---- [10/10] Battery ----
  stepStart("Battery voltage");
  int16_t battMv = modem.getBattVoltage();
  int8_t battPct = modem.getBattPercent();
  if (battMv > 0) {
    pass();
    Serial.printf("        Voltage: %.2f V   Percent: %d%%\n", battMv / 1000.0, battPct);
  } else {
    fail("No battery voltage reading.");
    Serial.println(F("        This usually just means: running on USB only, no LiPo attached."));
    Serial.println(F("        Attach a battery for real-world use - see the project guide."));
  }

  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F(" DIAGNOSTICS COMPLETE"));
  Serial.println(F(" See DementiaBand_Project_Guide.md for what to do next."));
  Serial.println(F("========================================"));
}

// --------------------------------------------------------------------------------
// After the diagnostics above finish, this sketch turns into a live AT command
// terminal: anything you type in the Serial Monitor's input box (top of the
// window) and send gets forwarded straight to the modem, and anything the
// modem replies gets printed back. Useful for testing fixes (like forcing
// GSM-only mode) without re-uploading for every attempt.
//
// Make sure Serial Monitor's line-ending dropdown is set to "Newline" or
// "Both NL & CR" (not "No line ending"), or the modem won't see a complete
// command.
// --------------------------------------------------------------------------------
void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() > 0) {
      SerialAT.println(cmd);
    }
  }
  if (SerialAT.available()) {
    Serial.write(SerialAT.read());
  }
}
