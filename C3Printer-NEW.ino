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
unsigned long lastTwitchReconnect = 0;
unsigned long lastPrinterReconnect = 0;

const int PRINTER_WIDTH = 400;
const int PRINTER_WIDTH_BYTES = PRINTER_WIDTH / 8;

struct MessageLine {
  String text;
  int size;
  int align;
  bool bold;
  bool reverse;
};

struct TwitchConfig {
  // Event enables
  bool enableSubs = true;
  bool enableBits = true;
  bool enablePoints = true;
  bool enableRaids = true;

  // Subs - 3 lines
  MessageLine subsLine1 = {"NEW SUB:", 2, 1, true, false};
  MessageLine subsLine2 = {"{user}!", 4, 1, true, false};
  MessageLine subsLine3 = {"", 2, 1, false, false};
  int subsFeed = 3;

  // Bits - 3 lines
  MessageLine bitsLine1 = {"BITS!", 3, 1, true, false};
  MessageLine bitsLine2 = {"{user}", 2, 1, false, false};
  MessageLine bitsLine3 = {"x{amount}", 3, 1, true, false};
  int bitsFeed = 3;

  // Points - 3 lines
  MessageLine pointsLine1 = {"{user}", 2, 1, false, false};
  MessageLine pointsLine2 = {"redeemed:", 2, 1, false, false};
  MessageLine pointsLine3 = {"{reward}", 3, 1, true, false};
  int pointsFeed = 3;

