#include <WiFi.h>
#include <FirebaseESP32.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

// ─── WiFi ────────────────────────────────────────────
#define WIFI_SSID     "Galaxy A22 5G 58a6"
#define WIFI_PASSWORD "mkua2373"

// ─── Firebase ────────────────────────────────────────
// BUG 1 FIXED: correct database URL
#define FIREBASE_HOST "bit-nexatag-default-rtdb.firebaseio.com"
#define FIREBASE_AUTH "utHtgBhcxvdYMDA8kj7frcgwV721jFKTchXnZ4k5"

// ─── Telegram ────────────────────────────────────────
#define BOT_TOKEN "8247546684:AAF8mGH8AMPpdlYTvN7mPpeV0N-W_pORe30"
#define CHAT_ID   "-5238943626"

// ─── Pins ────────────────────────────────────────────
#define SS_PIN    5
#define RST_PIN   27
#define LED_RED   25
#define LED_GREEN 26
#define LED_BLUE  14
#define BUZZER    32

// ─── Authorized Cards ────────────────────────────────
struct AuthCard {
  String uid;
  int tapCode;
  String ownerName;
};

AuthCard authorizedCards[] = {
  {"5D722B07", 3, "Ganesh"},
  {"17FE1807", 5, "Darshan"},
  {"53684856", 4, "Akash"}
};
// BUG 3 FIXED: was 2, now 3
int totalAuthorized = 3;

// ─── Objects ─────────────────────────────────────────
MFRC522 rfid(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ─── Anomaly Tracking ────────────────────────────────
struct DenialRecord {
  String uid;
  unsigned long firstDenialTime;
  int denialCount;
};
DenialRecord denialTracker[10];
int denialTrackerCount = 0;

int tapCodeFailCount = 0;
String lastTapCodeUID = "";
unsigned long lastTelegramTime = 0;
#define TELEGRAM_COOLDOWN 4000

// ─── Timing ──────────────────────────────────────────
#define TAPCODE_TOLERANCE 700

// ─────────────────────────────────────────────────────
// BUG 2 FIXED: UID reading with proper hex padding
// ─────────────────────────────────────────────────────
String readUID() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0"; // pad single digit
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

// ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(LED_RED,   OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE,  OUTPUT);
  pinMode(BUZZER,    OUTPUT);
  allLEDsOff();

  // LCD
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("SecureBit v1.0");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");

  // RC522
  SPI.begin();
  rfid.PCD_Init();
  delay(10);

  // WiFi
  lcd.clear();
  lcd.print("Connecting WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected: " + WiFi.localIP().toString());
    lcd.clear();
    lcd.print("WiFi Connected!");
    delay(1000);
  } else {
    Serial.println("\nWiFi FAILED");
    lcd.clear();
    lcd.print("WiFi Failed");
    lcd.setCursor(0, 1);
    lcd.print("Offline mode");
    delay(2000);
  }

  // Firebase
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Time sync — IST = UTC+5:30 = 19800 seconds
  configTime(19800, 0, "pool.ntp.org", "time.google.com");
  Serial.println("Waiting for NTP time sync...");

  // Wait up to 5 seconds for time
  struct tm timeinfo;
  int ntpAttempts = 0;
  while (!getLocalTime(&timeinfo) && ntpAttempts < 10) {
    delay(500);
    ntpAttempts++;
    Serial.print(".");
  }
  if (getLocalTime(&timeinfo)) {
    Serial.println("\nTime synced!");
  } else {
    Serial.println("\nNTP failed — will use millis() as fallback");
  }

  lcd.clear();
  lcd.print("System Ready");
  lcd.setCursor(0, 1);
  lcd.print("Scan your card");

  // Startup Telegram notification
  sendTelegram(
    "✅ *SecureBit System Online*\n"
    "━━━━━━━━━━━━━━━━━━━━\n"
    "📍 *Location:* Lab Entrance\n"
    "🕐 *Started:* " + getCurrentTime() + "\n"
    "━━━━━━━━━━━━━━━━━━━━\n"
    "NexaTag AI monitoring active ✓\n"
    "TapCode authentication enabled ✓"
  );
}

