#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Secrets.h>

const char* hostname = "c3printer";
#define TWITCH_CHANNEL "daverdavid"

WebServer server(80);
WiFiClientSecure twitchClient;
Preferences preferences;

static BLEUUID serviceUUID("49535343-fe7d-4ae5-8fa9-9fafd205e455");
static BLEUUID charWriteUUID("49535343-8841-43f4-a8d4-ecbe34729bb3");
static BLEUUID charNotifyUUID("49535343-1e4d-4bd9-ba61-23c647249616");

String printerMAC = "56:17:a1:30:0d:dc";
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteCharacteristic = nullptr;
bool printerConnected = false;
bool twitchConnected = false;
unsigned long lastTwitchPing = 0;

const int PRINTER_WIDTH = 400; // dots
const int PRINTER_WIDTH_BYTES = PRINTER_WIDTH / 8; // 48 bytes

struct TwitchConfig {
  // Event enables
  bool enableSubs = true;
  bool enableBits = true;
  bool enablePoints = true;
  bool enableRaids = true;
  
  // Messages
  String subsMessage = "NEW SUB: {user}!";
  String bitsMessage = "BITS: {user} x{amount}";
  String pointsMessage = "{user} redeemed: {reward}";
  String raidsMessage = "RAID from {user}!";
  
  // Subs formatting
  int subsSize = 3;
  int subsAlign = 1;
  bool subsBold = true;
  int subsFeed = 3;
  
  // Bits formatting
  int bitsSize = 3;
  int bitsAlign = 1;
  bool bitsBold = true;
  int bitsFeed = 3;
  
  // Points formatting
  int pointsSize = 3;
  int pointsAlign = 1;
  bool pointsBold = true;
  int pointsFeed = 3;
  
  // Raids formatting
  int raidsSize = 4;
  int raidsAlign = 1;
  bool raidsBold = true;
  int raidsFeed = 3;
} twitchCfg;

// ========== BITMAP CANVAS ==========

class PrintCanvas : public Adafruit_GFX {
public:
  uint8_t *buffer;
  int bufferSize;
  
  PrintCanvas(int16_t w, int16_t h) : Adafruit_GFX(w, h) {
    bufferSize = (w / 8) * h;
    buffer = (uint8_t*)malloc(bufferSize);
    if(buffer) {
      memset(buffer, 0, bufferSize);
    }
  }
  
  ~PrintCanvas() {
    if(buffer) free(buffer);
  }
  
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if(!buffer || x < 0 || x >= _width || y < 0 || y >= _height) return;
    int byteIndex = (y * (_width / 8)) + (x / 8);
    int bitIndex = 7 - (x % 8);
    if(color) {
      buffer[byteIndex] |= (1 << bitIndex);
    } else {
      buffer[byteIndex] &= ~(1 << bitIndex);
    }
  }
  
  void clear() {
    if(buffer) memset(buffer, 0, bufferSize);
  }
};

// ========== TEXT PROCESSING ==========

String processNewlines(String text) {
  text.replace("\\n", "\n");
  text.replace("{nl}", "\n");
  return text;
}

// Clean text for printing - remove control characters
String sanitizeText(String text) {
  String result = "";
  
  for(int i = 0; i < text.length(); i++) {
    char c = text.charAt(i);
    
    // Keep printable ASCII, newlines
    if((c >= 32 && c <= 126) || c == '\n' || c == '\r') {
      result += c;
    }
    // Skip control characters
    else if(c < 32 || c == 127) {
      continue;
    }
    // For UTF-8 multibyte sequences, replace with space
    else if((c & 0x80) != 0) {
      result += ' ';
    }
  }
  
  return result;
}

// Word wrap text to fit within max width
String wordWrap(String text, int maxWidth, int fontSize) {
  String result = "";
  int charWidth = 6 * fontSize; // Approximate character width
  int maxCharsPerLine = maxWidth / charWidth;
  
  if(maxCharsPerLine < 5) maxCharsPerLine = 5; // Minimum
  
  // Process line by line (preserve existing newlines)
  int lineStart = 0;
  while(lineStart < text.length()) {
    int lineEnd = text.indexOf('\n', lineStart);
    if(lineEnd < 0) lineEnd = text.length();
    
    String line = text.substring(lineStart, lineEnd);
    
    // If line is short enough, keep it
    if(line.length() <= maxCharsPerLine) {
      result += line;
      if(lineEnd < text.length()) result += "\n";
    }
    // Otherwise, wrap it
    else {
      while(line.length() > 0) {
        if(line.length() <= maxCharsPerLine) {
          result += line;
          break;
        }
        
        // Find last space within max width
        int breakPoint = maxCharsPerLine;
        int lastSpace = line.lastIndexOf(' ', breakPoint);
        
        if(lastSpace > 0 && lastSpace < breakPoint) {
          breakPoint = lastSpace;
        }
        
        // Add this chunk
        result += line.substring(0, breakPoint);
        result += "\n";
        
        // Continue with remainder
        line = line.substring(breakPoint);
        if(line.startsWith(" ")) line = line.substring(1); // Remove leading space
      }
      
      if(lineEnd < text.length()) result += "\n";
    }
    
    lineStart = lineEnd + 1;
  }
  
  return result;
}

