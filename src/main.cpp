#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include "SPIFFS.h"
#include <Arduino_JSON.h>
#include <ESPmDNS.h>

//MAX30100_PulseOximeter Library
#include <MAX30100_PulseOximeter.h>

//BMP280 Libraries
#include <SPI.h>
#include <Adafruit_BMP280.h>

//OLED Display Libraries
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSerif9pt7b.h>

#define BMP280_ADDRESS 0x76
#define REPORTING_PERIOD_MS 1000

#define SCREEN_WIDTH 128  // OLED display width, in pixels
#define SCREEN_HEIGHT 64  // OLED display height, in pixels

const char* deviceName = "healthmonitor";

uint32_t tsLastReport = 0;

enum class SENSOR {
  HEART_RATE,
  SPO2,
  TEMPERATURE
};

// Define a struct
struct StSensorValue 
{
  int pulseRate;
  int spO2;
  float temperatureF;
};

StSensorValue g_stSensorValue;

QueueHandle_t structQueue;
int queueSize = 10;

//Libraries Object Instantiation
Adafruit_BMP280 bmp;  // I2C
PulseOximeter pox;
// PulseOximeter pox;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Create AsyncWebServer object on port 80
AsyncWebServer server(80);

// Create an Event Source on /events
AsyncEventSource events("/events");

// Initialise the Display
void initDisplay() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 allocation failed");
    for (;;)
      ;
  }
  delay(2000);

  display.setTextColor(WHITE);
  display.setFont(&FreeSerif9pt7b);

  // Display the welcom message
  display.clearDisplay();
  display.setCursor(0, 15);  // Y Axis is Should be minimum 15
  display.print("  Welcome To\nHealth Monitoring System");
  display.display();
}

// Initialize WiFi
void initWiFi() {
  //connect to your local wi-fi network  
  WiFiManager wm;

  bool res = wm.autoConnect("HMSWifiManager","12345678"); // password protected ap
  if(!res) {
    Serial.println("Failed to connect");
  }
}

// Initialize SPIFFS
void initSPIFFS() {
  if (!SPIFFS.begin()) {
    Serial.println("An error has occurred while mounting SPIFFS");
  } else {
    Serial.println("SPIFFS mounted successfully");
  }
}

void onBeatDetected() {
  Serial.println("Beat Detected!");
}

// Initialise the PulseOxymeter
void initPulseOxymeter() {
  pinMode(19, OUTPUT);
  delay(100);
  Serial.println("Initializing pulse oximeter..");
  if (!pox.begin()) {
    Serial.println("FAILED");
    for (;;);
  } else {
    Serial.println("SUCCESS");
    pox.setOnBeatDetectedCallback(onBeatDetected);
  }
  pox.setIRLedCurrent(MAX30100_LED_CURR_7_6MA);
}

// Initialise the BMP Temprature Sensor
void initBMPTempratureSensor() {
  Serial.println("Initializing temperature sensor..");
  unsigned int status = bmp.begin(BMP280_ADDRESS);
  if (!status) {
    Serial.println(F("Could not find a valid BMP280 sensor, check wiring or "
                     "try a different address!"));
    Serial.print("SensorID was: 0x");
    Serial.println(bmp.sensorID(), 16);
    Serial.println("   ID of 0xFF probably means a bad address, a BMP 180 or BMP 085");
    Serial.println("   ID of 0x56-0x58 represents a BMP 280,");
    Serial.println("        ID of 0x60 represents a BME 280.");
    Serial.println("        ID of 0x61 represents a BME 680.");
    //while (1) delay(10);
  }
  /* Default settings from the datasheet. */
  bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,     /* Operating Mode. */
                  Adafruit_BMP280::SAMPLING_X2,     /* Temp. oversampling */
                  Adafruit_BMP280::SAMPLING_X16,    /* Pressure oversampling */
                  Adafruit_BMP280::FILTER_X16,      /* Filtering. */
                  Adafruit_BMP280::STANDBY_MS_500); /* Standby time. */
}


String getSensorReadings() {
  // Json Variable to Hold Sensor Readings
  JSONVar readings;
  readings["pulseRateValue"] = String(g_stSensorValue.pulseRate);
  readings["spo2Value"] = String(g_stSensorValue.spO2);
  readings["tempValue"] = String(g_stSensorValue.temperatureF);

  return JSON.stringify(readings);
}

void TaskShowDisplay(void *pvParameters)
{
  for( ;; )
  {
    StSensorValue stSensorValue;  
    // Read structure elements from queue and check if data received successfully 
    if (xQueueReceive(structQueue, &stSensorValue, portMAX_DELAY) == pdPASS)
    {
      printf("PulseRate: %i /Min\n", stSensorValue.pulseRate);
      printf("SpO2: %i %%\n\n", stSensorValue.spO2);
      printf("Temperature: %f *F\n", stSensorValue.temperatureF);
      printf("*********************************\n\n");

      display.clearDisplay();
      display.setCursor(0, 15);  // Y Axis is Should be minimum 15
      display.printf("Pulse: %i /Min\n", stSensorValue.pulseRate);
      display.setCursor(0, 35);
      display.printf("SpO2: %i %%\n", stSensorValue.spO2);
      display.setCursor(0, 55);
      display.printf("Temp: %.2f *F\n", stSensorValue.temperatureF);
      display.display();
    }
    vTaskDelay(1);
    // taskYIELD(); //terminate the task and inform schulder about it
  }
  // vTaskDelete( NULL );
}

void setup() {
  // Initialise the Serial
  Serial.begin(115200);

  // Initialise the Display
  initDisplay();

  // Initialize WiFi
  initWiFi();

  if (!MDNS.begin(deviceName)) {
    Serial.print("Error starting mDNS");
  }

  // Initialize SPIFFS
  initSPIFFS();

  // Initialise the PulseOxymeter
  initPulseOxymeter();

  // Initialise the BMP Temprature Sensor
  initBMPTempratureSensor();

  // Web Server Root URL
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/index.html", "text/html");
  });

  server.serveStatic("/", SPIFFS, "/");

  // Request for the latest sensor readings
  server.on("/readData", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "application/json", getSensorReadings());
  });

  // Start server
  server.begin();

  // create Structure Queue
  structQueue = xQueueCreate(queueSize, sizeof(StSensorValue));
  if(structQueue == NULL)
  {
    Serial.println("Error creating the queue");
  }            
  xTaskCreate(TaskShowDisplay,    /* Task function. */
              "ShowDisplay",      /* String with name of task. */
              10000,                    /* Stack size in words. */
              NULL,                     /* Parameter passed as input of the task */
              1,                        /* Priority of the task. */
              NULL                      /* Task handle. */
            );            
}

void loop() {
  pox.update();
  g_stSensorValue.pulseRate = static_cast<int>(pox.getHeartRate());
  g_stSensorValue.spO2 = static_cast<int>(pox.getSpO2());
  g_stSensorValue.temperatureF = (bmp.readTemperature() * 9) / 5 + 32;
  
  if (millis() - tsLastReport > REPORTING_PERIOD_MS) 
  {    
    xQueueSend(structQueue, &g_stSensorValue, portMAX_DELAY); //write struct message to queue
    tsLastReport = millis();
  }
  
}
  