// ─────────────────────────────────────────────────────
void loop() {
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  // BUG 2 FIXED: use new padded readUID() function
  String uid = readUID();
  Serial.println("Card detected: " + uid);

  // Find card in authorized list
  int cardIndex = -1;
  for (int i = 0; i < totalAuthorized; i++) {
    if (authorizedCards[i].uid == uid) {
      cardIndex = i;
      break;
    }
  }

  if (cardIndex == -1) {
    handleUnauthorized(uid);
  } else {
    handleAuthorized(cardIndex, uid);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  delay(1000);
}

// ─────────────────────────────────────────────────────
void handleUnauthorized(String uid) {
  lcd.clear();
  lcd.print("ACCESS DENIED");
  lcd.setCursor(0, 1);
  lcd.print("UID:" + uid.substring(0, 8));

  setLED("red");
  buzz(200); buzz(200);

  logToFirebase(uid, "DENIED", 0, 0, false);
  checkBruteForce(uid);

  // Unknown card alert (not in system at all)
  sendTelegram(
    "👾 *UNKNOWN CARD DETECTED*\n"
    "━━━━━━━━━━━━━━━━━━━━\n"
    "💳 *UID:* `" + uid + "`\n"
    "⏰ *Time:* " + getCurrentTime() + "\n"
    "━━━━━━━━━━━━━━━━━━━━\n"
    "Card not in system — access denied"
  );

  delay(2000);
  setLED("off");
  lcd.clear();
  lcd.print("Scan your card");
}

// ─────────────────────────────────────────────────────
void handleAuthorized(int cardIndex, String uid) {
  lcd.clear();
  lcd.print("Card OK! Hold..");
  lcd.setCursor(0, 1);
  lcd.print("Count blinks...");

  // Halt card first — puts it in HALT state
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  unsigned long startTime = millis();
  int blinkCount = 0;

  // ── CARD PRESENCE LOOP ──────────────────────────
  // WakeupA (WUPA command) detects cards in HALT state
  // RequestA (REQA command) only detects IDLE — that's why it failed
  while (true) {
    byte bufferATQA[2];
    byte bufferSize = sizeof(bufferATQA);

    // WakeupA works on halted cards — CRITICAL DIFFERENCE
    MFRC522::StatusCode status = rfid.PICC_WakeupA(
                                   bufferATQA, &bufferSize);

    bool cardPresent = (status == MFRC522::STATUS_OK ||
                        status == MFRC522::STATUS_COLLISION);

    if (!cardPresent) {
      // Card removed — exit loop and evaluate
      break;
    }

    // Card still there — halt it again for next iteration
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();

    // Blink and count
    unsigned long elapsed = millis() - startTime;
    int currentSecond = elapsed / 1000;

    if (currentSecond > blinkCount) {
      blinkCount = currentSecond;

      setLED("blue");
      delay(150);
      setLED("off");

      lcd.setCursor(0, 1);
      lcd.print("Holding: ");
      lcd.print(blinkCount);
      lcd.print("s      ");

      Serial.println("Holding... " +
                     String(blinkCount) + "s");
    }

    delay(100); // poll every 100ms
  }

  // ── EVALUATE HOLD DURATION ───────────────────────
  unsigned long holdDuration = millis() - startTime;
  int holdSeconds = holdDuration / 1000;
  int expected    = authorizedCards[cardIndex].tapCode;
  int diff        = abs((int)holdDuration - (expected * 1000));

  // Print to Serial only — never show expected on LCD
  Serial.println("Card removed");
  Serial.println("Hold: "     + String(holdDuration) + "ms");
  Serial.println("Expected: " + String(expected * 1000) + "ms");
  Serial.println("Diff: "     + String(diff) + "ms");
  Serial.println("Tolerance: 700ms");

  if (diff <= TAPCODE_TOLERANCE) {
    // ── ACCESS GRANTED ───────────────────────────
    lcd.clear();
    lcd.print("ACCESS GRANTED");
    lcd.setCursor(0, 1);
    lcd.print("Hi " +
              authorizedCards[cardIndex].ownerName);

    setLED("green");
    buzz(500);
    logToFirebase(uid, "GRANTED",
                  holdSeconds, expected, false);

    tapCodeFailCount = 0;
    lastTapCodeUID   = "";

    delay(3000);
    setLED("off");

  } else {
    // ── WRONG TAPCODE ────────────────────────────
    lcd.clear();
    lcd.print("WRONG TAPCODE");
    lcd.setCursor(0, 1);

    // Count attempts before incrementing
    // so display shows correct remaining tries
    if (lastTapCodeUID == uid) {
      tapCodeFailCount++;
    } else {
      tapCodeFailCount = 1;
      lastTapCodeUID   = uid;
    }

    int triesLeft = 3 - tapCodeFailCount;
    triesLeft = max(triesLeft, 0);

    // Show tries left — NOT the expected value
    lcd.print("Tries left: " + String(triesLeft));

    setLED("red");
    buzz(100); delay(100);
    buzz(100); delay(100);
    buzz(100);

    logToFirebase(uid, "TAPCODE_FAIL",
                  holdSeconds, expected, false);

    if (tapCodeFailCount >= 3) {
      triggerLockout(uid, cardIndex);
      tapCodeFailCount = 0;
      lastTapCodeUID   = "";
    }

    delay(2000);
    setLED("off");
  }

  lcd.clear();
  lcd.print("Scan your card");
}

// ─────────────────────────────────────────────────────
void checkBruteForce(String uid) {
  unsigned long now = millis();
  int index = -1;

  for (int i = 0; i < denialTrackerCount; i++) {
    if (denialTracker[i].uid == uid) { index = i; break; }
  }

  if (index == -1) {
    if (denialTrackerCount < 10) {
      denialTracker[denialTrackerCount++] = {uid, now, 1};
    }
  } else {
    if (now - denialTracker[index].firstDenialTime < 60000) {
      denialTracker[index].denialCount++;
      if (denialTracker[index].denialCount >= 3) {
        String msg = "Card denied " +
                     String(denialTracker[index].denialCount) +
                     " times in 60 seconds";
        logAnomaly("BRUTE_FORCE", uid, msg);

        sendTelegram(
          "🚨 *BRUTE FORCE DETECTED*\n"
          "━━━━━━━━━━━━━━━━━━━━\n"
          "💳 *Card UID:* `" + uid + "`\n"
          "🔢 *Attempts:* " +
          String(denialTracker[index].denialCount) +
          " denials in 60s\n"
          "⏰ *Time:* " + getCurrentTime() + "\n"
          "━━━━━━━━━━━━━━━━━━━━\n"
          "⚡ Check entrance immediately!"
        );

        denialTracker[index].denialCount = 0;
        denialTracker[index].firstDenialTime = now;
      }
    } else {
      denialTracker[index].firstDenialTime = now;
      denialTracker[index].denialCount = 1;
    }
  }
}

// ─────────────────────────────────────────────────────
void triggerLockout(String uid, int cardIndex) {
  String ownerName = (cardIndex >= 0) ?
                     authorizedCards[cardIndex].ownerName :
                     "Unknown";

  lcd.clear();
  lcd.print("!! LOCKOUT !!");
  lcd.setCursor(0, 1);
  lcd.print("Alert sent");

  for (int i = 0; i < 6; i++) {
    setLED("red"); delay(200);
    setLED("off"); delay(200);
  }

  logToFirebase(uid, "LOCKOUT", 0, 0, true);
  logAnomaly("TAPCODE_LOCKOUT", uid,
             "TapCode failed 3 times — card locked");

  sendTelegram(
    "🔒 *TAPCODE LOCKOUT*\n"
    "━━━━━━━━━━━━━━━━━━━━\n"
    "💳 *Card:* `" + uid + "`\n"
    "👤 *Registered to:* " + ownerName + "\n"
    "❌ *Reason:* 3 consecutive wrong TapCodes\n"
    "⏰ *Time:* " + getCurrentTime() + "\n"
    "━━━━━━━━━━━━━━━━━━━━\n"
    "⚠️ Card may be stolen or under attack"
  );
}

// ─────────────────────────────────────────────────────
// BUG 4 FIXED: no longer silently exits if NTP fails
// Uses millis() as fallback timestamp
// ─────────────────────────────────────────────────────
void logToFirebase(String uid, String result,
                   int tapEntered, int tapExpected,
                   bool anomaly) {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Firebase: WiFi not connected");
    return;
  }

  // Try to get real time — use fallback if NTP not ready
  String timeStr = "";
  bool afterHours = false;
  bool weekend    = false;

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buf[25];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    timeStr   = String(buf);
    afterHours = (timeinfo.tm_hour >= 23 || timeinfo.tm_hour < 6);
    weekend    = (timeinfo.tm_wday == 0 || timeinfo.tm_wday == 6);
  } else {
    // Fallback: use uptime in milliseconds as timestamp
    timeStr = "uptime_" + String(millis() / 1000) + "s";
    Serial.println("NTP not ready — using uptime as timestamp");
  }

  // Anomaly flags from time
  if (afterHours && result == "GRANTED") {
    anomaly = true;
    sendTelegram(
      "🌙 *AFTER-HOURS ACCESS*\n"
      "━━━━━━━━━━━━━━━━━━━━\n"
      "💳 *Card:* `" + uid + "`\n"
      "⏰ *Time:* " + timeStr + "\n"
      "━━━━━━━━━━━━━━━━━━━━\n"
      "Access granted outside permitted hours"
    );
  }
  if (weekend && result == "GRANTED") {
    anomaly = true;
    sendTelegram(
      "📅 *WEEKEND ACCESS*\n"
      "━━━━━━━━━━━━━━━━━━━━\n"
      "💳 *Card:* `" + uid + "`\n"
      "⏰ *Time:* " + timeStr + "\n"
      "━━━━━━━━━━━━━━━━━━━━\n"
      "Lab should be closed — verify access"
    );
  }

  // PATH FIXED: /logs/ matches HTML dashboard
  String path = "/logs/" + String(millis());

  FirebaseJson json;
  json.set("uid",              uid);
  json.set("timestamp",        timeStr);
  json.set("result",           result);
  json.set("tapcode_entered",  tapEntered);
  json.set("tapcode_expected", tapExpected);
  json.set("anomaly_flag",     anomaly);
  json.set("after_hours",      afterHours);
  json.set("weekend",          weekend);

  bool ok = Firebase.setJSON(fbdo, path, json);
  if (ok) {
    Serial.println("Firebase logged: " + result);
  } else {
    Serial.println("Firebase ERROR: " + fbdo.errorReason());
  }
}