// ========== BITMAP PRINTING ==========

void sendCmd(const uint8_t* cmd, size_t len) {
  if(!printerConnected || !pWriteCharacteristic) return;
  pWriteCharacteristic->writeValue((uint8_t*)cmd, len);
  delay(10);
}

void initPrinter() {
  uint8_t init[] = {0x1B, 0x40};
  sendCmd(init, 2);
  delay(100);
  
  uint8_t utf8[] = {0x1B, 0x74, 0x10};
  sendCmd(utf8, 3);
  delay(50);
}

void printBitmap(uint8_t *bitmap, int width, int height) {
  if(!printerConnected || !bitmap) return;
  
  int widthBytes = width / 8;
  
  uint8_t cmd[] = {
    0x1D, 0x76, 0x30,
    0x00,
    (uint8_t)(widthBytes & 0xFF),
    (uint8_t)(widthBytes >> 8),
    (uint8_t)(height & 0xFF),
    (uint8_t)(height >> 8)
  };
  
  sendCmd(cmd, 8);
  delay(50);
  
  int totalBytes = widthBytes * height;
  int chunkSize = 200;
  
  for(int i = 0; i < totalBytes; i += chunkSize) {
    int remaining = totalBytes - i;
    int sendSize = (remaining < chunkSize) ? remaining : chunkSize;
    pWriteCharacteristic->writeValue(&bitmap[i], sendSize);
    delay(50);
  }
  
  Serial.printf("Bitmap printed: %dx%d (%d bytes)\n", width, height, totalBytes);
}

void feedPaper(int lines) {
  if(lines > 0 && lines < 256) {
    uint8_t cmd[] = {0x1B, 0x64, (uint8_t)lines};
    sendCmd(cmd, 3);
  }
}

void printToThermal(String text, int textSize=3, int align=1, bool bold=true, int feedLines=3) {
  if(!printerConnected) {
    Serial.println("Printer not connected!");
    return;
  }
  
  text = processNewlines(text);
  
  // Apply word wrapping BEFORE rendering
  int fontSize = constrain(textSize, 1, 8);
  int maxTextWidth = PRINTER_WIDTH - 6; // Leave margins
  text = wordWrap(text, maxTextWidth, fontSize);
  
  Serial.println("\n=== PRINTING (BITMAP) ===");
  Serial.println("Text after wrapping:");
  Serial.println(text);
  Serial.printf("Size:%d Align:%d Bold:%d Feed:%d\n", textSize, align, bold, feedLines);
  
  // Count lines after wrapping
  int lineCount = 1;
  for(int i = 0; i < text.length(); i++) {
    if(text[i] == '\n') lineCount++;
  }
  
  Serial.printf("Line count: %d\n", lineCount);
  
  // Calculate dimensions
  int charHeight = 8 * fontSize;
  int lineSpacing = 2 * fontSize;
  int lineHeight = charHeight + lineSpacing;
  int topMargin = fontSize * 2;
  int bottomMargin = fontSize * 2;
  int totalHeight = topMargin + (lineHeight * lineCount) + bottomMargin;
  
  // Cap max height
  if(totalHeight > 1200) {
    Serial.println("WARNING: Text too tall, capping at 1200px");
    totalHeight = 1200;
    lineCount = (totalHeight - topMargin - bottomMargin) / lineHeight;
  }
  
  Serial.printf("Canvas dimensions: %dx%d\n", PRINTER_WIDTH, totalHeight);
  
  // Create canvas
  PrintCanvas canvas(PRINTER_WIDTH, totalHeight);
  
  if(!canvas.buffer) {
    Serial.println("ERROR: Canvas allocation failed!");
    return;
  }
  
  canvas.setTextSize(fontSize);
  canvas.setTextColor(1);
  canvas.setTextWrap(false);
  
  // Render each line
  int currentY = topMargin;
  int lineStart = 0;
  int linesRendered = 0;
  
  for(int lineNum = 0; lineNum < lineCount && lineStart < text.length(); lineNum++) {
    int lineEnd = text.indexOf('\n', lineStart);
    if(lineEnd < 0) lineEnd = text.length();
    
    String line = text.substring(lineStart, lineEnd);
    
    if(line.length() > 0) {
      // Get text bounds
      int16_t x1, y1;
      uint16_t w, h;
      canvas.getTextBounds(line, 0, 0, &x1, &y1, &w, &h);
      
      // Constrain width to prevent overflow
      if(w > PRINTER_WIDTH - 0) {
        // Truncate if still too long
        while(line.length() > 0 && w > PRINTER_WIDTH - 0) {
          line = line.substring(0, line.length() - 1);
          canvas.getTextBounds(line, 0, 0, &x1, &y1, &w, &h);
        }
        Serial.printf("Line %d truncated to: '%s'\n", lineNum, line.c_str());
      }
      
      // Calculate X position
      int x = 2; // Default left
      if(align == 1) { // Center
        x = (PRINTER_WIDTH - w) / 2;
        if(x < 2) x = 2;
      } else if(align == 2) { // Right
        x = PRINTER_WIDTH - w - 2;
        if(x < 2) x = 2;
      }
      
      Serial.printf("Line %d: '%s' at (%d,%d) w=%d\n", lineNum, line.c_str(), x, currentY, w);
      
      // Draw text
      canvas.setCursor(x, currentY);
      canvas.print(line);
      
      // Bold effect
      if(bold) {
        canvas.setCursor(x + 1, currentY);
        canvas.print(line);
        canvas.setCursor(x, currentY + 1);
        canvas.print(line);
        if(fontSize >= 3) {
          canvas.setCursor(x + 1, currentY + 1);
          canvas.print(line);
        }
      }
      
      linesRendered++;
    }
    
    currentY += lineHeight;
    lineStart = lineEnd + 1;
  }
  
  Serial.printf("Rendered %d lines\n", linesRendered);
  
  // Print the bitmap
  printBitmap(canvas.buffer, PRINTER_WIDTH, totalHeight);
  
  // Feed paper
  feedPaper(feedLines);
  
  Serial.println("=== DONE ===\n");
}

