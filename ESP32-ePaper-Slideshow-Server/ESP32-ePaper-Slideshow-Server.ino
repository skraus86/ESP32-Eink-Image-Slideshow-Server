#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_750_T7.h>  // 7.5" b/w display

// Waveshare ESP32 e-Paper Driver Board pins
#define EPD_BUSY 25
#define EPD_RST  26
#define EPD_DC   27
#define EPD_CS   15
#define EPD_SCK  13
#define EPD_MOSI 14

// Display object
GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT> display(
  GxEPD2_750_T7(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

AsyncWebServer server(80);

const char* ssid = "ESP32_EINK";
const char* password = "eink1234";

unsigned long lastUpdate = 0;
const unsigned long slideDelay = 10000; // 10 sec per slide
int currentImageIndex = 0;

std::vector<String> imageList;

// List all .bin images in /images directory
void listImages() {
  imageList.clear();
  if (!LittleFS.exists("/images")) LittleFS.mkdir("/images");

  File root = LittleFS.open("/images");
  if (!root || !root.isDirectory()) return;
  File file = root.openNextFile();
  while (file) {
    String name = file.name();
    if (name.endsWith(".bin")) {
      // remove "/images/" prefix before storing
      imageList.push_back(name.substring(8));
    }
    file = root.openNextFile();
  }
}

// Display 1-bit image on e-paper
void displayImage(const String& filename) {
  String path = "/images/" + filename;
  File file = LittleFS.open(path, "r");
  if (!file) {
    Serial.println("Failed to open " + path);
    return;
  }

  display.setFullWindow();
  display.firstPage();
  do {
    uint8_t line[800 / 8];
    for (int y = 0; y < 480; y++) {
      size_t r = file.read(line, sizeof(line));
      if (r != sizeof(line)) {
        Serial.println("File too short");
        break;
      }
      for (int x = 0; x < 800; x++) {
        int byteIndex = x / 8;
        int bitIndex = 7 - (x % 8);
        bool color = (line[byteIndex] >> bitIndex) & 0x01;
        display.drawPixel(x, y, color ? GxEPD_BLACK : GxEPD_WHITE);
      }
    }
  } while (display.nextPage());

  file.close();
  Serial.println("Displayed: " + path);
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nStarting...");

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);

  if (!LittleFS.begin(true)) {
    Serial.println("Failed to mount LittleFS");
    return;
  }

  display.init();
  display.setRotation(1);

  // Create /images folder if missing
  if (!LittleFS.exists("/images")) {
    LittleFS.mkdir("/images");
    Serial.println("Created /images folder");
  }

  WiFi.softAP(ssid, password);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  listImages();

  // Serve index.html
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
  });

  // Upload handler
  server.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      request->send(200, "text/plain", "Upload complete");
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (!index) {
        if (!LittleFS.exists("/images")) LittleFS.mkdir("/images");

        // Fix bad filenames
        if (filename.length() == 0 || filename == "n" || filename == "file" || filename == "upload") {
          filename = "upload_" + String(millis()) + ".bin";
        }

        filename.replace("/", "");
        if (!filename.endsWith(".bin")) filename += ".bin";

        Serial.println("Receiving upload: " + filename);
        request->_tempFile = LittleFS.open("/images/" + filename, "w");
      }

      if (len) {
        request->_tempFile.write(data, len);
      }

      if (final) {
        request->_tempFile.close();
        Serial.println("Uploaded: " + filename);
        listImages();
      }
    });

  // List files
  server.on("/list", HTTP_GET, [](AsyncWebServerRequest *request){
    DynamicJsonDocument doc(1024);
    JsonArray arr = doc.createNestedArray("files");
    for (auto &f : imageList) {
      arr.add(f);
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  // Serve image for preview (raw .bin)
  server.on("/image", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->hasParam("name")) {
      request->send(400, "text/plain", "Missing name");
      return;
    }
    String name = request->getParam("name")->value();
    String path = "/images/" + name;
    if (!LittleFS.exists(path)) {
      request->send(404, "text/plain", "Not found");
      return;
    }
    request->send(LittleFS, path, "application/octet-stream");
  });

  // Delete image file
  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->hasParam("name")) {
      request->send(400, "text/plain", "Missing name");
      return;
    }
    String name = request->getParam("name")->value();
    String path = "/images/" + name;
    if (LittleFS.exists(path)) {
      LittleFS.remove(path);
      Serial.println("Deleted: " + path);
      listImages();
    }
    request->send(200, "text/plain", "Deleted");
  });

  server.begin();
  Serial.println("Server started");
}

void loop() {
  if (imageList.empty()) return;

  unsigned long now = millis();
  if (now - lastUpdate > slideDelay) {
    displayImage(imageList[currentImageIndex]);
    currentImageIndex = (currentImageIndex + 1) % imageList.size();
    lastUpdate = now;
  }
}