  // Raids - 3 lines
  MessageLine raidsLine1 = {"RAID!", 4, 1, true, false};
  MessageLine raidsLine2 = {"from", 2, 1, false, false};
  MessageLine raidsLine3 = {"{user}!", 3, 1, true, false};
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

String sanitizeText(String text) {
  String result = "";
  for(int i = 0; i < text.length(); i++) {
    char c = text.charAt(i);
    if((c >= 32 && c <= 126) || c == '\n' || c == '\r') {
      result += c;
    }
    else if(c < 32 || c == 127) {
      continue;
    }
    else if((c & 0x80) != 0) {
      result += ' ';
    }
  }
  return result;
}

String wordWrap(String text, int maxWidth, int fontSize) {
  String result = "";
  int charWidth = 6 * fontSize;
  int maxCharsPerLine = maxWidth / charWidth;
  if(maxCharsPerLine < 5) maxCharsPerLine = 5;

  int lineStart = 0;
  while(lineStart < text.length()) {
    int lineEnd = text.indexOf('\n', lineStart);
    if(lineEnd < 0) lineEnd = text.length();

    String line = text.substring(lineStart, lineEnd);

    if(line.length() <= maxCharsPerLine) {
      result += line;
      if(lineEnd < text.length()) result += "\n";
    }
    else {
      while(line.length() > 0) {
        if(line.length() <= maxCharsPerLine) {
          result += line;
          break;
        }

        int breakPoint = maxCharsPerLine;
        int lastSpace = line.lastIndexOf(' ', breakPoint);
        if(lastSpace > 0 && lastSpace < breakPoint) {
          breakPoint = lastSpace;
        }

        result += line.substring(0, breakPoint);
        result += "\n";
        line = line.substring(breakPoint);
        if(line.startsWith(" ")) line = line.substring(1);
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

void printMultiLine(MessageLine lines[], int numLines, int feedLines) {
  if(!printerConnected) {
    Serial.println("Printer not connected!");
    return;
  }

  // Calculate total height needed
  int totalHeight = 0;
  for(int i = 0; i < numLines; i++) {
    if(lines[i].text.length() == 0) continue;

    String text = processNewlines(lines[i].text);
    int fontSize = constrain(lines[i].size, 1, 8);
    int maxTextWidth = PRINTER_WIDTH - 6;
    text = wordWrap(text, maxTextWidth, fontSize);

    int lineCount = 1;
    for(int j = 0; j < text.length(); j++) {
      if(text[j] == '\n') lineCount++;
    }

    int charHeight = 8 * fontSize;
    int lineSpacing = 2 * fontSize;
    int lineHeight = charHeight + lineSpacing;
    int topMargin = fontSize * 2;
    int bottomMargin = fontSize * 2;

    totalHeight += topMargin + (lineHeight * lineCount) + bottomMargin;
  }

  if(totalHeight > 1200) {
    Serial.println("WARNING: Text too tall, capping at 1200px");
    totalHeight = 1200;
  }

  PrintCanvas canvas(PRINTER_WIDTH, totalHeight);
  if(!canvas.buffer) {
    Serial.println("ERROR: Canvas allocation failed!");
    return;
  }

  int currentY = 0;

  // Render each message line
  for(int i = 0; i < numLines; i++) {
    if(lines[i].text.length() == 0) continue;

    String text = processNewlines(lines[i].text);
    int fontSize = constrain(lines[i].size, 1, 8);
    int maxTextWidth = PRINTER_WIDTH - 6;
    text = wordWrap(text, maxTextWidth, fontSize);

    canvas.setTextSize(fontSize);
    canvas.setTextColor(lines[i].reverse ? 0 : 1);
    canvas.setTextWrap(false);

    int lineCount = 1;
    for(int j = 0; j < text.length(); j++) {
      if(text[j] == '\n') lineCount++;
    }

    int charHeight = 8 * fontSize;
    int lineSpacing = 2 * fontSize;
    int lineHeight = charHeight + lineSpacing;
    int topMargin = fontSize * 2;

    currentY += topMargin;

    int lineStart = 0;
    for(int lineNum = 0; lineNum < lineCount && lineStart < text.length(); lineNum++) {
      int lineEnd = text.indexOf('\n', lineStart);
      if(lineEnd < 0) lineEnd = text.length();

      String line = text.substring(lineStart, lineEnd);
      if(line.length() > 0) {
        int16_t x1, y1;
        uint16_t w, h;
        canvas.getTextBounds(line, 0, 0, &x1, &y1, &w, &h);

        if(w > PRINTER_WIDTH - 0) {
          while(line.length() > 0 && w > PRINTER_WIDTH - 0) {
            line = line.substring(0, line.length() - 1);
            canvas.getTextBounds(line, 0, 0, &x1, &y1, &w, &h);
          }
        }

        int x = 2;
        if(lines[i].align == 1) {
          x = (PRINTER_WIDTH - w) / 2;
          if(x < 2) x = 2;
        } else if(lines[i].align == 2) {
          x = PRINTER_WIDTH - w - 2;
          if(x < 2) x = 2;
        }

        // Draw reverse background if needed
        if(lines[i].reverse) {
          canvas.fillRect(x - 2, currentY - 2, w + 4, h + 4, 1);
        }

        canvas.setCursor(x, currentY);
        canvas.print(line);

        if(lines[i].bold) {
          canvas.setCursor(x + 1, currentY);
          canvas.print(line);
          canvas.setCursor(x, currentY + 1);
          canvas.print(line);
          if(fontSize >= 3) {
            canvas.setCursor(x + 1, currentY + 1);
            canvas.print(line);
          }
        }
      }

      currentY += lineHeight;
      lineStart = lineEnd + 1;
    }

    int bottomMargin = fontSize * 2;
    currentY += bottomMargin;
  }

  printBitmap(canvas.buffer, PRINTER_WIDTH, totalHeight);
  feedPaper(feedLines);
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
    String username = sanitizeText(msg.substring(userStart, userEnd));

    MessageLine lines[3];
    lines[0] = twitchCfg.subsLine1;
    lines[1] = twitchCfg.subsLine2;
    lines[2] = twitchCfg.subsLine3;

    for(int i = 0; i < 3; i++) {
      lines[i].text.replace("{user}", username);
      lines[i].text = sanitizeText(lines[i].text);
    }

    printMultiLine(lines, 3, twitchCfg.subsFeed);
  }

  if(twitchCfg.enableBits && msg.indexOf("bits=") > 0) {
    int bitsStart = msg.indexOf("bits=") + 5;
    int bitsEnd = msg.indexOf(";", bitsStart);
    if(bitsEnd < 0) bitsEnd = msg.indexOf(" ", bitsStart);
    String bitsAmount = sanitizeText(msg.substring(bitsStart, bitsEnd));

    int userStart = msg.indexOf("display-name=") + 13;
    int userEnd = msg.indexOf(";", userStart);
    if(userEnd < 0) userEnd = msg.indexOf(" ", userStart);
    String username = sanitizeText(msg.substring(userStart, userEnd));

    MessageLine lines[3];
    lines[0] = twitchCfg.bitsLine1;
    lines[1] = twitchCfg.bitsLine2;
    lines[2] = twitchCfg.bitsLine3;

    for(int i = 0; i < 3; i++) {
      lines[i].text.replace("{user}", username);
      lines[i].text.replace("{amount}", bitsAmount);
      lines[i].text = sanitizeText(lines[i].text);
    }

    printMultiLine(lines, 3, twitchCfg.bitsFeed);
  }

  if(twitchCfg.enablePoints && msg.indexOf("custom-reward-id=") > 0) {
    int userStart = msg.indexOf("display-name=") + 13;
    int userEnd = msg.indexOf(";", userStart);
    if(userEnd < 0) userEnd = msg.indexOf(" ", userStart);
    String username = sanitizeText(msg.substring(userStart, userEnd));

    int msgStart = msg.lastIndexOf(":") + 1;
    String rewardName = sanitizeText(msg.substring(msgStart));
    rewardName.trim();

    MessageLine lines[3];
    lines[0] = twitchCfg.pointsLine1;
    lines[1] = twitchCfg.pointsLine2;
    lines[2] = twitchCfg.pointsLine3;

    for(int i = 0; i < 3; i++) {
      lines[i].text.replace("{user}", username);
      lines[i].text.replace("{reward}", rewardName);
      lines[i].text = sanitizeText(lines[i].text);
    }

    printMultiLine(lines, 3, twitchCfg.pointsFeed);
  }

  if(twitchCfg.enableRaids && msg.indexOf("msg-id=raid") > 0) {
    int userStart = msg.indexOf("display-name=") + 13;
    int userEnd = msg.indexOf(";", userStart);
    if(userEnd < 0) userEnd = msg.indexOf(" ", userStart);
    String username = sanitizeText(msg.substring(userStart, userEnd));

    MessageLine lines[3];
    lines[0] = twitchCfg.raidsLine1;
    lines[1] = twitchCfg.raidsLine2;
    lines[2] = twitchCfg.raidsLine3;

    for(int i = 0; i < 3; i++) {
      lines[i].text.replace("{user}", username);
      lines[i].text = sanitizeText(lines[i].text);
    }

    printMultiLine(lines, 3, twitchCfg.raidsFeed);
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
  // Auto-reconnect if disconnected
  if(!twitchConnected || !twitchClient.connected()) {
    if(millis() - lastTwitchReconnect > 10000) {  // Try every 10 seconds
      Serial.println("Twitch disconnected, reconnecting...");
      twitchConnected = false;
      connectTwitch();
      lastTwitchReconnect = millis();
    }
    return;
  }

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

  delay(1000);

  Serial.println("Initializing printer...");
  uint8_t init[] = {0x1B, 0x40};
  sendCmd(init, 2);
  delay(200);
  sendCmd(init, 2);
  delay(200);

  uint8_t feed[] = {0x1B, 0x64, 0x05};
  sendCmd(feed, 1);
  delay(300);

  uint8_t utf8[] = {0x1B, 0x74, 0x10};
  sendCmd(utf8, 3);
  delay(100);

  const char* testStr = ".\n";
  pWriteCharacteristic->writeValue((uint8_t*)testStr, strlen(testStr));
  delay(100);
  sendCmd(feed, 1);
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

void checkPrinterConnection() {
  // Auto-reconnect if disconnected
  if(!printerConnected) {
    if(millis() - lastPrinterReconnect > 15000) {  // Try every 15 seconds
      Serial.println("Printer disconnected, attempting reconnect...");
      connectPrinter();
      lastPrinterReconnect = millis();
    }
  }
}

// ========== CONFIGURATION STORAGE ==========
void loadConfig() {
  preferences.begin("twitch", false);
  
  twitchCfg.enableSubs = preferences.getBool("esub", true);
  twitchCfg.enableBits = preferences.getBool("ebits", true);
  twitchCfg.enablePoints = preferences.getBool("epoints", true);
  twitchCfg.enableRaids = preferences.getBool("eraids", true);
  
  // Load subs config
  twitchCfg.subsLine1.text = preferences.getString("sub1_txt", "NEW SUB:");
  twitchCfg.subsLine1.size = preferences.getInt("sub1_sz", 2);
  twitchCfg.subsLine1.align = preferences.getInt("sub1_al", 1);
  twitchCfg.subsLine1.bold = preferences.getBool("sub1_b", true);
  twitchCfg.subsLine1.reverse = preferences.getBool("sub1_r", false);
  
  twitchCfg.subsLine2.text = preferences.getString("sub2_txt", "{user}!");
  twitchCfg.subsLine2.size = preferences.getInt("sub2_sz", 4);
  twitchCfg.subsLine2.align = preferences.getInt("sub2_al", 1);
  twitchCfg.subsLine2.bold = preferences.getBool("sub2_b", true);
  twitchCfg.subsLine2.reverse = preferences.getBool("sub2_r", false);
  
  twitchCfg.subsLine3.text = preferences.getString("sub3_txt", "");
  twitchCfg.subsLine3.size = preferences.getInt("sub3_sz", 2);
  twitchCfg.subsLine3.align = preferences.getInt("sub3_al", 1);
  twitchCfg.subsLine3.bold = preferences.getBool("sub3_b", false);
  twitchCfg.subsLine3.reverse = preferences.getBool("sub3_r", false);
  
  twitchCfg.subsFeed = preferences.getInt("subs_f", 3);
  
  // Load bits config
  twitchCfg.bitsLine1.text = preferences.getString("bit1_txt", "BITS!");
  twitchCfg.bitsLine1.size = preferences.getInt("bit1_sz", 3);
  twitchCfg.bitsLine1.align = preferences.getInt("bit1_al", 1);
  twitchCfg.bitsLine1.bold = preferences.getBool("bit1_b", true);
  twitchCfg.bitsLine1.reverse = preferences.getBool("bit1_r", false);
  
  twitchCfg.bitsLine2.text = preferences.getString("bit2_txt", "{user}");
  twitchCfg.bitsLine2.size = preferences.getInt("bit2_sz", 2);
  twitchCfg.bitsLine2.align = preferences.getInt("bit2_al", 1);
  twitchCfg.bitsLine2.bold = preferences.getBool("bit2_b", false);
  twitchCfg.bitsLine2.reverse = preferences.getBool("bit2_r", false);
  
  twitchCfg.bitsLine3.text = preferences.getString("bit3_txt", "x{amount}");
  twitchCfg.bitsLine3.size = preferences.getInt("bit3_sz", 3);
  twitchCfg.bitsLine3.align = preferences.getInt("bit3_al", 1);
  twitchCfg.bitsLine3.bold = preferences.getBool("bit3_b", true);
  twitchCfg.bitsLine3.reverse = preferences.getBool("bit3_r", false);
  
  twitchCfg.bitsFeed = preferences.getInt("bits_f", 3);
  
  // Load points config
  twitchCfg.pointsLine1.text = preferences.getString("pts1_txt", "{user}");
  twitchCfg.pointsLine1.size = preferences.getInt("pts1_sz", 2);
  twitchCfg.pointsLine1.align = preferences.getInt("pts1_al", 1);
  twitchCfg.pointsLine1.bold = preferences.getBool("pts1_b", false);
  twitchCfg.pointsLine1.reverse = preferences.getBool("pts1_r", false);
  
  twitchCfg.pointsLine2.text = preferences.getString("pts2_txt", "redeemed:");
  twitchCfg.pointsLine2.size = preferences.getInt("pts2_sz", 2);
  twitchCfg.pointsLine2.align = preferences.getInt("pts2_al", 1);
  twitchCfg.pointsLine2.bold = preferences.getBool("pts2_b", false);
  twitchCfg.pointsLine2.reverse = preferences.getBool("pts2_r", false);
  
  twitchCfg.pointsLine3.text = preferences.getString("pts3_txt", "{reward}");
  twitchCfg.pointsLine3.size = preferences.getInt("pts3_sz", 3);
  twitchCfg.pointsLine3.align = preferences.getInt("pts3_al", 1);
  twitchCfg.pointsLine3.bold = preferences.getBool("pts3_b", true);
  twitchCfg.pointsLine3.reverse = preferences.getBool("pts3_r", false);
  
  twitchCfg.pointsFeed = preferences.getInt("pts_f", 3);
  
  // Load raids config
  twitchCfg.raidsLine1.text = preferences.getString("raid1_txt", "RAID!");
  twitchCfg.raidsLine1.size = preferences.getInt("raid1_sz", 4);
  twitchCfg.raidsLine1.align = preferences.getInt("raid1_al", 1);
  twitchCfg.raidsLine1.bold = preferences.getBool("raid1_b", true);
  twitchCfg.raidsLine1.reverse = preferences.getBool("raid1_r", false);
  
  twitchCfg.raidsLine2.text = preferences.getString("raid2_txt", "from");
  twitchCfg.raidsLine2.size = preferences.getInt("raid2_sz", 2);
  twitchCfg.raidsLine2.align = preferences.getInt("raid2_al", 1);
  twitchCfg.raidsLine2.bold = preferences.getBool("raid2_b", false);
  twitchCfg.raidsLine2.reverse = preferences.getBool("raid2_r", false);
  
  twitchCfg.raidsLine3.text = preferences.getString("raid3_txt", "{user}!");
  twitchCfg.raidsLine3.size = preferences.getInt("raid3_sz", 3);
  twitchCfg.raidsLine3.align = preferences.getInt("raid3_al", 1);
  twitchCfg.raidsLine3.bold = preferences.getBool("raid3_b", true);
  twitchCfg.raidsLine3.reverse = preferences.getBool("raid3_r", false);
  
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
  
  // Save subs
  preferences.putString("sub1_txt", twitchCfg.subsLine1.text);
  preferences.putInt("sub1_sz", twitchCfg.subsLine1.size);
  preferences.putInt("sub1_al", twitchCfg.subsLine1.align);
  preferences.putBool("sub1_b", twitchCfg.subsLine1.bold);
  preferences.putBool("sub1_r", twitchCfg.subsLine1.reverse);
  
  preferences.putString("sub2_txt", twitchCfg.subsLine2.text);
  preferences.putInt("sub2_sz", twitchCfg.subsLine2.size);
  preferences.putInt("sub2_al", twitchCfg.subsLine2.align);
  preferences.putBool("sub2_b", twitchCfg.subsLine2.bold);
  preferences.putBool("sub2_r", twitchCfg.subsLine2.reverse);
  
  preferences.putString("sub3_txt", twitchCfg.subsLine3.text);
  preferences.putInt("sub3_sz", twitchCfg.subsLine3.size);
  preferences.putInt("sub3_al", twitchCfg.subsLine3.align);
  preferences.putBool("sub3_b", twitchCfg.subsLine3.bold);
  preferences.putBool("sub3_r", twitchCfg.subsLine3.reverse);
  
  preferences.putInt("subs_f", twitchCfg.subsFeed);
  
  // Save bits
  preferences.putString("bit1_txt", twitchCfg.bitsLine1.text);
  preferences.putInt("bit1_sz", twitchCfg.bitsLine1.size);
  preferences.putInt("bit1_al", twitchCfg.bitsLine1.align);
  preferences.putBool("bit1_b", twitchCfg.bitsLine1.bold);
  preferences.putBool("bit1_r", twitchCfg.bitsLine1.reverse);
  
  preferences.putString("bit2_txt", twitchCfg.bitsLine2.text);
  preferences.putInt("bit2_sz", twitchCfg.bitsLine2.size);
  preferences.putInt("bit2_al", twitchCfg.bitsLine2.align);
  preferences.putBool("bit2_b", twitchCfg.bitsLine2.bold);
  preferences.putBool("bit2_r", twitchCfg.bitsLine2.reverse);
  
  preferences.putString("bit3_txt", twitchCfg.bitsLine3.text);
  preferences.putInt("bit3_sz", twitchCfg.bitsLine3.size);
  preferences.putInt("bit3_al", twitchCfg.bitsLine3.align);
  preferences.putBool("bit3_b", twitchCfg.bitsLine3.bold);
  preferences.putBool("bit3_r", twitchCfg.bitsLine3.reverse);
  
  preferences.putInt("bits_f", twitchCfg.bitsFeed);
  
  // Save points
  preferences.putString("pts1_txt", twitchCfg.pointsLine1.text);
  preferences.putInt("pts1_sz", twitchCfg.pointsLine1.size);
  preferences.putInt("pts1_al", twitchCfg.pointsLine1.align);
  preferences.putBool("pts1_b", twitchCfg.pointsLine1.bold);
  preferences.putBool("pts1_r", twitchCfg.pointsLine1.reverse);
  
  preferences.putString("pts2_txt", twitchCfg.pointsLine2.text);
  preferences.putInt("pts2_sz", twitchCfg.pointsLine2.size);
  preferences.putInt("pts2_al", twitchCfg.pointsLine2.align);
  preferences.putBool("pts2_b", twitchCfg.pointsLine2.bold);
  preferences.putBool("pts2_r", twitchCfg.pointsLine2.reverse);
  
  preferences.putString("pts3_txt", twitchCfg.pointsLine3.text);
  preferences.putInt("pts3_sz", twitchCfg.pointsLine3.size);
  preferences.putInt("pts3_al", twitchCfg.pointsLine3.align);
  preferences.putBool("pts3_b", twitchCfg.pointsLine3.bold);
  preferences.putBool("pts3_r", twitchCfg.pointsLine3.reverse);
  
  preferences.putInt("pts_f", twitchCfg.pointsFeed);
  
  // Save raids
  preferences.putString("raid1_txt", twitchCfg.raidsLine1.text);
  preferences.putInt("raid1_sz", twitchCfg.raidsLine1.size);
  preferences.putInt("raid1_al", twitchCfg.raidsLine1.align);
  preferences.putBool("raid1_b", twitchCfg.raidsLine1.bold);
  preferences.putBool("raid1_r", twitchCfg.raidsLine1.reverse);
  
  preferences.putString("raid2_txt", twitchCfg.raidsLine2.text);
  preferences.putInt("raid2_sz", twitchCfg.raidsLine2.size);
  preferences.putInt("raid2_al", twitchCfg.raidsLine2.align);
  preferences.putBool("raid2_b", twitchCfg.raidsLine2.bold);
  preferences.putBool("raid2_r", twitchCfg.raidsLine2.reverse);
  
  preferences.putString("raid3_txt", twitchCfg.raidsLine3.text);
  preferences.putInt("raid3_sz", twitchCfg.raidsLine3.size);
  preferences.putInt("raid3_al", twitchCfg.raidsLine3.align);
  preferences.putBool("raid3_b", twitchCfg.raidsLine3.bold);
  preferences.putBool("raid3_r", twitchCfg.raidsLine3.reverse);
  
  preferences.putInt("raid_f", twitchCfg.raidsFeed);
  
  preferences.end();
  Serial.println("Configuration saved");
}

// ========== WEB SERVER HTML ==========
const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<title>ESP32-C3 Printer</title>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:Arial;margin:20px;background:#f0f0f0}
.container{max-width:800px;margin:0 auto;background:white;padding:20px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,0.1)}
h1{text-align:center;color:#333}
.status{padding:15px;background:#e7f3ff;border-radius:5px;margin-bottom:20px}
.status div{margin:5px 0}
.status .connected{color:green;font-weight:bold}
.status .disconnected{color:red;font-weight:bold}
button{padding:10px 20px;margin:5px;border:none;border-radius:5px;cursor:pointer;font-size:14px}
.btn-primary{background:#007bff;color:white}
.btn-danger{background:#dc3545;color:white}
.btn-success{background:#28a745;color:white}
button:hover{opacity:0.9}
.event-section{border:1px solid #ddd;padding:15px;margin:15px 0;border-radius:5px}
.event-header{font-size:18px;font-weight:bold;margin-bottom:10px}
.line-group{background:#f9f9f9;padding:10px;margin:8px 0;border-radius:4px;border-left:3px solid #007bff}
.line-label{font-weight:bold;color:#555;margin-bottom:5px}
input[type=text],textarea{width:100%;padding:8px;margin:5px 0;border:1px solid #ddd;border-radius:4px;box-sizing:border-box}
textarea{resize:vertical;min-height:40px}
.controls{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:5px}
.ctrl-group{display:flex;align-items:center;gap:4px}
.ctrl-label{font-size:12px;color:#666}
select{padding:4px 6px;border:1px solid #ddd;border-radius:3px;font-size:12px}
input[type=number]{width:50px;padding:4px;border:1px solid #ddd;border-radius:3px}
input[type=checkbox]{margin:0 4px}
.info{font-size:12px;color:#666;margin-top:10px;padding:8px;background:#f9f9f9;border-radius:4px}
</style>
</head>
<body>
<div class="container">
<h1>🖨️ ESP32-C3 Thermal Printer</h1>

<div class="status">
<h3>Status</h3>
<div>Printer: <span id="pstatus" class="disconnected">Disconnected</span></div>
<div>Twitch: <span id="tstatus" class="disconnected">Disconnected</span></div>
</div>

<button class="btn-primary" onclick="cmd('/c')">🔌 Connect Printer</button>
<button class="btn-danger" onclick="cmd('/d')">Disconnect</button>

<h2>📺 Twitch Event Configuration</h2>

<div class="event-section">
<div class="event-header">📌 Subscriptions <input type="checkbox" id="esub" checked></div>
<div class="line-group">
<div class="line-label">Line 1:</div>
<input type="text" id="sub1_txt" value="NEW SUB:" placeholder="Line 1 text">
<div class="controls">
<div class="ctrl-group"><span class="ctrl-label">Size:</span><select id="sub1_sz"><option>1</option><option selected>2</option><option>3</option><option>4</option><option>5</option><option>6</option><option>7</option><option>8</option></select></div>
<div class="ctrl-group"><span class="ctrl-label">Align:</span><select id="sub1_al"><option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option></select></div>
<div class="ctrl-group"><input type="checkbox" id="sub1_b" checked><span class="ctrl-label">Bold</span></div>
<div class="ctrl-group"><input type="checkbox" id="sub1_r"><span class="ctrl-label">Reverse</span></div>
</div>
</div>
<div class="line-group">
<div class="line-label">Line 2:</div>
<input type="text" id="sub2_txt" value="{user}!" placeholder="Line 2 text">
<div class="controls">
<div class="ctrl-group"><span class="ctrl-label">Size:</span><select id="sub2_sz"><option>1</option><option>2</option><option>3</option><option selected>4</option><option>5</option><option>6</option><option>7</option><option>8</option></select></div>
<div class="ctrl-group"><span class="ctrl-label">Align:</span><select id="sub2_al"><option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option></select></div>
<div class="ctrl-group"><input type="checkbox" id="sub2_b" checked><span class="ctrl-label">Bold</span></div>
<div class="ctrl-group"><input type="checkbox" id="sub2_r"><span class="ctrl-label">Reverse</span></div>
</div>
</div>
<div class="line-group">
<div class="line-label">Line 3:</div>
<input type="text" id="sub3_txt" value="" placeholder="Line 3 text (optional)">
<div class="controls">
<div class="ctrl-group"><span class="ctrl-label">Size:</span><select id="sub3_sz"><option>1</option><option selected>2</option><option>3</option><option>4</option><option>5</option><option>6</option><option>7</option><option>8</option></select></div>
<div class="ctrl-group"><span class="ctrl-label">Align:</span><select id="sub3_al"><option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option></select></div>
<div class="ctrl-group"><input type="checkbox" id="sub3_b"><span class="ctrl-label">Bold</span></div>
<div class="ctrl-group"><input type="checkbox" id="sub3_r"><span class="ctrl-label">Reverse</span></div>
</div>
</div>
<div class="controls">
<div class="ctrl-group"><span class="ctrl-label">Feed Lines:</span><input type="number" id="subs_f" value="3" min="0" max="10"></div>
</div>
</div>

<div class="event-section">
<div class="event-header">💎 Bits/Cheers <input type="checkbox" id="ebits" checked></div>
<div class="line-group">
<div class="line-label">Line 1:</div>
<input type="text" id="bit1_txt" value="BITS!" placeholder="Line 1 text">
<div class="controls">
<div class="ctrl-group"><span class="ctrl-label">Size:</span><select id="bit1_sz"><option>1</option><option>2</option><option selected>3</option><option>4</option><option>5</option><option>6</option><option>7</option><option>8</option></select></div>
<div class="ctrl-group"><span class="ctrl-label">Align:</span><select id="bit1_al"><option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option></select></div>
<div class="ctrl-group"><input type="checkbox" id="bit1_b" checked><span class="ctrl-label">Bold</span></div>
<div class="ctrl-group"><input type="checkbox" id="bit1_r"><span class="ctrl-label">Reverse</span></div>
</div>
</div>
<div class="line-group">
<div class="line-label">Line 2:</div>
<input type="text" id="bit2_txt" value="{user}" placeholder="Line 2 text">
<div class="controls">
<div class="ctrl-group"><span class="ctrl-label">Size:</span><select id="bit2_sz"><option>1</option><option selected>2</option><option>3</option><option>4</option><option>5</option><option>6</option><option>7</option><option>8</option></select></div>
<div class="ctrl-group"><span class="ctrl-label">Align:</span><select id="bit2_al"><option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option></select></div>
<div class="ctrl-group"><input type="checkbox" id="bit2_b"><span class="ctrl-label">Bold</span></div>
<div class="ctrl-group"><input type="checkbox" id="bit2_r"><span class="ctrl-label">Reverse</span></div>
</div>
</div>
<div class="line-group">
<div class="line-label">Line 3:</div>
<input type="text" id="bit3_txt" value="x{amount}" placeholder="Line 3 text">
<div class="controls">
<div class="ctrl-group"><span class="ctrl-label">Size:</span><select id="bit3_sz"><option>1</option><option>2</option><option selected>3</option><option>4</option><option>5</option><option>6</option><option>7</option><option>8</option></select></div>
<div class="ctrl-group"><span class="ctrl-label">Align:</span><select id="bit3_al"><option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option></select></div>
<div class="ctrl-group"><input type="checkbox" id="bit3_b" checked><span class="ctrl-label">Bold</span></div>
<div class="ctrl-group"><input type="checkbox" id="bit3_r"><span class="ctrl-label">Reverse</span></div>
</div>
</div>
<div class="controls">
<div class="ctrl-group"><span class="ctrl-label">Feed Lines:</span><input type="number" id="bits_f" value="3" min="0" max="10"></div>
</div>
</div>

<div class="event-section">
<div class="event-header">⭐ Channel Points <input type="checkbox" id="epoints" checked></div>
<div class="line-group">
<div class="line-label">Line 1:</div>
<input type="text" id="pts1_txt" value="{user}" placeholder="Line 1 text">
<div class="controls">
<div class="ctrl-group"><span class="ctrl-label">Size:</span><select id="pts1_sz"><option>1</option><option selected>2</option><option>3</option><option>4</option><option>5</option><option>6</option><option>7</option><option>8</option></select></div>
<div class="ctrl-group"><span class="ctrl-label">Align:</span><select id="pts1_al"><option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option></select></div>
<div class="ctrl-group"><input type="checkbox" id="pts1_b"><span class="ctrl-label">Bold</span></div>
<div class="ctrl-group"><input type="checkbox" id="pts1_r"><span class="ctrl-label">Reverse</span></div>
</div>
</div>
<div class="line-group">
<div class="line-label">Line 2:</div>
<input type="text" id="pts2_txt" value="redeemed:" placeholder="Line 2 text">
<div class="controls">
<div class="ctrl-group"><span class="ctrl-label">Size:</span><select id="pts2_sz"><option>1</option><option selected>2</option><option>3</option><option>4</option><option>5</option><option>6</option><option>7</option><option>8</option></select></div>
<div class="ctrl-group"><span class="ctrl-label">Align:</span><select id="pts2_al"><option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option></select></div>
<div class="ctrl-group"><input type="checkbox" id="pts2_b"><span class="ctrl-label">Bold</span></div>
<div class="ctrl-group"><input type="checkbox" id="pts2_r"><span class="ctrl-label">Reverse</span></div>
</div>
</div>
<div class="line-group">
<div class="line-label">Line 3:</div>
<input type="text" id="pts3_txt" value="{reward}" placeholder="Line 3 text">
<div class="controls">
<div class="ctrl-group"><span class="ctrl-label">Size:</span><select id="pts3_sz"><option>1</option><option>2</option><option selected>3</option><option>4</option><option>5</option><option>6</option><option>7</option><option>8</option></select></div>
<div class="ctrl-group"><span class="ctrl-label">Align:</span><select id="pts3_al"><option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option></select></div>
<div class="ctrl-group"><input type="checkbox" id="pts3_b" checked><span class="ctrl-label">Bold</span></div>
<div class="ctrl-group"><input type="checkbox" id="pts3_r"><span class="ctrl-label">Reverse</span></div>
</div>
</div>
<div class="controls">
<div class="ctrl-group"><span class="ctrl-label">Feed Lines:</span><input type="number" id="pts_f" value="3" min="0" max="10"></div>
</div>
</div>

<div class="event-section">
<div class="event-header">🎉 Raids <input type="checkbox" id="eraids" checked></div>
<div class="line-group">
<div class="line-label">Line 1:</div>
<input type="text" id="raid1_txt" value="RAID!" placeholder="Line 1 text">
<div class="controls">
<div class="ctrl-group"><span class="ctrl-label">Size:</span><select id="raid1_sz"><option>1</option><option>2</option><option>3</option><option selected>4</option><option>5</option><option>6</option><option>7</option><option>8</option></select></div>
<div class="ctrl-group"><span class="ctrl-label">Align:</span><select id="raid1_al"><option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option></select></div>
<div class="ctrl-group"><input type="checkbox" id="raid1_b" checked><span class="ctrl-label">Bold</span></div>
<div class="ctrl-group"><input type="checkbox" id="raid1_r"><span class="ctrl-label">Reverse</span></div>
</div>
</div>
<div class="line-group">
<div class="line-label">Line 2:</div>
<input type="text" id="raid2_txt" value="from" placeholder="Line 2 text">
<div class="controls">
<div class="ctrl-group"><span class="ctrl-label">Size:</span><select id="raid2_sz"><option>1</option><option selected>2</option><option>3</option><option>4</option><option>5</option><option>6</option><option>7</option><option>8</option></select></div>
<div class="ctrl-group"><span class="ctrl-label">Align:</span><select id="raid2_al"><option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option></select></div>
<div class="ctrl-group"><input type="checkbox" id="raid2_b"><span class="ctrl-label">Bold</span></div>
<div class="ctrl-group"><input type="checkbox" id="raid2_r"><span class="ctrl-label">Reverse</span></div>
</div>
</div>
<div class="line-group">
<div class="line-label">Line 3:</div>
<input type="text" id="raid3_txt" value="{user}!" placeholder="Line 3 text">
<div class="controls">
<div class="ctrl-group"><span class="ctrl-label">Size:</span><select id="raid3_sz"><option>1</option><option>2</option><option selected>3</option><option>4</option><option>5</option><option>6</option><option>7</option><option>8</option></select></div>
<div class="ctrl-group"><span class="ctrl-label">Align:</span><select id="raid3_al"><option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option></select></div>
<div class="ctrl-group"><input type="checkbox" id="raid3_b" checked><span class="ctrl-label">Bold</span></div>
<div class="ctrl-group"><input type="checkbox" id="raid3_r"><span class="ctrl-label">Reverse</span></div>
</div>
</div>
<div class="controls">
<div class="ctrl-group"><span class="ctrl-label">Feed Lines:</span><input type="number" id="raid_f" value="3" min="0" max="10"></div>
</div>
</div>

<button class="btn-success" onclick="saveAll()">💾 Save All Config</button>

<div class="info">
Placeholders: {user}, {amount}, {reward} | New lines: \n or {nl}
</div>

<h3>📝 Manual Test Print</h3>
<textarea id="testmsg">Hello World!\nThis is a test</textarea>
<div class="controls">
<div class="ctrl-group"><span class="ctrl-label">Size:</span><select id="testsz"><option>1</option><option>2</option><option selected>3</option><option>4</option><option>5</option><option>6</option><option>7</option><option>8</option></select></div>
<div class="ctrl-group"><span class="ctrl-label">Align:</span><select id="testal"><option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option></select></div>
<div class="ctrl-group"><input type="checkbox" id="testb" checked><span class="ctrl-label">Bold</span></div>
<div class="ctrl-group"><input type="checkbox" id="testr"><span class="ctrl-label">Reverse</span></div>
</div>
<button class="btn-primary" onclick="testPrint()">🖨️ Print Test</button>
<button class="btn-primary" onclick="cmd('/f')">📄 Feed 3 Lines</button>

</div>

<script>
function updateStatus(){
fetch('/s').then(r=>r.json()).then(d=>{
document.getElementById('pstatus').textContent=d.printer?'Connected':'Disconnected';
document.getElementById('pstatus').className=d.printer?'connected':'disconnected';
document.getElementById('tstatus').textContent=d.twitch?'Connected':'Disconnected';
document.getElementById('tstatus').className=d.twitch?'connected':'disconnected';
});
}
function cmd(url){
fetch(url).then(()=>setTimeout(updateStatus,1000));
}
function loadCfg(){
fetch('/gcfg').then(r=>r.json()).then(d=>{
document.getElementById('esub').checked=d.esub;
document.getElementById('ebits').checked=d.ebits;
document.getElementById('epoints').checked=d.epoints;
document.getElementById('eraids').checked=d.eraids;
document.getElementById('sub1_txt').value=d.sub1_txt;
document.getElementById('sub1_sz').value=d.sub1_sz;
document.getElementById('sub1_al').value=d.sub1_al;
document.getElementById('sub1_b').checked=d.sub1_b;
document.getElementById('sub1_r').checked=d.sub1_r;
document.getElementById('sub2_txt').value=d.sub2_txt;
document.getElementById('sub2_sz').value=d.sub2_sz;
document.getElementById('sub2_al').value=d.sub2_al;
document.getElementById('sub2_b').checked=d.sub2_b;
document.getElementById('sub2_r').checked=d.sub2_r;
document.getElementById('sub3_txt').value=d.sub3_txt;
document.getElementById('sub3_sz').value=d.sub3_sz;
document.getElementById('sub3_al').value=d.sub3_al;
document.getElementById('sub3_b').checked=d.sub3_b;
document.getElementById('sub3_r').checked=d.sub3_r;
document.getElementById('subs_f').value=d.subs_f;
document.getElementById('bit1_txt').value=d.bit1_txt;
document.getElementById('bit1_sz').value=d.bit1_sz;
document.getElementById('bit1_al').value=d.bit1_al;
document.getElementById('bit1_b').checked=d.bit1_b;
document.getElementById('bit1_r').checked=d.bit1_r;
document.getElementById('bit2_txt').value=d.bit2_txt;
document.getElementById('bit2_sz').value=d.bit2_sz;
document.getElementById('bit2_al').value=d.bit2_al;
document.getElementById('bit2_b').checked=d.bit2_b;
document.getElementById('bit2_r').checked=d.bit2_r;
document.getElementById('bit3_txt').value=d.bit3_txt;
document.getElementById('bit3_sz').value=d.bit3_sz;
document.getElementById('bit3_al').value=d.bit3_al;
document.getElementById('bit3_b').checked=d.bit3_b;
document.getElementById('bit3_r').checked=d.bit3_r;
document.getElementById('bits_f').value=d.bits_f;
document.getElementById('pts1_txt').value=d.pts1_txt;
document.getElementById('pts1_sz').value=d.pts1_sz;
document.getElementById('pts1_al').value=d.pts1_al;
document.getElementById('pts1_b').checked=d.pts1_b;
document.getElementById('pts1_r').checked=d.pts1_r;
document.getElementById('pts2_txt').value=d.pts2_txt;
document.getElementById('pts2_sz').value=d.pts2_sz;
document.getElementById('pts2_al').value=d.pts2_al;
document.getElementById('pts2_b').checked=d.pts2_b;
document.getElementById('pts2_r').checked=d.pts2_r;
document.getElementById('pts3_txt').value=d.pts3_txt;
document.getElementById('pts3_sz').value=d.pts3_sz;
document.getElementById('pts3_al').value=d.pts3_al;
document.getElementById('pts3_b').checked=d.pts3_b;
document.getElementById('pts3_r').checked=d.pts3_r;
document.getElementById('pts_f').value=d.pts_f;
document.getElementById('raid1_txt').value=d.raid1_txt;
document.getElementById('raid1_sz').value=d.raid1_sz;
document.getElementById('raid1_al').value=d.raid1_al;
document.getElementById('raid1_b').checked=d.raid1_b;
document.getElementById('raid1_r').checked=d.raid1_r;
document.getElementById('raid2_txt').value=d.raid2_txt;
document.getElementById('raid2_sz').value=d.raid2_sz;
document.getElementById('raid2_al').value=d.raid2_al;
document.getElementById('raid2_b').checked=d.raid2_b;
document.getElementById('raid2_r').checked=d.raid2_r;
document.getElementById('raid3_txt').value=d.raid3_txt;
document.getElementById('raid3_sz').value=d.raid3_sz;
document.getElementById('raid3_al').value=d.raid3_al;
document.getElementById('raid3_b').checked=d.raid3_b;
document.getElementById('raid3_r').checked=d.raid3_r;
document.getElementById('raid_f').value=d.raid_f;
});
}
function saveAll(){
let fd=new FormData();
fd.append('esub',document.getElementById('esub').checked?'1':'0');
fd.append('ebits',document.getElementById('ebits').checked?'1':'0');
fd.append('epoints',document.getElementById('epoints').checked?'1':'0');
fd.append('eraids',document.getElementById('eraids').checked?'1':'0');
fd.append('sub1_txt',document.getElementById('sub1_txt').value);
fd.append('sub1_sz',document.getElementById('sub1_sz').value);
fd.append('sub1_al',document.getElementById('sub1_al').value);
fd.append('sub1_b',document.getElementById('sub1_b').checked?'1':'0');
fd.append('sub1_r',document.getElementById('sub1_r').checked?'1':'0');
fd.append('sub2_txt',document.getElementById('sub2_txt').value);
fd.append('sub2_sz',document.getElementById('sub2_sz').value);
fd.append('sub2_al',document.getElementById('sub2_al').value);
fd.append('sub2_b',document.getElementById('sub2_b').checked?'1':'0');
fd.append('sub2_r',document.getElementById('sub2_r').checked?'1':'0');
fd.append('sub3_txt',document.getElementById('sub3_txt').value);
fd.append('sub3_sz',document.getElementById('sub3_sz').value);
fd.append('sub3_al',document.getElementById('sub3_al').value);
fd.append('sub3_b',document.getElementById('sub3_b').checked?'1':'0');
fd.append('sub3_r',document.getElementById('sub3_r').checked?'1':'0');
fd.append('subs_f',document.getElementById('subs_f').value);
fd.append('bit1_txt',document.getElementById('bit1_txt').value);
fd.append('bit1_sz',document.getElementById('bit1_sz').value);
fd.append('bit1_al',document.getElementById('bit1_al').value);
fd.append('bit1_b',document.getElementById('bit1_b').checked?'1':'0');
fd.append('bit1_r',document.getElementById('bit1_r').checked?'1':'0');
fd.append('bit2_txt',document.getElementById('bit2_txt').value);
fd.append('bit2_sz',document.getElementById('bit2_sz').value);
fd.append('bit2_al',document.getElementById('bit2_al').value);
fd.append('bit2_b',document.getElementById('bit2_b').checked?'1':'0');
fd.append('bit2_r',document.getElementById('bit2_r').checked?'1':'0');
fd.append('bit3_txt',document.getElementById('bit3_txt').value);
fd.append('bit3_sz',document.getElementById('bit3_sz').value);
fd.append('bit3_al',document.getElementById('bit3_al').value);
fd.append('bit3_b',document.getElementById('bit3_b').checked?'1':'0');
fd.append('bit3_r',document.getElementById('bit3_r').checked?'1':'0');
fd.append('bits_f',document.getElementById('bits_f').value);
fd.append('pts1_txt',document.getElementById('pts1_txt').value);
fd.append('pts1_sz',document.getElementById('pts1_sz').value);
fd.append('pts1_al',document.getElementById('pts1_al').value);
fd.append('pts1_b',document.getElementById('pts1_b').checked?'1':'0');
fd.append('pts1_r',document.getElementById('pts1_r').checked?'1':'0');
fd.append('pts2_txt',document.getElementById('pts2_txt').value);
fd.append('pts2_sz',document.getElementById('pts2_sz').value);
fd.append('pts2_al',document.getElementById('pts2_al').value);
fd.append('pts2_b',document.getElementById('pts2_b').checked?'1':'0');
fd.append('pts2_r',document.getElementById('pts2_r').checked?'1':'0');
fd.append('pts3_txt',document.getElementById('pts3_txt').value);
fd.append('pts3_sz',document.getElementById('pts3_sz').value);
fd.append('pts3_al',document.getElementById('pts3_al').value);
fd.append('pts3_b',document.getElementById('pts3_b').checked?'1':'0');
fd.append('pts3_r',document.getElementById('pts3_r').checked?'1':'0');
fd.append('pts_f',document.getElementById('pts_f').value);
fd.append('raid1_txt',document.getElementById('raid1_txt').value);
fd.append('raid1_sz',document.getElementById('raid1_sz').value);
fd.append('raid1_al',document.getElementById('raid1_al').value);
fd.append('raid1_b',document.getElementById('raid1_b').checked?'1':'0');
fd.append('raid1_r',document.getElementById('raid1_r').checked?'1':'0');
fd.append('raid2_txt',document.getElementById('raid2_txt').value);
fd.append('raid2_sz',document.getElementById('raid2_sz').value);
fd.append('raid2_al',document.getElementById('raid2_al').value);
fd.append('raid2_b',document.getElementById('raid2_b').checked?'1':'0');
fd.append('raid2_r',document.getElementById('raid2_r').checked?'1':'0');
fd.append('raid3_txt',document.getElementById('raid3_txt').value);
fd.append('raid3_sz',document.getElementById('raid3_sz').value);
fd.append('raid3_al',document.getElementById('raid3_al').value);
fd.append('raid3_b',document.getElementById('raid3_b').checked?'1':'0');
fd.append('raid3_r',document.getElementById('raid3_r').checked?'1':'0');
fd.append('raid_f',document.getElementById('raid_f').value);
fetch('/tcfg',{method:'POST',body:fd}).then(()=>alert('Config saved!'));
}
function testPrint(){
let fd=new FormData();
fd.append('txt',document.getElementById('testmsg').value);
fd.append('size',document.getElementById('testsz').value);
fd.append('align',document.getElementById('testal').value);
fd.append('bold',document.getElementById('testb').checked?'1':'0');
fd.append('reverse',document.getElementById('testr').checked?'1':'0');
fetch('/p',{method:'POST',body:fd}).then(()=>alert('Printing...'));
}
setInterval(updateStatus,3000);
updateStatus();
loadCfg();
</script>
</body>
</html>
)rawliteral";

// ========== WEB SERVER HANDLERS ==========
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

  json += "\"sub1_txt\":\"" + twitchCfg.subsLine1.text + "\",";
  json += "\"sub1_sz\":" + String(twitchCfg.subsLine1.size) + ",";
  json += "\"sub1_al\":" + String(twitchCfg.subsLine1.align) + ",";
  json += "\"sub1_b\":" + String(twitchCfg.subsLine1.bold ? "true" : "false") + ",";
  json += "\"sub1_r\":" + String(twitchCfg.subsLine1.reverse ? "true" : "false") + ",";

  json += "\"sub2_txt\":\"" + twitchCfg.subsLine2.text + "\",";
  json += "\"sub2_sz\":" + String(twitchCfg.subsLine2.size) + ",";
  json += "\"sub2_al\":" + String(twitchCfg.subsLine2.align) + ",";
  json += "\"sub2_b\":" + String(twitchCfg.subsLine2.bold ? "true" : "false") + ",";
  json += "\"sub2_r\":" + String(twitchCfg.subsLine2.reverse ? "true" : "false") + ",";

  json += "\"sub3_txt\":\"" + twitchCfg.subsLine3.text + "\",";
  json += "\"sub3_sz\":" + String(twitchCfg.subsLine3.size) + ",";
  json += "\"sub3_al\":" + String(twitchCfg.subsLine3.align) + ",";
  json += "\"sub3_b\":" + String(twitchCfg.subsLine3.bold ? "true" : "false") + ",";
  json += "\"sub3_r\":" + String(twitchCfg.subsLine3.reverse ? "true" : "false") + ",";

  json += "\"subs_f\":" + String(twitchCfg.subsFeed) + ",";

  json += "\"bit1_txt\":\"" + twitchCfg.bitsLine1.text + "\",";
  json += "\"bit1_sz\":" + String(twitchCfg.bitsLine1.size) + ",";
  json += "\"bit1_al\":" + String(twitchCfg.bitsLine1.align) + ",";
  json += "\"bit1_b\":" + String(twitchCfg.bitsLine1.bold ? "true" : "false") + ",";
  json += "\"bit1_r\":" + String(twitchCfg.bitsLine1.reverse ? "true" : "false") + ",";

  json += "\"bit2_txt\":\"" + twitchCfg.bitsLine2.text + "\",";
  json += "\"bit2_sz\":" + String(twitchCfg.bitsLine2.size) + ",";
  json += "\"bit2_al\":" + String(twitchCfg.bitsLine2.align) + ",";
  json += "\"bit2_b\":" + String(twitchCfg.bitsLine2.bold ? "true" : "false") + ",";
  json += "\"bit2_r\":" + String(twitchCfg.bitsLine2.reverse ? "true" : "false") + ",";

  json += "\"bit3_txt\":\"" + twitchCfg.bitsLine3.text + "\",";
  json += "\"bit3_sz\":" + String(twitchCfg.bitsLine3.size) + ",";
  json += "\"bit3_al\":" + String(twitchCfg.bitsLine3.align) + ",";
  json += "\"bit3_b\":" + String(twitchCfg.bitsLine3.bold ? "true" : "false") + ",";
  json += "\"bit3_r\":" + String(twitchCfg.bitsLine3.reverse ? "true" : "false") + ",";

  json += "\"bits_f\":" + String(twitchCfg.bitsFeed) + ",";

  json += "\"pts1_txt\":\"" + twitchCfg.pointsLine1.text + "\",";
  json += "\"pts1_sz\":" + String(twitchCfg.pointsLine1.size) + ",";
  json += "\"pts1_al\":" + String(twitchCfg.pointsLine1.align) + ",";
  json += "\"pts1_b\":" + String(twitchCfg.pointsLine1.bold ? "true" : "false") + ",";
  json += "\"pts1_r\":" + String(twitchCfg.pointsLine1.reverse ? "true" : "false") + ",";

  json += "\"pts2_txt\":\"" + twitchCfg.pointsLine2.text + "\",";
  json += "\"pts2_sz\":" + String(twitchCfg.pointsLine2.size) + ",";
  json += "\"pts2_al\":" + String(twitchCfg.pointsLine2.align) + ",";
  json += "\"pts2_b\":" + String(twitchCfg.pointsLine2.bold ? "true" : "false") + ",";
  json += "\"pts2_r\":" + String(twitchCfg.pointsLine2.reverse ? "true" : "false") + ",";

  json += "\"pts3_txt\":\"" + twitchCfg.pointsLine3.text + "\",";
  json += "\"pts3_sz\":" + String(twitchCfg.pointsLine3.size) + ",";
  json += "\"pts3_al\":" + String(twitchCfg.pointsLine3.align) + ",";
  json += "\"pts3_b\":" + String(twitchCfg.pointsLine3.bold ? "true" : "false") + ",";
  json += "\"pts3_r\":" + String(twitchCfg.pointsLine3.reverse ? "true" : "false") + ",";

  json += "\"pts_f\":" + String(twitchCfg.pointsFeed) + ",";

  json += "\"raid1_txt\":\"" + twitchCfg.raidsLine1.text + "\",";
  json += "\"raid1_sz\":" + String(twitchCfg.raidsLine1.size) + ",";
  json += "\"raid1_al\":" + String(twitchCfg.raidsLine1.align) + ",";
  json += "\"raid1_b\":" + String(twitchCfg.raidsLine1.bold ? "true" : "false") + ",";
  json += "\"raid1_r\":" + String(twitchCfg.raidsLine1.reverse ? "true" : "false") + ",";

  json += "\"raid2_txt\":\"" + twitchCfg.raidsLine2.text + "\",";
  json += "\"raid2_sz\":" + String(twitchCfg.raidsLine2.size) + ",";
  json += "\"raid2_al\":" + String(twitchCfg.raidsLine2.align) + ",";
  json += "\"raid2_b\":" + String(twitchCfg.raidsLine2.bold ? "true" : "false") + ",";
  json += "\"raid2_r\":" + String(twitchCfg.raidsLine2.reverse ? "true" : "false") + ",";

  json += "\"raid3_txt\":\"" + twitchCfg.raidsLine3.text + "\",";
  json += "\"raid3_sz\":" + String(twitchCfg.raidsLine3.size) + ",";
  json += "\"raid3_al\":" + String(twitchCfg.raidsLine3.align) + ",";
  json += "\"raid3_b\":" + String(twitchCfg.raidsLine3.bold ? "true" : "false") + ",";
  json += "\"raid3_r\":" + String(twitchCfg.raidsLine3.reverse ? "true" : "false") + ",";

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
  bool bold = (server.arg("bold") == "1");
  bool reverse = (server.arg("reverse") == "1");

  MessageLine lines[1];
  lines[0] = {text, size, align, bold, reverse};
  printMultiLine(lines, 1, 3);

  server.send(200, "text/plain", "Printed!");
}

void handleFeed() {
  if(!printerConnected) {
    server.send(400, "text/plain", "Not connected");
    return;
  }

  feedPaper(3);
  server.send(200, "text/plain", "Fed");
}

void handleTwitchConfig() {
  twitchCfg.enableSubs = (server.arg("esub") == "1");
  twitchCfg.enableBits = (server.arg("ebits") == "1");
  twitchCfg.enablePoints = (server.arg("epoints") == "1");
  twitchCfg.enableRaids = (server.arg("eraids") == "1");

  twitchCfg.subsLine1.text = server.arg("sub1_txt");
  twitchCfg.subsLine1.size = server.arg("sub1_sz").toInt();
  twitchCfg.subsLine1.align = server.arg("sub1_al").toInt();
  twitchCfg.subsLine1.bold = (server.arg("sub1_b") == "1");
  twitchCfg.subsLine1.reverse = (server.arg("sub1_r") == "1");

  twitchCfg.subsLine2.text = server.arg("sub2_txt");
  twitchCfg.subsLine2.size = server.arg("sub2_sz").toInt();
  twitchCfg.subsLine2.align = server.arg("sub2_al").toInt();
  twitchCfg.subsLine2.bold = (server.arg("sub2_b") == "1");
  twitchCfg.subsLine2.reverse = (server.arg("sub2_r") == "1");

  twitchCfg.subsLine3.text = server.arg("sub3_txt");
  twitchCfg.subsLine3.size = server.arg("sub3_sz").toInt();
  twitchCfg.subsLine3.align = server.arg("sub3_al").toInt();
  twitchCfg.subsLine3.bold = (server.arg("sub3_b") == "1");
  twitchCfg.subsLine3.reverse = (server.arg("sub3_r") == "1");

  twitchCfg.subsFeed = server.arg("subs_f").toInt();

  twitchCfg.bitsLine1.text = server.arg("bit1_txt");
  twitchCfg.bitsLine1.size = server.arg("bit1_sz").toInt();
  twitchCfg.bitsLine1.align = server.arg("bit1_al").toInt();
  twitchCfg.bitsLine1.bold = (server.arg("bit1_b") == "1");
  twitchCfg.bitsLine1.reverse = (server.arg("bit1_r") == "1");

  twitchCfg.bitsLine2.text = server.arg("bit2_txt");
  twitchCfg.bitsLine2.size = server.arg("bit2_sz").toInt();
  twitchCfg.bitsLine2.align = server.arg("bit2_al").toInt();
  twitchCfg.bitsLine2.bold = (server.arg("bit2_b") == "1");
  twitchCfg.bitsLine2.reverse = (server.arg("bit2_r") == "1");

  twitchCfg.bitsLine3.text = server.arg("bit3_txt");
  twitchCfg.bitsLine3.size = server.arg("bit3_sz").toInt();
  twitchCfg.bitsLine3.align = server.arg("bit3_al").toInt();
  twitchCfg.bitsLine3.bold = (server.arg("bit3_b") == "1");
  twitchCfg.bitsLine3.reverse = (server.arg("bit3_r") == "1");

  twitchCfg.bitsFeed = server.arg("bits_f").toInt();

  twitchCfg.pointsLine1.text = server.arg("pts1_txt");
  twitchCfg.pointsLine1.size = server.arg("pts1_sz").toInt();
  twitchCfg.pointsLine1.align = server.arg("pts1_al").toInt();
  twitchCfg.pointsLine1.bold = (server.arg("pts1_b") == "1");
  twitchCfg.pointsLine1.reverse = (server.arg("pts1_r") == "1");

  twitchCfg.pointsLine2.text = server.arg("pts2_txt");
  twitchCfg.pointsLine2.size = server.arg("pts2_sz").toInt();
  twitchCfg.pointsLine2.align = server.arg("pts2_al").toInt();
  twitchCfg.pointsLine2.bold = (server.arg("pts2_b") == "1");
  twitchCfg.pointsLine2.reverse = (server.arg("pts2_r") == "1");

  twitchCfg.pointsLine3.text = server.arg("pts3_txt");
  twitchCfg.pointsLine3.size = server.arg("pts3_sz").toInt();
  twitchCfg.pointsLine3.align = server.arg("pts3_al").toInt();
  twitchCfg.pointsLine3.bold = (server.arg("pts3_b") == "1");
  twitchCfg.pointsLine3.reverse = (server.arg("pts3_r") == "1");

  twitchCfg.pointsFeed = server.arg("pts_f").toInt();

  twitchCfg.raidsLine1.text = server.arg("raid1_txt");
  twitchCfg.raidsLine1.size = server.arg("raid1_sz").toInt();
  twitchCfg.raidsLine1.align = server.arg("raid1_al").toInt();
  twitchCfg.raidsLine1.bold = (server.arg("raid1_b") == "1");
  twitchCfg.raidsLine1.reverse = (server.arg("raid1_r") == "1");

  twitchCfg.raidsLine2.text = server.arg("raid2_txt");
  twitchCfg.raidsLine2.size = server.arg("raid2_sz").toInt();
  twitchCfg.raidsLine2.align = server.arg("raid2_al").toInt();
  twitchCfg.raidsLine2.bold = (server.arg("raid2_b") == "1");
  twitchCfg.raidsLine2.reverse = (server.arg("raid2_r") == "1");

  twitchCfg.raidsLine3.text = server.arg("raid3_txt");
  twitchCfg.raidsLine3.size = server.arg("raid3_sz").toInt();
  twitchCfg.raidsLine3.align = server.arg("raid3_al").toInt();
  twitchCfg.raidsLine3.bold = (server.arg("raid3_b") == "1");
  twitchCfg.raidsLine3.reverse = (server.arg("raid3_r") == "1");

  twitchCfg.raidsFeed = server.arg("raid_f").toInt();

  saveConfig();
  server.send(200, "text/plain", "Config saved!");
}

// ========== SETUP & LOOP ==========
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\nESP32-C3 Thermal Printer Starting...");

  loadConfig();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname);
  WiFi.begin(MYSSID, MYPSK);

  Serial.print("Connecting to WiFi");
  while(WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected!");
  Serial.println("IP: " + WiFi.localIP().toString());

  // mDNS setup
  if(MDNS.begin(hostname)) {
    Serial.println("mDNS started: http://" + String(hostname) + ".local");
    MDNS.addService("http", "tcp", 80);
  }

  // OTA setup
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.begin();
  Serial.println("OTA Ready");

  // Web server routes
  server.on("/", handleRoot);
  server.on("/s", handleStatus);
  server.on("/gcfg", handleGetConfig);
  server.on("/c", handleConnect);
  server.on("/d", handleDisconnect);
  server.on("/p", HTTP_POST, handlePrint);
  server.on("/f", handleFeed);
  server.on("/tcfg", HTTP_POST, handleTwitchConfig);
  server.begin();
  Serial.println("Web server started");

  // Auto-connect printer on startup
  Serial.println("Auto-connecting to printer...");
  connectPrinter();

  // Auto-connect Twitch on startup
  Serial.println("Auto-connecting to Twitch...");
  connectTwitch();

  Serial.println("Setup complete!");
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  handleTwitchIRC();
  checkPrinterConnection();
  delay(10);
}
