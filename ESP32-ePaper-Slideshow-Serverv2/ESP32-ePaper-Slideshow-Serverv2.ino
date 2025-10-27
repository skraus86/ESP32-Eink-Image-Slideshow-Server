#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_750_T7.h>

// ===== EINK PINS =====
#define EPD_BUSY 25
#define EPD_RST  26
#define EPD_DC   27
#define EPD_CS   15
#define EPD_SCK  13
#define EPD_MOSI 14

// ===== SD PINS =====
#define SD_CS    5
#define SD_MOSI  23
#define SD_MISO  19
#define SD_SCK   18

// ===== DISPLAY OBJECT =====
GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT> display(GxEPD2_750_T7(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// ===== SD SPI BUS =====
SPIClass sdSPI(VSPI);

AsyncWebServer server(80);
const char* ssid = "ESP32-eInk";
const char* password = "12345678";

bool slideshowRunning = false;
unsigned long lastSlideTime = 0;
int currentImage = 0;
const unsigned long slideInterval = 10000; // 10 seconds

// ===== HELPER: Read 16/32-bit from File =====
uint16_t read16(File &f) {
  uint16_t result;
  ((uint8_t*)&result)[0] = f.read();
  ((uint8_t*)&result)[1] = f.read();
  return result;
}
uint32_t read32(File &f) {
  uint32_t result;
  ((uint8_t*)&result)[0] = f.read();
  ((uint8_t*)&result)[1] = f.read();
  ((uint8_t*)&result)[2] = f.read();
  ((uint8_t*)&result)[3] = f.read();
  return result;
}

// ===== BMP DRAW FUNCTION =====
void displayBMP(const char *filename) {
  File file = SD.open(filename);
  if (!file) {
    Serial.println("Image open failed");
    return;
  }

  if (read16(file) != 0x4D42) {
    Serial.println("Not a BMP file");
    file.close();
    return;
  }

  (void)read32(file); // file size
  (void)read32(file); // creator bytes
  uint32_t imageOffset = read32(file);
  (void)read32(file); // DIB header size
  int32_t width = read32(file);
  int32_t height = read32(file);
  (void)read16(file);
  uint16_t depth = read16(file);
  (void)read32(file);

  if (depth != 24 && depth != 1) {
    Serial.printf("Unsupported BMP depth: %u\n", depth);
    file.close();
    return;
  }

  Serial.printf("Drawing %s (%dx%d, %u-bit)\n", filename, width, height, depth);

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    file.seek(imageOffset);

    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        uint8_t gray = 255;

        if (depth == 24) {
          uint8_t b = file.read();
          uint8_t g = file.read();
          uint8_t r = file.read();
          gray = (r * 30 + g * 59 + b * 11) / 100;
        } else if (depth == 1) {
          uint8_t byteVal = file.read();
          for (int bit = 7; bit >= 0 && x < width; bit--, x++) {
            bool pixelOn = byteVal & (1 << bit);
            display.drawPixel(x, height - 1 - y, pixelOn ? GxEPD_BLACK : GxEPD_WHITE);
          }
          continue;
        }

        display.drawPixel(x, height - 1 - y, gray < 128 ? GxEPD_BLACK : GxEPD_WHITE);
      }

      // Row padding
      uint32_t rowSize = ((depth * width + 31) / 32) * 4;
      uint32_t skip = rowSize - ((depth * width + 7) / 8);
      while (skip--) file.read();
    }

  } while (display.nextPage());

  file.close();
  Serial.println("Display complete.");
}

// ===== FILE LIST HANDLER =====
void handleList(AsyncWebServerRequest *request) {
  String json = "[";
  File root = SD.open("/");
  bool first = true;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) {
      if (!first) json += ",";
      json += "\"" + String(entry.name()) + "\"";
      first = false;
    }
    entry.close();
  }
  json += "]";
  request->send(200, "application/json", json);
}

// ===== FILE DELETE HANDLER =====
void handleDelete(AsyncWebServerRequest *request) {
  if (!request->hasParam("name")) {
    request->send(400, "text/plain", "Missing file name");
    return;
  }
  String filename = "/" + request->getParam("name")->value();
  if (SD.exists(filename)) {
    SD.remove(filename);
    request->send(200, "text/plain", "Deleted");
  } else {
    request->send(404, "text/plain", "Not found");
  }
}

// ===== UPLOAD HANDLER =====
void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (!index) {
    Serial.printf("UploadStart: %s\n", filename.c_str());
    File file = SD.open("/" + filename, FILE_WRITE);
    if (!file) return;
    file.write(data, len);
    file.close();
  } else {
    File file = SD.open("/" + filename, FILE_APPEND);
    file.write(data, len);
    file.close();
  }
  if (final) {
    Serial.printf("UploadEnd: %s\n", filename.c_str());
  }
}

// ===== SLIDESHOW LOGIC =====
void runSlideshow() {
  File root = SD.open("/");
  int fileCount = 0;
  String files[50];
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) {
      String name = String(entry.name());
      if (name == "/index.html") continue;  // <-- ignore index.html
      files[fileCount++] = "/" + name;
    }
    entry.close();
  }
  root.close();

  if (fileCount == 0) return;

  if (millis() - lastSlideTime > slideInterval) {
    displayBMP(files[currentImage].c_str());
    currentImage = (currentImage + 1) % fileCount;
    lastSlideTime = millis();
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);

  // Initialize eInk display SPI
  SPI.begin(EPD_SCK, -1, EPD_MOSI);
  display.init(115200);
  display.setRotation(0);

  // Initialize SD card on dedicated SPI bus
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI)) {
    Serial.println("SD mount failed!");
  } else {
    Serial.println("SD card OK");
  }

  // Setup WiFi AP
  WiFi.softAP(ssid, password);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  // Serve web UI
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SD, "/index.html", "text/html");
  });

  // File API
  server.on("/list", HTTP_GET, handleList);
  server.on("/delete", HTTP_GET, handleDelete);
  server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "OK");
  }, handleUpload);

  // Slideshow control endpoints
  server.on("/start", HTTP_GET, [](AsyncWebServerRequest *request) {
    slideshowRunning = true;
    request->send(200, "text/plain", "Slideshow started");
  });
  server.on("/stop", HTTP_GET, [](AsyncWebServerRequest *request) {
    slideshowRunning = false;
    request->send(200, "text/plain", "Slideshow stopped");
  });

  server.begin();
  Serial.println("Server started");
}

// ===== LOOP =====
void loop() {
  if (slideshowRunning) runSlideshow();
}