// ========== TWITCH INTEGRATION ==========

void parseTwitchMessage(String msg) {
  Serial.println("Twitch RAW: " + msg);
  
  if(twitchCfg.enableSubs && msg.indexOf("msg-id=sub") > 0) {
    int userStart = msg.indexOf("display-name=") + 13;
    int userEnd = msg.indexOf(";", userStart);
    if(userEnd < 0) userEnd = msg.indexOf(" ", userStart);
    String username = msg.substring(userStart, userEnd);
    username = sanitizeText(username);
    
    String printMsg = twitchCfg.subsMessage;
    printMsg.replace("{user}", username);
    printMsg = sanitizeText(printMsg);
    
    Serial.println("Printing SUB: " + printMsg);
    printToThermal(printMsg, twitchCfg.subsSize, twitchCfg.subsAlign, 
                   twitchCfg.subsBold, twitchCfg.subsFeed);
    Serial.println("SUB complete: " + username);
  }
  
  if(twitchCfg.enableBits && msg.indexOf("bits=") > 0) {
    int bitsStart = msg.indexOf("bits=") + 5;
    int bitsEnd = msg.indexOf(";", bitsStart);
    if(bitsEnd < 0) bitsEnd = msg.indexOf(" ", bitsStart);
    String bitsAmount = msg.substring(bitsStart, bitsEnd);
    bitsAmount = sanitizeText(bitsAmount);
    
    int userStart = msg.indexOf("display-name=") + 13;
    int userEnd = msg.indexOf(";", userStart);
    if(userEnd < 0) userEnd = msg.indexOf(" ", userStart);
    String username = msg.substring(userStart, userEnd);
    username = sanitizeText(username);
    
    String printMsg = twitchCfg.bitsMessage;
    printMsg.replace("{user}", username);
    printMsg.replace("{amount}", bitsAmount);
    printMsg = sanitizeText(printMsg);
    
    Serial.println("Printing BITS: " + printMsg);
    printToThermal(printMsg, twitchCfg.bitsSize, twitchCfg.bitsAlign,
                   twitchCfg.bitsBold, twitchCfg.bitsFeed);
    Serial.println("BITS complete: " + username + " - " + bitsAmount);
  }
  
  if(twitchCfg.enablePoints && msg.indexOf("custom-reward-id=") > 0) {
    int userStart = msg.indexOf("display-name=") + 13;
    int userEnd = msg.indexOf(";", userStart);
    if(userEnd < 0) userEnd = msg.indexOf(" ", userStart);
    String username = msg.substring(userStart, userEnd);
    username = sanitizeText(username);
    
    int msgStart = msg.lastIndexOf(":") + 1;
    String rewardName = msg.substring(msgStart);
    rewardName.trim();
    rewardName = sanitizeText(rewardName);
    
    String printMsg = twitchCfg.pointsMessage;
    printMsg.replace("{user}", username);
    printMsg.replace("{reward}", rewardName);
    printMsg = sanitizeText(printMsg);
    
    Serial.println("Printing POINTS: " + printMsg);
    printToThermal(printMsg, twitchCfg.pointsSize, twitchCfg.pointsAlign,
                   twitchCfg.pointsBold, twitchCfg.pointsFeed);
    Serial.println("POINTS complete: " + username + " - " + rewardName);
  }
  
  if(twitchCfg.enableRaids && msg.indexOf("msg-id=raid") > 0) {
    int userStart = msg.indexOf("display-name=") + 13;
    int userEnd = msg.indexOf(";", userStart);
    if(userEnd < 0) userEnd = msg.indexOf(" ", userStart);
    String username = msg.substring(userStart, userEnd);
    username = sanitizeText(username);
    
    String printMsg = twitchCfg.raidsMessage;
    printMsg.replace("{user}", username);
    printMsg = sanitizeText(printMsg);
    
    Serial.println("Printing RAID: " + printMsg);
    printToThermal(printMsg, twitchCfg.raidsSize, twitchCfg.raidsAlign,
                   twitchCfg.raidsBold, twitchCfg.raidsFeed);
    Serial.println("RAID complete: " + username);
  }
}