// ─────────────────────────────────────────────────────
void logAnomaly(String type, String uid, String message) {
  if (WiFi.status() != WL_CONNECTED) return;

  String path = "/anomaly_alerts/" + String(millis());
  FirebaseJson json;
  json.set("type",      type);
  json.set("uid",       uid);
  json.set("message",   message);
  json.set("timestamp", getCurrentTime());

  bool ok = Firebase.setJSON(fbdo, path, json);
  Serial.println(ok ? "Anomaly logged: " + type
                    : "Anomaly log FAILED: " + fbdo.errorReason());
}

// ─────────────────────────────────────────────────────
void sendTelegram(String message) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastTelegramTime < TELEGRAM_COOLDOWN) {
    Serial.println("Telegram: rate limited");
    return;
  }
  lastTelegramTime = millis();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = "https://api.telegram.org/bot";
  url += BOT_TOKEN;
  url += "/sendMessage";

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  // Escape special characters for Markdown
  message.replace("_", "\\_");
  message.replace("-", "\\-");
  message.replace(".", "\\.");

  String payload = "{\"chat_id\":\"" + String(CHAT_ID) +
                   "\",\"text\":\"" + message +
                   "\",\"parse_mode\":\"Markdown\"}";

  int code = http.POST(payload);
  Serial.println(code == 200 ? "Telegram sent ✓"
                             : "Telegram failed: " + String(code));
  http.end();
}

// ─────────────────────────────────────────────────────
String getCurrentTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "Time unavailable";
  char buf[25];
  strftime(buf, sizeof(buf), "%d %b %Y  %H:%M:%S", &timeinfo);
  return String(buf);
}

// ─────────────────────────────────────────────────────
void setLED(String color) {
  // Common anode: LOW = ON, HIGH = OFF
  if (color == "red") {
    digitalWrite(LED_RED, LOW); digitalWrite(LED_GREEN, HIGH); digitalWrite(LED_BLUE, HIGH);
  } else if (color == "green") {
    digitalWrite(LED_RED, HIGH); digitalWrite(LED_GREEN, LOW); digitalWrite(LED_BLUE, HIGH);
  } else if (color == "blue") {
    digitalWrite(LED_RED, HIGH); digitalWrite(LED_GREEN, HIGH); digitalWrite(LED_BLUE, LOW);
  } else if (color == "white") {
    digitalWrite(LED_RED, LOW); digitalWrite(LED_GREEN, LOW); digitalWrite(LED_BLUE, LOW);
  } else {
    allLEDsOff();
  }
}

void allLEDsOff() {
  digitalWrite(LED_RED, HIGH);
  digitalWrite(LED_GREEN, HIGH);
  digitalWrite(LED_BLUE, HIGH);
}

void buzz(int duration) {
  digitalWrite(BUZZER, HIGH);
  delay(duration);
  digitalWrite(BUZZER, LOW);
  delay(50);
}