void connectTwitch() {
  Serial.println("Connecting to Twitch IRC...");
  twitchClient.setInsecure();
  
  if(twitchClient.connect("irc.chat.twitch.tv", 6697)) {
    Serial.println("Connected to Twitch");
    twitchClient.println("PASS " TWITCH_OAUTH_SECRET);
    twitchClient.println("NICK " TWITCH_OAUTH_NICK);
    twitchClient.println("CAP REQ :twitch.tv/tags twitch.tv/commands");
    twitchClient.println("JOIN #" TWITCH_CHANNEL);
    twitchConnected = true;
    lastTwitchPing = millis();
    Serial.println("Joined #" TWITCH_CHANNEL);
  } else {
    Serial.println("Twitch connection failed");
    twitchConnected = false;
  }
}

void handleTwitchIRC() {
  if(!twitchConnected) return;
  
  while(twitchClient.available()) {
    String line = twitchClient.readStringUntil('\n');
    line.trim();
    
    if(line.length() > 0) {
      if(line.startsWith("PING")) {
        twitchClient.println("PONG :tmi.twitch.tv");
        lastTwitchPing = millis();
      }
      else if(line.indexOf("PRIVMSG") > 0 || line.indexOf("USERNOTICE") > 0) {
        parseTwitchMessage(line);
      }
    }
  }
  
  if(millis() - lastTwitchPing > 240000) {
    twitchClient.println("PING :tmi.twitch.tv");
    lastTwitchPing = millis();
  }
  
  if(!twitchClient.connected()) {
    Serial.println("Twitch disconnected, reconnecting...");
    twitchConnected = false;
    delay(5000);
    connectTwitch();
  }
}

// ========== BLE CONNECTION ==========

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    Serial.println("BLE Connected");
  }

  void onDisconnect(BLEClient* pclient) {
    printerConnected = false;
    Serial.println("BLE Disconnected");
  }
};

bool connectPrinter() {
  Serial.println("Connecting to printer: " + printerMAC);
  
  BLEDevice::init("ESP32-C3-Printer");
  
  if(pClient != nullptr) {
    delete pClient;
  }
  
  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());
  
  BLEAddress address(printerMAC.c_str());
  
  if(!pClient->connect(address)) {
    Serial.println("Connection failed");
    return false;
  }
  
  Serial.println("Connected! Discovering services...");
  
  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if(pRemoteService == nullptr) {
    Serial.println("Failed to find service");
    pClient->disconnect();
    return false;
  }
  
  pWriteCharacteristic = pRemoteService->getCharacteristic(charWriteUUID);
  if(pWriteCharacteristic == nullptr) {
    Serial.println("Failed to find write characteristic");
    pClient->disconnect();
    return false;
  }
  
  printerConnected = true;
  Serial.println("Printer characteristic found!");
  
  // IMPORTANT: Wait for printer to be ready
  delay(1000);
  
  // Initialize and clear printer buffer
  Serial.println("Initializing printer...");
  
  // Send multiple reset commands
  uint8_t init[] = {0x1B, 0x40};  // ESC @
  sendCmd(init, 2);
  delay(200);
  
  sendCmd(init, 2);  // Send twice for good measure
  delay(200);
  
  // Clear any buffered data by feeding blank lines
  uint8_t feed[] = {0x1B, 0x64, 0x05};  // Feed 5 lines
  sendCmd(feed, 3);
  delay(300);
  
  // Set UTF-8 encoding
  uint8_t utf8[] = {0x1B, 0x74, 0x10};
  sendCmd(utf8, 3);
  delay(100);
  
  // Send a small test pattern and feed it away
  const char* testStr = ".\n";
  pWriteCharacteristic->writeValue((uint8_t*)testStr, strlen(testStr));
  delay(100);
  
  sendCmd(feed, 3);  // Feed it away
  delay(200);
  
  Serial.println("Printer ready!");
  
  return true;
}

void disconnectPrinter() {
  if(pClient != nullptr && printerConnected) {
    pClient->disconnect();
  }
  printerConnected = false;
  Serial.println("Printer disconnected");
}

// ========== CONFIGURATION STORAGE ==========

void loadConfig() {
  preferences.begin("twitch", false);
  
  twitchCfg.enableSubs = preferences.getBool("esub", true);
  twitchCfg.enableBits = preferences.getBool("ebits", true);
  twitchCfg.enablePoints = preferences.getBool("epoints", true);
  twitchCfg.enableRaids = preferences.getBool("eraids", true);
  
  twitchCfg.subsMessage = preferences.getString("msub", "NEW SUB: {user}!");
  twitchCfg.bitsMessage = preferences.getString("mbits", "BITS: {user} x{amount}");
  twitchCfg.pointsMessage = preferences.getString("mpoints", "{user} redeemed: {reward}");
  twitchCfg.raidsMessage = preferences.getString("mraids", "RAID from {user}!");
  
  twitchCfg.subsSize = preferences.getInt("subs_sz", 3);
  twitchCfg.subsAlign = preferences.getInt("subs_al", 1);
  twitchCfg.subsBold = preferences.getBool("subs_b", true);
  twitchCfg.subsFeed = preferences.getInt("subs_f", 3);
  
  twitchCfg.bitsSize = preferences.getInt("bits_sz", 3);
  twitchCfg.bitsAlign = preferences.getInt("bits_al", 1);
  twitchCfg.bitsBold = preferences.getBool("bits_b", true);
  twitchCfg.bitsFeed = preferences.getInt("bits_f", 3);
  
  twitchCfg.pointsSize = preferences.getInt("pts_sz", 3);
  twitchCfg.pointsAlign = preferences.getInt("pts_al", 1);
  twitchCfg.pointsBold = preferences.getBool("pts_b", true);
  twitchCfg.pointsFeed = preferences.getInt("pts_f", 3);
  
  twitchCfg.raidsSize = preferences.getInt("raid_sz", 4);
  twitchCfg.raidsAlign = preferences.getInt("raid_al", 1);
  twitchCfg.raidsBold = preferences.getBool("raid_b", true);
  twitchCfg.raidsFeed = preferences.getInt("raid_f", 3);
  
  preferences.end();
  Serial.println("Configuration loaded");
}

void saveConfig() {
  preferences.begin("twitch", false);
  
  preferences.putBool("esub", twitchCfg.enableSubs);
  preferences.putBool("ebits", twitchCfg.enableBits);
  preferences.putBool("epoints", twitchCfg.enablePoints);
  preferences.putBool("eraids", twitchCfg.enableRaids);
  
  preferences.putString("msub", twitchCfg.subsMessage);
  preferences.putString("mbits", twitchCfg.bitsMessage);
  preferences.putString("mpoints", twitchCfg.pointsMessage);
  preferences.putString("mraids", twitchCfg.raidsMessage);
  
  preferences.putInt("subs_sz", twitchCfg.subsSize);
  preferences.putInt("subs_al", twitchCfg.subsAlign);
  preferences.putBool("subs_b", twitchCfg.subsBold);
  preferences.putInt("subs_f", twitchCfg.subsFeed);
  
  preferences.putInt("bits_sz", twitchCfg.bitsSize);
  preferences.putInt("bits_al", twitchCfg.bitsAlign);
  preferences.putBool("bits_b", twitchCfg.bitsBold);
  preferences.putInt("bits_f", twitchCfg.bitsFeed);
  
  preferences.putInt("pts_sz", twitchCfg.pointsSize);
  preferences.putInt("pts_al", twitchCfg.pointsAlign);
  preferences.putBool("pts_b", twitchCfg.pointsBold);
  preferences.putInt("pts_f", twitchCfg.pointsFeed);
  
  preferences.putInt("raid_sz", twitchCfg.raidsSize);
  preferences.putInt("raid_al", twitchCfg.raidsAlign);
  preferences.putBool("raid_b", twitchCfg.raidsBold);
  preferences.putInt("raid_f", twitchCfg.raidsFeed);
  
  preferences.end();
  Serial.println("Configuration saved");
}

// ========== WEB SERVER ==========

const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<title>ESP32-C3 Printer</title>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:Arial;max-width:700px;margin:20px auto;padding:10px;background:#f0f0f0}
.card{background:white;padding:15px;margin:10px 0;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}
h2{margin-top:0;color:#333;font-size:18px}
h3{margin:15px 0 10px 0;color:#555;font-size:14px;border-bottom:1px solid #ddd;padding-bottom:5px}
.status{padding:10px;border-radius:5px;margin:10px 0;font-weight:bold}
.connected{background:#d4edda;color:#155724}
.disconnected{background:#f8d7da;color:#721c24}
button{background:#007bff;color:white;border:none;padding:10px 20px;border-radius:5px;cursor:pointer;margin:5px}
button:hover{background:#0056b3}
button.danger{background:#dc3545}
button.danger:hover{background:#c82333}
button.success{background:#28a745}
button.success:hover{background:#218838}
input[type=text],textarea,select,input[type=number]{width:100%;padding:8px;margin:5px 0;border:1px solid #ddd;border-radius:4px;box-sizing:border-box;font-size:14px}
textarea{min-height:60px;font-family:monospace}
label{display:block;margin-top:8px;font-weight:bold;font-size:13px}
.row{display:flex;gap:10px;align-items:center;margin:8px 0}
.row>*{flex:1}
.note{font-size:11px;color:#666;font-style:italic;margin-top:5px}
.event-section{border:1px solid #e0e0e0;padding:10px;margin:10px 0;border-radius:5px;background:#f9f9f9}
</style>
</head>
<body>
<h1>🖨️ ESP32-C3 Thermal Printer</h1>

<div class="card">
<h2>Status</h2>
<div id="pstatus" class="status disconnected">Printer: Disconnected</div>
<div id="tstatus" class="status disconnected">Twitch: Disconnected</div>
<button onclick="connect()">🔌 Connect Printer</button>
<button class="danger" onclick="disconnect()">Disconnect</button>
</div>

<div class="card">
<h2>📺 Twitch Event Configuration</h2>

<div class="event-section">
<h3>📌 Subscriptions</h3>
<label><input type="checkbox" id="esub" checked> Enable</label>
<label>Message:</label>
<input type="text" id="msub" value="NEW SUB: {user}!">
<div class="row">
<div><label>Size:</label><select id="subs_sz">
<option value="1">1x</option><option value="2">2x</option><option value="3" selected>3x</option>
<option value="4">4x</option><option value="5">5x</option><option value="6">6x</option>
<option value="7">7x</option><option value="8">8x</option>
</select></div>
<div><label>Align:</label><select id="subs_al">
<option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option>
</select></div>
</div>
<div class="row">
<div><label>Feed Lines:</label><input type="number" id="subs_f" value="3" min="0" max="10"></div>
<div><label style="margin-top:20px"><input type="checkbox" id="subs_b" checked> Bold</label></div>
</div>
</div>

<div class="event-section">
<h3>💎 Bits/Cheers</h3>
<label><input type="checkbox" id="ebits" checked> Enable</label>
<label>Message:</label>
<input type="text" id="mbits" value="BITS: {user} x{amount}">
<div class="row">
<div><label>Size:</label><select id="bits_sz">
<option value="1">1x</option><option value="2">2x</option><option value="3" selected>3x</option>
<option value="4">4x</option><option value="5">5x</option><option value="6">6x</option>
<option value="7">7x</option><option value="8">8x</option>
</select></div>
<div><label>Align:</label><select id="bits_al">
<option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option>
</select></div>
</div>
<div class="row">
<div><label>Feed Lines:</label><input type="number" id="bits_f" value="3" min="0" max="10"></div>
<div><label style="margin-top:20px"><input type="checkbox" id="bits_b" checked> Bold</label></div>
</div>
</div>

<div class="event-section">
<h3>⭐ Channel Points</h3>
<label><input type="checkbox" id="epoints" checked> Enable</label>
<label>Message:</label>
<input type="text" id="mpoints" value="{user} redeemed: {reward}">
<div class="row">
<div><label>Size:</label><select id="pts_sz">
<option value="1">1x</option><option value="2">2x</option><option value="3" selected>3x</option>
<option value="4">4x</option><option value="5">5x</option><option value="6">6x</option>
<option value="7">7x</option><option value="8">8x</option>
</select></div>
<div><label>Align:</label><select id="pts_al">
<option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option>
</select></div>
</div>
<div class="row">
<div><label>Feed Lines:</label><input type="number" id="pts_f" value="3" min="0" max="10"></div>
<div><label style="margin-top:20px"><input type="checkbox" id="pts_b" checked> Bold</label></div>
</div>
</div>

<div class="event-section">
<h3>🎉 Raids</h3>
<label><input type="checkbox" id="eraids" checked> Enable</label>
<label>Message:</label>
<input type="text" id="mraids" value="RAID from {user}!">
<div class="row">
<div><label>Size:</label><select id="raid_sz">
<option value="1">1x</option><option value="2">2x</option><option value="3">3x</option>
<option value="4" selected>4x</option><option value="5">5x</option><option value="6">6x</option>
<option value="7">7x</option><option value="8">8x</option>
</select></div>
<div><label>Align:</label><select id="raid_al">
<option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option>
</select></div>
</div>
<div class="row">
<div><label>Feed Lines:</label><input type="number" id="raid_f" value="3" min="0" max="10"></div>
<div><label style="margin-top:20px"><input type="checkbox" id="raid_b" checked> Bold</label></div>
</div>
</div>

<button onclick="saveCfg()">💾 Save All Config</button>
<div class="note">Placeholders: {user}, {amount}, {reward} | New lines: \n or {nl}</div>
</div>

<div class="card">
<h2>📝 Manual Test Print</h2>
<textarea id="txt">Hello World!\nThis is a test</textarea>

<div class="row">
<div><label>Size:</label><select id="size">
<option value="1">1x</option><option value="2">2x</option><option value="3" selected>3x</option>
<option value="4">4x</option><option value="5">5x</option><option value="6">6x</option>
<option value="7">7x</option><option value="8">8x</option>
</select></div>
<div><label>Align:</label><select id="align">
<option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option>
</select></div>
</div>

<label><input type="checkbox" id="bold" checked> Bold</label>

<button onclick="print()">🖨️ Print Test</button>
<button onclick="feed()">📄 Feed 3 Lines</button>
</div>

<script>
function updateStatus(){
  fetch('/s').then(r=>r.json()).then(d=>{
    document.getElementById('pstatus').className='status '+(d.printer?'connected':'disconnected');
    document.getElementById('pstatus').innerText='Printer: '+(d.printer?'Connected':'Disconnected');
    document.getElementById('tstatus').className='status '+(d.twitch?'connected':'disconnected');
    document.getElementById('tstatus').innerText='Twitch: '+(d.twitch?'Connected':'Disconnected');
  });
}

function loadCfg(){
  fetch('/gcfg').then(r=>r.json()).then(d=>{
    document.getElementById('esub').checked=d.esub;
    document.getElementById('ebits').checked=d.ebits;
    document.getElementById('epoints').checked=d.epoints;
    document.getElementById('eraids').checked=d.eraids;
    document.getElementById('msub').value=d.msub;
    document.getElementById('mbits').value=d.mbits;
    document.getElementById('mpoints').value=d.mpoints;
    document.getElementById('mraids').value=d.mraids;
    
    document.getElementById('subs_sz').value=d.subs_sz;
    document.getElementById('subs_al').value=d.subs_al;
    document.getElementById('subs_b').checked=d.subs_b;
    document.getElementById('subs_f').value=d.subs_f;
    
    document.getElementById('bits_sz').value=d.bits_sz;
    document.getElementById('bits_al').value=d.bits_al;
    document.getElementById('bits_b').checked=d.bits_b;
    document.getElementById('bits_f').value=d.bits_f;
    
    document.getElementById('pts_sz').value=d.pts_sz;
    document.getElementById('pts_al').value=d.pts_al;
    document.getElementById('pts_b').checked=d.pts_b;
    document.getElementById('pts_f').value=d.pts_f;
    
    document.getElementById('raid_sz').value=d.raid_sz;
    document.getElementById('raid_al').value=d.raid_al;
    document.getElementById('raid_b').checked=d.raid_b;
    document.getElementById('raid_f').value=d.raid_f;
  });
}

function connect(){
  fetch('/c').then(r=>r.text()).then(d=>{alert(d);updateStatus();});
}

function disconnect(){
  fetch('/d').then(r=>r.text()).then(d=>{alert(d);updateStatus();});
}

function print(){
  let fd=new FormData();
  fd.append('txt',document.getElementById('txt').value);
  fd.append('size',document.getElementById('size').value);
  fd.append('align',document.getElementById('align').value);
  fd.append('bold',document.getElementById('bold').checked?'1':'0');
  fetch('/p',{method:'POST',body:fd}).then(r=>r.text()).then(d=>alert(d));
}

function feed(){
  fetch('/f?lines=3').then(r=>r.text()).then(d=>alert(d));
}

function saveCfg(){
  let fd=new FormData();
  fd.append('esub',document.getElementById('esub').checked?'1':'0');
  fd.append('ebits',document.getElementById('ebits').checked?'1':'0');
  fd.append('epoints',document.getElementById('epoints').checked?'1':'0');
  fd.append('eraids',document.getElementById('eraids').checked?'1':'0');
  fd.append('msub',document.getElementById('msub').value);
  fd.append('mbits',document.getElementById('mbits').value);
  fd.append('mpoints',document.getElementById('mpoints').value);
  fd.append('mraids',document.getElementById('mraids').value);
  
  fd.append('subs_sz',document.getElementById('subs_sz').value);
  fd.append('subs_al',document.getElementById('subs_al').value);
  fd.append('subs_b',document.getElementById('subs_b').checked?'1':'0');
  fd.append('subs_f',document.getElementById('subs_f').value);
  
  fd.append('bits_sz',document.getElementById('bits_sz').value);
  fd.append('bits_al',document.getElementById('bits_al').value);
  fd.append('bits_b',document.getElementById('bits_b').checked?'1':'0');
  fd.append('bits_f',document.getElementById('bits_f').value);
  
  fd.append('pts_sz',document.getElementById('pts_sz').value);
  fd.append('pts_al',document.getElementById('pts_al').value);
  fd.append('pts_b',document.getElementById('pts_b').checked?'1':'0');
  fd.append('pts_f',document.getElementById('pts_f').value);
  
  fd.append('raid_sz',document.getElementById('raid_sz').value);
  fd.append('raid_al',document.getElementById('raid_al').value);
  fd.append('raid_b',document.getElementById('raid_b').checked?'1':'0');
  fd.append('raid_f',document.getElementById('raid_f').value);
  
  fetch('/tcfg',{method:'POST',body:fd}).then(r=>r.text()).then(d=>{alert(d);});
}

updateStatus();
setInterval(updateStatus,3000);
loadCfg();
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html; charset=UTF-8", htmlPage);
}

void handleStatus() {
  String json = "{\"printer\":" + String(printerConnected ? "true" : "false") + 
                ",\"twitch\":" + String(twitchConnected ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

void handleGetConfig() {
  String json = "{";
  json += "\"esub\":" + String(twitchCfg.enableSubs ? "true" : "false") + ",";
  json += "\"ebits\":" + String(twitchCfg.enableBits ? "true" : "false") + ",";
  json += "\"epoints\":" + String(twitchCfg.enablePoints ? "true" : "false") + ",";
  json += "\"eraids\":" + String(twitchCfg.enableRaids ? "true" : "false") + ",";
  json += "\"msub\":\"" + twitchCfg.subsMessage + "\",";
  json += "\"mbits\":\"" + twitchCfg.bitsMessage + "\",";
  json += "\"mpoints\":\"" + twitchCfg.pointsMessage + "\",";
  json += "\"mraids\":\"" + twitchCfg.raidsMessage + "\",";
  
  json += "\"subs_sz\":" + String(twitchCfg.subsSize) + ",";
  json += "\"subs_al\":" + String(twitchCfg.subsAlign) + ",";
  json += "\"subs_b\":" + String(twitchCfg.subsBold ? "true" : "false") + ",";
  json += "\"subs_f\":" + String(twitchCfg.subsFeed) + ",";
  
  json += "\"bits_sz\":" + String(twitchCfg.bitsSize) + ",";
  json += "\"bits_al\":" + String(twitchCfg.bitsAlign) + ",";
  json += "\"bits_b\":" + String(twitchCfg.bitsBold ? "true" : "false") + ",";
  json += "\"bits_f\":" + String(twitchCfg.bitsFeed) + ",";
  
  json += "\"pts_sz\":" + String(twitchCfg.pointsSize) + ",";
  json += "\"pts_al\":" + String(twitchCfg.pointsAlign) + ",";
  json += "\"pts_b\":" + String(twitchCfg.pointsBold ? "true" : "false") + ",";
  json += "\"pts_f\":" + String(twitchCfg.pointsFeed) + ",";
  
  json += "\"raid_sz\":" + String(twitchCfg.raidsSize) + ",";
  json += "\"raid_al\":" + String(twitchCfg.raidsAlign) + ",";
  json += "\"raid_b\":" + String(twitchCfg.raidsBold ? "true" : "false") + ",";
  json += "\"raid_f\":" + String(twitchCfg.raidsFeed);
  
  json += "}";
  server.send(200, "application/json", json);
}

void handleConnect() {
  if(connectPrinter()) {
    server.send(200, "text/plain", "Connected!");
  } else {
    server.send(500, "text/plain", "Connection failed");
  }
}

void handleDisconnect() {
  disconnectPrinter();
  server.send(200, "text/plain", "Disconnected");
}

void handlePrint() {
  if(!printerConnected) {
    server.send(400, "text/plain", "Printer not connected");
    return;
  }
  
  String text = server.arg("txt");
  int size = server.arg("size").toInt();
  int align = server.arg("align").toInt();
  bool bold = server.arg("bold") == "1";
  
  printToThermal(text, size, align, bold, 3);
  server.send(200, "text/plain", "Printed!");
}

void handleFeed() {
  if(!printerConnected) {
    server.send(400, "text/plain", "Not connected");
    return;
  }
  
  int lines = server.arg("lines").toInt();
  if(lines < 1) lines = 3;
  
  feedPaper(lines);
  server.send(200, "text/plain", "Fed " + String(lines) + " lines");
}

void handleTwitchConfig() {
  twitchCfg.enableSubs = server.arg("esub") == "1";
  twitchCfg.enableBits = server.arg("ebits") == "1";
  twitchCfg.enablePoints = server.arg("epoints") == "1";
  twitchCfg.enableRaids = server.arg("eraids") == "1";
  
  twitchCfg.subsMessage = server.arg("msub");
  twitchCfg.bitsMessage = server.arg("mbits");
  twitchCfg.pointsMessage = server.arg("mpoints");
  twitchCfg.raidsMessage = server.arg("mraids");
  
  twitchCfg.subsSize = server.arg("subs_sz").toInt();
  twitchCfg.subsAlign = server.arg("subs_al").toInt();
  twitchCfg.subsBold = server.arg("subs_b") == "1";
  twitchCfg.subsFeed = server.arg("subs_f").toInt();
  
  twitchCfg.bitsSize = server.arg("bits_sz").toInt();
  twitchCfg.bitsAlign = server.arg("bits_al").toInt();
  twitchCfg.bitsBold = server.arg("bits_b") == "1";
  twitchCfg.bitsFeed = server.arg("bits_f").toInt();
  
  twitchCfg.pointsSize = server.arg("pts_sz").toInt();
  twitchCfg.pointsAlign = server.arg("pts_al").toInt();
  twitchCfg.pointsBold = server.arg("pts_b") == "1";
  twitchCfg.pointsFeed = server.arg("pts_f").toInt();
  
  twitchCfg.raidsSize = server.arg("raid_sz").toInt();
  twitchCfg.raidsAlign = server.arg("raid_al").toInt();
  twitchCfg.raidsBold = server.arg("raid_b") == "1";
  twitchCfg.raidsFeed = server.arg("raid_f").toInt();
  
  saveConfig();
  server.send(200, "text/plain", "Config saved!");
}

// ========== SETUP & LOOP ==========

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\nESP32-C3 Thermal Printer (Bitmap Mode)");
  
  loadConfig();
  
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname);
  WiFi.begin(MYSSID, MYPSK);
  
  while(WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi OK");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  
  if(MDNS.begin(hostname)) {
    Serial.println(String("mDNS: http://") + hostname + ".local");
    MDNS.addService("http", "tcp", 80);
  }
  
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.begin();
  Serial.println("OTA Ready");
  
  connectTwitch();
  
  server.on("/", handleRoot);
  server.on("/s", handleStatus);
  server.on("/gcfg", handleGetConfig);
  server.on("/c", handleConnect);
  server.on("/d", handleDisconnect);
  server.on("/p", HTTP_POST, handlePrint);
  server.on("/f", handleFeed);
  server.on("/tcfg", HTTP_POST, handleTwitchConfig);
  server.begin();
  
  Serial.println("Ready!");
}

void loop() {
  ArduinoOTA.handle();
  handleTwitchIRC();
  server.handleClient();
  delay(10);
}
