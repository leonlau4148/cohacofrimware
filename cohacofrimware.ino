#include <SPI.h>
#include <Wire.h>
#include <Bonezegei_DS3231.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <FirebaseESP32.h>
#include <SD.h>
#include <WebServer.h>
#include <map>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebSocketsServer.h>
WebSocketsServer webSocketsServer(8080); // WebSocket server on port 81
// Web server on port 80
WebServer server(80);

// Pin definitions
const int relayPin = 18;          // Relay connected to GPIO 18
const int buttonPin = 19;         // Button connected to GPIO 19
const int firstirsensorPin = 4;   // First IR sensor connected to GPIO 4
const int secondirsensorPin = 7;  // Second IR sensor connected to GPIO 7
const int thirdirsensorPin = 23;   // Third IR sensor connected to GPIO 8
const int CS_PIN = 10;            // Chip Select for SD card on GPIO 10 (SDIO_DATA_2)
const int SCK_PIN = 12;           // Serial Clock for SD card on GPIO 12 (SPI CLK)
const int MOSI_PIN = 11;          // Master Out Slave In for SD card on GPIO 11 (SPI MOSI)
const int MISO_PIN = 13;          // Master In Slave Out for SD card on GPIO 13 (SPI MISO)
const int SDA_PIN = 21;           // I2C data (SDA) on GPIO 21
const int SCL_PIN = 20;           // I2C clock (SCL) on GPIO 20

// RTC and LCD setup
Bonezegei_DS3231 rtc(0x68);          // I2C address for DS3231
LiquidCrystal_I2C lcd(0x27, 20, 4);  // I2C LCD (20x4)

// Wi-Fi credentials
const char* ssid = "testpc";        // Replace with your SSID
const char* password = "raven133";  // Replace with your Wi-Fi password

String machineName = "Cohaco";  // Default machine name
String deviceID;
String fullMachineName;

// Credentials for the web interface
const char* machineUser;     // Set your username here
const char* machinePassword = "admin123";  // Set your password here

// Firebase setup
FirebaseData firebaseData;
FirebaseAuth auth;
FirebaseConfig config;
Preferences preferences;

#define API_KEY "firebase api key"
#define DATABASE_URL "firebase url"
// Obstacle detection variables for the three IR sensors
int firstSensorCount = 0;
int secondSensorCount = 0;
int thirdSensorCount = 0;

// New variables to load and store the latest date's sensor values
int latestFirstSensorCount = 0;
int latestSecondSensorCount = 0;
int latestThirdSensorCount = 0;
String latestDate = ""; // To store the latest date loaded from the SD card
//streamsensorvalues
int streamsens1 = 0;
int streamsens2 = 0;
int streamsens3 = 0;

bool firstSensorDetected = false;
bool secondSensorDetected = false;
bool thirdSensorDetected = false;

bool sdCardInitialized = false;
String lastLoggedDate = "";  // Tracks the last date that was logged
// Variables
int buttonState = HIGH;       // Variable to store the current button state (default to HIGH for pull-up)
int lastButtonState = HIGH;   // Variable to store the previous button state
bool firstRun = true;     
bool relayState = LOW;  // Store the current state of the relay
bool firebaseConnected = false; 
//int baseYear = 2000;  // Default base year
int day, month, year, baseYear, hour, minute, ampm, timeFormat;
bool isAPMode = true;  // Global flag to track AP or STA mode
bool apModeMessagePrinted = false; 


// Wi-Fi credentials
char ssidValue[32] = "";  // Maximum length of Wi-Fi SSID
char passwordValue[64] = "";  // Maximum length of Wi-Fi password

String tempSSID = "";
String tempPassword = "";

bool  loadSDExecuted = false; 

unsigned long previousMillis = 0;
const long interval = 2000; // Interval in milliseconds for Firebase checks

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    ;  // Wait for serial port to connect
  }
  delay(10000); //startup delay

  // Open preferences in read-write mode
  preferences.begin("wifi-creds", false);
  
  initializeHardware();
// Retry initializing the SD card until successful
  while (!initializeSDCard()) {
    Serial.println("Retrying SD card initialization...");
    //delay(5000); // Wait before retrying
  }

  // Proceed to check and create the log file after successful initialization
  File logFile = SD.open("/logs/obstacle_log.txt", FILE_READ);
  if (!logFile) {
    // File does not exist; create a new one
    logFile = SD.open("/logs/obstacle_log.txt", FILE_WRITE);
    if (logFile) {
      String currentDate = getCurrentDate();
      logFile.println(currentDate + " firstsensor=0 secondsensor=0 thirdsensor=0"); // Write the initial data
      logFile.close(); // Always close the file after writing
      Serial.println("New log file created with initial data.");
    } else {
      Serial.println("Failed to create log file.");
    }
  } else {
    Serial.println("Log file already exists.");
    logFile.close(); // Close the file after reading
  }

  setconstants ();
   // Load Wi-Fi credentials from EEPROM
  loadWiFiCredentials();
   // If credentials exist, attempt to connect to Wi-Fi
  if (strlen(ssidValue) > 0 && strlen(passwordValue) > 0) {
    isAPMode = false;
    connectToWiFi();
  }
  else{
   setupWiFi();
  }
  setupWebServer();
  webSocketsServer.begin();
  webSocketsServer.onEvent(webSocketEvent);
  Serial.println("Setup completed.");

}
void loop() {

   
  // Get the current time from the RTC
  rtc.getTime();

  // Get the current date as a string
  String currentDate = getCurrentDate();
  // Run `loadObstacleDataFromSD()` only once
  if (!loadSDExecuted) {
    loadObstacleDataFromSD();
    loadSDExecuted = true;  // Mark as executed
    Serial.println("Obstacle data loaded from SD card.");
  }
  // Check if the date has changed
  if (currentDate != lastLoggedDate) {
    // Append the new day's log entry with reset counts
    saveConsolidatedObstacleDataToSD(currentDate);

    // Reset the sensor counts for the new day
    firstSensorCount = 0;
    secondSensorCount = 0;
    thirdSensorCount = 0;

    // Update the last logged date to the current date
    lastLoggedDate = currentDate;
  }

  // Handle client requests
  server.handleClient();
  webSocketsServer.loop();
  if (!isAPMode && !firebaseConnected) {
    setupFirebase(); // Firebase enabled in STA mode
      
    apModeMessagePrinted = false;  // Reset flag so it can print again if we switch back to AP mode
  }else if (isAPMode) {
    if (!apModeMessagePrinted) {
      Serial.println("In AP mode. Firebase disabled.");
      firebaseConnected = false;  // Ensure Firebase is marked as disconnected
      apModeMessagePrinted = true;
    }
  }

   unsigned long currentMillis = millis();

  // Perform Firebase operations periodically
  if (Firebase.ready() && (currentMillis - previousMillis >= interval)) {
    previousMillis = currentMillis;

    // Call Firebase functions with spacing
    firebaseRelayControl();
    delay(100);  // Small delay between operations
    firebaseCheckTriggerSave();
    delay(100);
    firebaseCheckTrigger();
    delay(100);
    firebaseCheckTriggerAP();
  }
  
  // Display the current time on the LCD
  displayCurrentTimeOnLCD();

  // Handle button and relay control
  handleButtonRelay();

   // Ensure that the relay pin stays in its last state
  digitalWrite(relayPin, relayState);  // Maintain the state based on the global variable

  // Check and handle obstacle detection for all sensors
  handleObstacleDetection(firstirsensorPin, firstSensorDetected, firstSensorCount, "firstsensor");
  handleObstacleDetection(secondirsensorPin, secondSensorDetected, secondSensorCount, "secondsensor");
  handleObstacleDetection(thirdirsensorPin, thirdSensorDetected, thirdSensorCount, "thirdsensor");
  
  // Avoid bouncing
  delay(100);
  sendSensorData();
   
}

void firebaseCheckTriggerSave() {
  if (Firebase.getBool(firebaseData, "devices/" + String(fullMachineName) + "/triggerSave")) {
    if (firebaseData.boolData() == true) {
      
      // Reset the trigger in Firebase
      if (Firebase.setBool(firebaseData, "devices/" + String(fullMachineName) + "/triggerSave", false)) {
        Serial.println("TriggerSave reset successfully in Firebase");
      } else {
        Serial.println("Failed to reset TriggerSave in Firebase");
        Serial.println("REASON: " + firebaseData.errorReason());
      }
      // Perform the action for triggerSave
      checkForWiFiUpdate();
    }
  } else {
    Serial.println("Failed to get TriggerSave data from Firebase");
    Serial.println("REASON: " + firebaseData.errorReason());
    ESP.restart();
  }
}

void firebaseCheckTriggerAP() {
  if (Firebase.getBool(firebaseData, "devices/" + String(fullMachineName) + "/triggerAP")) {
    if (firebaseData.boolData() == true) {
      // Reset the trigger in Firebase
      if (Firebase.setBool(firebaseData, "devices/" + String(fullMachineName) + "/triggerAP", false)) {
        Serial.println("TriggerAP reset successfully in Firebase");
      } else {
        Serial.println("Failed to reset TriggerAP in Firebase");
        Serial.println("REASON: " + firebaseData.errorReason());
      }
      // Perform the action for triggerAP
      WiFi.disconnect();
      preferences.clear();
      delay(1000);
      setupWiFi(); // Call your setup WiFi function
      
      
    }
  } else {
    Serial.println("Failed to get TriggerAP data from Firebase");
    Serial.println("REASON: " + firebaseData.errorReason());
    ESP.restart();
  }
}


void firebaseCheckTrigger() {
  if (Firebase.getBool(firebaseData, "devices/" + String(fullMachineName) + "/triggerScan")) {
    if (firebaseData.boolData() == true) {
      firebaseWifiScan(); // Perform the scan
      if (Firebase.setBool(firebaseData, "devices/" + String(fullMachineName) + "/triggerScan", false)) {
        Serial.println("Trigger reset successfully in Firebase");
      } else {
        Serial.println("Failed to reset trigger in Firebase");
        Serial.println("REASON: " + firebaseData.errorReason());
      }
    }
  } else {
    Serial.println("Failed to get trigger data from Firebase");
    Serial.println("REASON: " + firebaseData.errorReason());
  }
}


void sendSensorData() {

  int firstvalsens = firstSensorCount;
  int secondvalsens = secondSensorCount;
  int thirdvalsens = thirdSensorCount;

    String response = String(firstvalsens) + "," +
                      String(secondvalsens) + "," +
                      String(thirdvalsens);
    
    webSocketsServer.broadcastTXT(response); // Send data to all connected clients
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
    if (type == WStype_TEXT) {
        String message = String((char*)payload);
        Serial.println("Received: " + message);
        // Handle incoming messages if necessary
    }
}

String generateUUID(uint64_t macAddress) {
  // Use the MAC address as part of the UUID
  String uuid = String((uint32_t)(macAddress >> 32), HEX);
  uuid.toUpperCase();
  return uuid;
}

void initializeHardware() {
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);       // Initial relay state
  pinMode(buttonPin,  INPUT_PULLUP);  // 
  pinMode(firstirsensorPin, INPUT);
  pinMode(secondirsensorPin, INPUT);
  pinMode(thirdirsensorPin, INPUT);

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);

  // Initialize I2C for RTC and LCD
  Wire.begin(SDA_PIN, SCL_PIN);

  // Initialize RTC
  rtc.begin();
  //rtc.setFormat(12);        // 12-hour format
  //rtc.setAMPM(1);           // Set to PM (1)
  //rtc.setTime("08:27:00");    // Set time to 06:34:00
  //rtc.setDate("2/16/25");     // Set date to 09/07/2024

  lcd.init();
  lcd.backlight();

  Serial.println("Hardware initialized.");
}

bool initializeSDCard() {
  delay(5000); // Optional delay for setup purposes
  Serial.print("Initializing SD card...");

  if (!SD.begin(CS_PIN)) {
    Serial.println("SD card initialization failed!");
    return false;
  }
  
  Serial.println("SD card initialization done.");

  uint32_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.print("Card Size: ");
  Serial.print(cardSize);
  Serial.println(" MB");

  if (!SD.exists("/logs")) {
    if (SD.mkdir("/logs")) {
      Serial.println("Directory '/logs' created.");
    } else {
      Serial.println("Failed to create directory '/logs'.");
    }
  }

  sdCardInitialized = true;
  return true;
}


void setconstants (){
   // Generate UUID and create full machine name
  deviceID = generateUUID(ESP.getEfuseMac());
  fullMachineName = machineName + "-" + deviceID;
  
  // Initialize global username with unique machine name
  machineUser = fullMachineName.c_str();

  
}

void setupWiFi() {
  
  // Define SSID and Password for the Access Point
  const char* apSSID = machineUser;
  const char* apPassword = "12345678";

  Serial.print("Configuring Access Point with SSID: ");
  Serial.println(apSSID);

  // Initialize the Access Point
  WiFi.softAP(apSSID, apPassword);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);

  if (!MDNS.begin(machineUser)) {
    Serial.println("Error setting up mDNS responder!");
  } else {
    Serial.println("mDNS responder started with name: " + fullMachineName);
  }
}
void initializeFirebasePaths() {
  String devicePath = "devices/"+ fullMachineName;
  // Initialize default paths and values if they don’t exist
  if (!Firebase.getString(firebaseData, devicePath + "/relayControl")) {
    Firebase.setString(firebaseData, devicePath + "/relayControl", "off");
  }
  if (!Firebase.getString(firebaseData, devicePath + "/status")){
      Firebase.setString(firebaseData, devicePath + "/status", "online");
      Firebase.setString(firebaseData, devicePath + "/devicePass", machinePassword);
      Firebase.setString(firebaseData, devicePath + "/name", fullMachineName);
  }
  if (!Firebase.getBool(firebaseData, devicePath + "/triggerScan")){
      Firebase.setBool(firebaseData, devicePath + "/triggerScan", false);
  }
  if (!Firebase.getString(firebaseData, devicePath + "/credentials")){
      saveCredentialsToFirebase();
  }
  if (!Firebase.getBool(firebaseData, devicePath + "/triggerAP")){
      Firebase.setBool(firebaseData, devicePath + "/triggerAP", false);
  }
   if (!Firebase.getBool(firebaseData, devicePath + "/triggerSave")){
      Firebase.setBool(firebaseData, devicePath + "/triggerSave", false);
  }

}

void setupFirebase() {

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.signer.tokens.legacy_token = "YeQ4PPW1dSicOiNUQjaNQMDASqJi9KGejR5AQOI4";

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);


  if (Firebase.ready()) {
      if (!firebaseConnected) { // Print only once
      // Initialize Firebase paths if not existing
      initializeFirebasePaths();
      // Start Firebase stream with callback
      saveLogsToFirebase();
     

      firebaseConnected = true; // Set the flag to true
      Serial.println("Firebase connected and stream started.");    
    }
  } else {
    Serial.println("Firebase connection failed.");
    Serial.println(firebaseData.errorReason());
  }
}




void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/download", handleFileDownload);
  server.on("/createfile", handleCreateFile);
  server.on("/emptyfile", handleEmptyFile);
  server.on("/relay", HTTP_POST, handleRelayControl); // For relay control
  server.on("/data", handleLiveData);  // For fetching live data
  server.on("/setDateTime", handleSetDateTime);
  server.on("/mountSD", HTTP_GET,handlemountSD);
  server.on("/getRelayState", handleGetRelayState);
  server.on("/switch-mode", handleModeSwitch);  // Route for switching mode
  server.on("/scan-networks", handleWiFiScan);  // Route to scan Wi-Fi
  server.begin();
  Serial.println("HTTP server started.");
}

void handleObstacleDetection(int sensorPin, bool& sensorDetected, int& sensorCount, String sensorName) {
  int sensorValue = digitalRead(sensorPin);

  if (sensorValue == LOW && !sensorDetected) {
    sensorDetected = true;
    sensorCount++;

    String currentDate = getCurrentDate();

    // Update the current date's log entry
    updateLogEntryForCurrentDate(currentDate, firstSensorCount, secondSensorCount, thirdSensorCount);
    if(Firebase.ready()){
      saveObstacleDataToFirebase(currentDate);
    }
    displayLatestObstacleLogOnLCD(currentDate, firstSensorCount, secondSensorCount, thirdSensorCount);
  } else if (sensorValue == HIGH && sensorDetected) {
    sensorDetected = false;
  }
}

void saveConsolidatedObstacleDataToSD(String currentDate) {
  if (!sdCardInitialized) return;

  // Open the log file for reading to check if the date already exists
  File logFile = SD.open("/logs/obstacle_log.txt", FILE_READ);
  if (!logFile) {
    Serial.println("Error opening log file for reading.");
    return;
  }

  String fileContent = "";
  bool dateFound = false;

  while (logFile.available()) {
    String line = logFile.readStringUntil('\n');
    
    // Check if the line contains the current date
    if (line.startsWith(currentDate)) {
      dateFound = true;
    }

    // Store the log file content
    fileContent += line + "\n";
  }

  logFile.close();

  // If the date is not found, append a new entry
  if (!dateFound) {
    File dataFile = SD.open("/logs/obstacle_log.txt", FILE_APPEND);
    if (dataFile) {
      dataFile.println(currentDate + " firstsensor=0 secondsensor=0 thirdsensor=0");
      dataFile.close();
      Serial.println("New log entry created for the current date.");
    } else {
      Serial.println("Error opening file '/logs/obstacle_log.txt' for appending.");
    }
  }
}

void updateLogEntryForCurrentDate(String currentDate, int firstCount, int secondCount, int thirdCount) {
  if (!sdCardInitialized) return;

  // Open the log file and read its contents
  File logFile = SD.open("/logs/obstacle_log.txt", FILE_READ);
  if (!logFile) {
    Serial.println("Error opening log file for reading. update "+ currentDate);
    return;
  }

  String fileContent = "";
  bool dateFound = false;

  while (logFile.available()) {
    String line = logFile.readStringUntil('\n');

    // If the line contains the current date, only update if the new counts are greater than existing
    if (line.startsWith(currentDate)) {
      int loggedFirstCount, loggedSecondCount, loggedThirdCount;
      sscanf(line.c_str(), "%*s firstsensor=%d secondsensor=%d thirdsensor=%d", &loggedFirstCount, &loggedSecondCount, &loggedThirdCount);

      // Update counts only if new values are greater
      line = currentDate + " firstsensor=" + String(max(loggedFirstCount, firstCount)) +
             " secondsensor=" + String(max(loggedSecondCount, secondCount)) +
             " thirdsensor=" + String(max(loggedThirdCount, thirdCount));
      dateFound = true;
    }

    fileContent += line + "\n";
  }

  logFile.close();

  // If the current date was not found, append a new line
  if (!dateFound) {
    fileContent += currentDate + " firstsensor=" + String(firstCount) +
                   " secondsensor=" + String(secondCount) +
                   " thirdsensor=" + String(thirdCount) + "\n";
  }
String rtcDate = getCurrentDate();
  // Write the updated content back to the log file
  File writeFile = SD.open("/logs/obstacle_log.txt", FILE_WRITE);
  if (writeFile) {
    writeFile.print(fileContent);
    writeFile.close();
    Serial.println("Log updated successfully. " +rtcDate);
  } else {
    Serial.println("Error writing to log file.");
  }
}

void loadObstacleDataFromSD() {
   String rtcDate = getCurrentDate();
  if (sdCardInitialized) {
    File dataFile = SD.open("/logs/obstacle_log.txt", FILE_READ);
    if (dataFile) {
      String lastLine;
      while (dataFile.available()) {
        lastLine = dataFile.readStringUntil('\n');
      }
      dataFile.close();

      if (lastLine.length() > 0) {
        // Parse the last line for sensor counts and last logged date
        char loggedDate[11];  // Adjust size if your date format changes
        sscanf(lastLine.c_str(), "%10s firstsensor=%d secondsensor=%d thirdsensor=%d", loggedDate, &firstSensorCount, &secondSensorCount, &thirdSensorCount);

        // Ensure you only load data from the current RTC date
       
        if (rtcDate == String(loggedDate)) {
          lastLoggedDate = rtcDate;
          Serial.println("Last line from log: " + lastLine);
          Serial.println("Loaded counts: " + String(firstSensorCount) + ", " + String(secondSensorCount) + ", " + String(thirdSensorCount));
          Serial.println("Last logged date: " + lastLoggedDate);
        } else {
          // New date - don't reset values, just keep them as they were
          Serial.println("No data for the new date: " + rtcDate);
        }
      }
    } else {
      Serial.println("Error opening file '/logs/obstacle_log.txt' for reading. load");
    }
  }
}


void handleButtonRelay() {
  // Read the current button state
  buttonState = digitalRead(buttonPin);

  // Ignore the first button read to avoid false triggers at startup
  if (firstRun) {
    lastButtonState = buttonState;
    firstRun = false;
    return;
  }

  // Check for a button press (button state changes from HIGH to LOW)
  if (buttonState == LOW && lastButtonState == HIGH) {  // Active LOW due to pull-up
    // Toggle the relay state
    relayState = !relayState;

    // Update the relay pin
    digitalWrite(relayPin, relayState);

    // Log the state
    Serial.println(relayState ? "Relay turned ON by button" : "Relay turned OFF by button");

    // Add a small delay to debounce the button
    delay(50);

    // Update Firebase with the new relay state
    if (Firebase.ready()) {
      if (Firebase.setString(firebaseData, "devices/" + String(fullMachineName) + "/relayControl", relayState ? "on" : "off")) {
        Serial.println("Firebase updated with new relay state");
      } else {
        Serial.println("Failed to update Firebase");
        Serial.println("REASON: " + firebaseData.errorReason());
      }
    }
  }

  // Save the current button state for the next loop
  lastButtonState = buttonState;

  // Optionally, send the current state to a web server (if applicable)
  String state = getRelayState();
  server.send(200, "text/plain", state);
}


void displayCurrentTimeOnLCD() {
  String currentDate = getCurrentDate();
  String currrentTime = formatTimeWithPeriod();
  lcd.setCursor(0, 0);
  lcd.print(currentDate + " "+ currrentTime);
}

void displayLatestObstacleLogOnLCD(String date, int firstCount, int secondCount, int thirdCount) {
  String currentDate = getCurrentDate();
  String currentTime = formatTimeWithPeriod();
  int sum = firstCount + secondCount + thirdCount;
  lcd.clear();  // Clear the display before showing new data

  // Display the date and time on the first row
  lcd.setCursor(0, 0);
  lcd.print(currentDate + " " + currentTime);

  lcd.setCursor(0, 1);
  lcd.print("Overall = "+ String(sum));

  // Display the header row
  lcd.setCursor(0, 2);
  lcd.print("Small Medium Large");

  // Display the counts on the next row
  char buffer[21]; // 20 characters + null terminator
  snprintf(buffer, sizeof(buffer), "%-6d%-7d%-7d", firstCount, secondCount, thirdCount);
  lcd.setCursor(0, 3);
  lcd.print(buffer);
}


void saveObstacleDataToFirebase(String date) {
  if (Firebase.ready()) {
    
    String path = "/devices/" + fullMachineName + "/obstacle_logs/" + date;

    Firebase.setInt(firebaseData, path + "/firstSensor", firstSensorCount);
    Firebase.setInt(firebaseData, path + "/secondSensor", secondSensorCount);
    Firebase.setInt(firebaseData, path + "/thirdSensor", thirdSensorCount);

    if (firebaseData.errorReason() == "") {
      Serial.println("Data saved to Firebase successfully.");
    } else {
      Serial.println("Failed to save data to Firebase: " + firebaseData.errorReason());
    }
  } else {
    Serial.println("Firebase is not ready.");
  }
}


String getCurrentDate() {
  // Default to a known invalid date if RTC is missing
 // if (!rtc.begin()) {
 //   return "Invalid Date";
 // }

  int day = rtc.getDate();
  int month = rtc.getMonth();
  int year = rtc.getYear();

  String date = twoDigit(month) + "-" + twoDigit(day) + "-" + "20" + twoDigit(year);

  return date;
}

String formatTimeWithPeriod() {
  int hour = rtc.getHour();
  String period = "AM";

  if (rtc.getFormat() == 12) {  // 12-hour format
    period = rtc.getAMPM() ? "PM" : "AM";
    if (hour == 0) {
      hour = 12;  // Midnight case
    } else if (hour > 12) {
      hour -= 12;  // Convert to 12-hour format
    }
  } else {  // 24-hour format
    if (hour >= 12) {
      period = "PM";
      if (hour > 12) {
        hour -= 12;  // Convert to 12-hour format
      }
    } else if (hour == 0) {
      hour = 12;  // Midnight case
    }
  }

  return twoDigit(hour) + ":" + twoDigit(rtc.getMinute()) +" " + period;
}

String twoDigit(int number) {
  if (number < 10) {
    return "0" + String(number);
  }
  return String(number);
}

// Authenticate user before accessing any page
bool isAuthenticated() {
  if (!server.authenticate(machineUser, machinePassword)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

void handleRoot() {
  if (!isAuthenticated()) return;  // Require login
  server.send(200, "text/html", "<html><body><h1>ESP32 Web Server</h1><p><a href=\"/download\">Download Log File</a></p></body></html>");
}

void handleFileDownload() {
  if (!isAuthenticated()) return;  // Require login
  File downloadFile = SD.open("/logs/obstacle_log.txt");
  if (downloadFile) {
    server.streamFile(downloadFile, "text/plain");
    downloadFile.close();
  } else {
    server.send(404, "text/plain", "File not found");
  }
}

void handleCreateFile() {
  if (!isAuthenticated()) return;  // Require login
  String filePath = "/logs/obstacle_log.txt";

  // Check if the file exists
  if (SD.exists(filePath)) {
    server.send(200, "text/plain", "File already exists.");
    Serial.println("File already exists.");
  } else {
    // Create the file
    File file = SD.open(filePath, FILE_WRITE);
    if (file) {
      file.close();  // Close immediately to create the file
      server.send(200, "text/plain", "File created successfully.");
      Serial.println("File created successfully.");
    } else {
      server.send(500, "text/plain", "Failed to create the file.");
      Serial.println("Failed to create the file.");
    }
  }
}

void handleEmptyFile() {
  if (!isAuthenticated()) return;  // Require login
  String filePath = "/logs/obstacle_log.txt";

  // Check if the file exists
  if (SD.exists(filePath)) {
    // Open the file in write mode to truncate (empty) the file
    File file = SD.open(filePath, FILE_WRITE);
    if (file) {
      file.close();  // Close immediately to empty the file
      server.send(200, "text/plain", "File emptied successfully.");
      Serial.println("File emptied successfully.");
    } else {
      server.send(500, "text/plain", "Failed to empty the file.");
      Serial.println("Failed to empty the file.");
    }
  } else {
    server.send(404, "text/plain", "File not found.");
    Serial.println("File not found.");
  }
}



String getRelayState() {
  if (digitalRead(relayPin) == HIGH) {
    return "on";
  } else {
    return "off";
  }
}

void handleGetRelayState() {
  String state = getRelayState();
  server.send(200, "text/plain", state);
}


void handleRelayControl() {
  if (!isAuthenticated()) return;  // Require login
  if (server.hasArg("state")) {
    String state = server.arg("state");
    if (state == "on") {
      relayState = HIGH;  // Update the global state
      digitalWrite(relayPin, HIGH);  // Turn relay ON
      server.send(200, "text/plain", "Relay turned ON.");
    } else if (state == "off") {
      relayState = LOW;  // Update the global state
      digitalWrite(relayPin, LOW);   // Turn relay OFF
      server.send(200, "text/plain", "Relay turned OFF.");
    } else {
      server.send(400, "text/plain", "Invalid relay state.");
    }
  } else {
    server.send(400, "text/plain", "Missing relay state.");
  }
}
void handlemountSD() {
  if (!isAuthenticated()) return;  // Require login
  if (!SD.begin(CS_PIN))  {
      server.send(500, "text/plain", "SD card mount failed.");
    } else {
      server.send(200, "text/plain", "SD card mounted successfully.");
    }
}


void handleLiveData() {
  if (!isAuthenticated()) return;  // Require login
  String currentDate = getCurrentDate();
  // Format the response as plain text
  String response = currentDate +  String(firstSensorCount) + String(secondSensorCount) + String(thirdSensorCount);

  // Send plain text response
  server.send(200, "text/plain", response);
}

// Function to update RTC based on data received from Flutter, with AM/PM and format support
void updateRTC(int newDay, int newMonth, int newYear, int newBaseYear, int hour, int minute, int ampm, int timeFormat) {
  baseYear = newBaseYear;  // Update base year

  // Combine baseYear and 2-digit year to get full year
  int fullYear = baseYear + newYear;

  // If 12-hour format is selected, convert to 24-hour format if needed
  if (timeFormat == 12) {
    if (ampm == 1 && hour != 12) {  // PM and not 12 PM
      hour += 12;
    } else if (ampm == 0 && hour == 12) {  // 12 AM
      hour = 0;
    }
  }
  
  // Format the date and time as strings
  String formattedDate = String(newMonth) + "/" + String(newDay) + "/" + String(fullYear % 100);  // Format as MM/DD/YY
  String formattedTime = String(hour) + ":" + String(minute) + ":00";  // Format as HH:MM:SS

  // Convert Strings to const char* (C-style strings)
  rtc.setTime(formattedTime.c_str());  // Set the formatted time
  rtc.setDate(formattedDate.c_str());  // Set the formatted date

  // Set AM/PM if in 12-hour mode
  if (timeFormat == 12) {
    rtc.setAMPM(ampm);  // Set AM/PM: 0 = AM, 1 = PM
  }
  
  // Set the time format (12-hour or 24-hour)
  rtc.setFormat(timeFormat);  // Set format to 12-hour or 24-hour
} 

void handleSetDateTime() {
  if (server.hasArg("day") && server.hasArg("month") && server.hasArg("year") && server.hasArg("baseYear") &&
      server.hasArg("hour") && server.hasArg("minute") && server.hasArg("ampm") && server.hasArg("timeFormat")) {

    // Save current date's obstacle data before updating the RTC date
    String currentDate = getCurrentDate();
    saveConsolidatedObstacleDataToSD(currentDate);
    
    day = server.arg("day").toInt();
    month = server.arg("month").toInt();
    year = server.arg("year").toInt();  // Only 2 digits (e.g., 24 for 2024)
    baseYear = server.arg("baseYear").toInt();
    hour = server.arg("hour").toInt();
    minute = server.arg("minute").toInt();
    int ampm = server.arg("ampm").toInt();  // AM/PM toggle (0 for AM, 1 for PM)
    int timeFormat = server.arg("timeFormat").toInt();  // 12-hour (12) or 24-hour (24) format

    // Update the RTC with the received date, time, and format
    updateRTC(day, month, year, baseYear, hour, minute, ampm, timeFormat);

    server.send(200, "text/plain", "RTC updated successfully");
  } else {
    server.send(400, "text/plain", "Invalid parameters");
  }
}


void handleModeSwitch() {
  if (server.hasArg("mode")) {
    String mode = server.arg("mode");

    if (mode == "online") {
      String wifiSSID = server.arg("ssid");
      String wifiPassword = server.arg("password");

      tempSSID = wifiSSID;
      tempPassword = wifiPassword;

       // Copy to global variables
      strncpy(ssidValue, wifiSSID.c_str(), sizeof(ssidValue) - 1);
      ssidValue[sizeof(ssidValue) - 1] = '\0';

      strncpy(passwordValue, wifiPassword.c_str(), sizeof(passwordValue) - 1);
      passwordValue[sizeof(passwordValue) - 1] = '\0';

        // Save to Preferences
      saveWiFiCredentials(ssidValue, passwordValue);
      WiFi.softAPdisconnect(true);  // Turn off AP mode
      Serial.println("Connecting to Wi-Fi...");

      // Begin connection to the specified Wi-Fi
      WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

      // Retry logic
      int maxRetries = 5;
      int retryCount = 0;
      while (WiFi.status() != WL_CONNECTED && retryCount < maxRetries) {
        delay(2000); // Wait 2 seconds before retrying
        Serial.println("Retrying connection to Wi-Fi...");
        retryCount++;
      }

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Connected to Wi-Fi.");
        WiFi.softAPdisconnect();
        isAPMode = false;
        server.send(200, "text/plain", "Switched to Wi-Fi mode.");
      } else {
        Serial.println("Failed to connect to Wi-Fi.");
        // Send error message with a reason for failure
        String errorMsg = "Failed to connect to Wi-Fi after " + String(maxRetries) + " attempts.";
        server.send(500, "text/plain", errorMsg);
        // Re-enable AP mode
       preferences.clear();
       setupWiFi();
       isAPMode = true;
      }
    } 
    else if (mode == "offline") {
      // Disconnect from any Wi-Fi connection
      WiFi.disconnect(true);
      preferences.clear();
      // Re-enable AP mode
      setupWiFi();  
      isAPMode = true;
      Serial.println("Switched to AP mode.");
      server.send(200, "text/plain", "Switched to AP mode.");
    }
  } 
  else {
    // Invalid request, no mode argument present
    server.send(400, "text/plain", "Invalid request.");
  }
}


// Function to scan Wi-Fi networks
void handleWiFiScan() {
  int numNetworks = WiFi.scanNetworks();
  String result = "[";  // Start JSON array

  for (int i = 0; i < numNetworks; ++i) {
    if (i > 0) {
      result += ",";
    }
    result += "{";
    result += "\"ssid\":\"" + WiFi.SSID(i) + "\",";
    result += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
    result += "\"encryptionType\":" + String(WiFi.encryptionType(i));
    result += "}";
  }

  result += "]";
  
  server.send(200, "application/json", result);
  WiFi.scanDelete();  // Clear the list of scanned networks
}

 

void firebaseRelayControl(){
  if (Firebase.getString(firebaseData, "devices/" + String(fullMachineName) + "/relayControl")) {
    String firebaseState = firebaseData.stringData();

    if (firebaseState == "on" && relayState == LOW) {
      relayState = HIGH;
      digitalWrite(relayPin, HIGH);  // Turn relay ON
      Serial.println("Relay turned ON via Firebase");
    } else if (firebaseState == "off" && relayState == HIGH) {
      relayState = LOW;
      digitalWrite(relayPin, LOW);   // Turn relay OFF
      Serial.println("Relay turned OFF via Firebase");
    }
  } else {
    Serial.println("Failed to get data from Firebase");
    Serial.println("REASON: " + firebaseData.errorReason());
  }
}

void saveLogsToFirebase() {
  if (!sdCardInitialized) {
    Serial.println("SD card not initialized.");
    return;
  }

  File logFile = SD.open("/logs/obstacle_log.txt", FILE_READ);
  if (!logFile) {
    Serial.println("Failed to open log file.");
    return;
  }

  // Read each line in the file
  while (logFile.available()) {
    String line = logFile.readStringUntil('\n');
    if (line.length() > 0) {
      char date[11];
      int firstSensor, secondSensor, thirdSensor;

      // Parse the line for date and sensor counts
      sscanf(line.c_str(), "%10s firstsensor=%d secondsensor=%d thirdsensor=%d",
             date, &firstSensor, &secondSensor, &thirdSensor);

      // Construct Firebase path based on date
      String firebasePath = "devices/" + fullMachineName + "/obstacle_logs/" + String(date);

      // Check if data for this date already exists in Firebase
      if (Firebase.ready()) {
        // Attempt to retrieve the existing first sensor value
        bool success = Firebase.getInt(firebaseData, firebasePath + "/firstSensor");

        // If the retrieval was unsuccessful or the data has changed, update Firebase
        if (!success || firebaseData.intData() != firstSensor) {
          Firebase.setInt(firebaseData, firebasePath + "/firstSensor", firstSensor);
          Firebase.setInt(firebaseData, firebasePath + "/secondSensor", secondSensor);
          Firebase.setInt(firebaseData, firebasePath + "/thirdSensor", thirdSensor);

          if (firebaseData.errorReason() == "") {
            Serial.println("Data for " + String(date) + " updated in Firebase successfully.");
          } else {
            Serial.println("Failed to update data for " + String(date) + " to Firebase: " + firebaseData.errorReason());
          }
        } else {
          Serial.println("No change in data for " + String(date) + " or data is already up-to-date.");
        }
      } else {
        Serial.println("Firebase not ready.");
      }
    }
  }

  logFile.close();
  Serial.println("Finished checking and updating log entries in Firebase.");
}

// Save Wi-Fi credentials to Preferences
void saveWiFiCredentials(const char *newSSID, const char *newPassword) {
  preferences.putString("ssid", newSSID);
  preferences.putString("password", newPassword);
  Serial.println("Wi-Fi credentials saved.");
}

void loadWiFiCredentials() {
  String ssidStr = preferences.getString("ssid", "");  // Default empty string if not found
  String passwordStr = preferences.getString("password", "");

  // Safely copy into char arrays with null termination
  strncpy(ssidValue, ssidStr.c_str(), sizeof(ssidValue) - 1);
  ssidValue[sizeof(ssidValue) - 1] = '\0';  // Ensure null termination

  strncpy(passwordValue, passwordStr.c_str(), sizeof(passwordValue) - 1);
  passwordValue[sizeof(passwordValue) - 1] = '\0';  // Ensure null termination

  Serial.println("Loaded Wi-Fi credentials:");
  Serial.print("SSID: ");
  Serial.println(ssidValue);
  Serial.print("Password: ");
  Serial.println(passwordValue);
}

// Connect to Wi-Fi
void connectToWiFi() {
  WiFi.disconnect();
  WiFi.begin(ssidValue, passwordValue);
  
  Serial.print("Connecting to Wi-Fi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    saveLogsToFirebase();
    Serial.println("\nConnected to Wi-Fi.");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect. Check credentials.");
  }
}

// Check for updated Wi-Fi credentials in Firebase
void checkForWiFiUpdate() {
  if (Firebase.get(firebaseData, "/devices/" + fullMachineName + "/credentials")) {
    if (firebaseData.dataType() == "json") {
      FirebaseJson &json = firebaseData.jsonObject();
      FirebaseJsonData jsonData;

      if (json.get(jsonData, "ssid") && jsonData.type == "string") {
        String newSSID = jsonData.stringValue;
        if (json.get(jsonData, "password") && jsonData.type == "string") {
          String newPassword = jsonData.stringValue;

          if (newSSID != String(ssidValue) || newPassword != String(passwordValue)) {
            Serial.println("New Wi-Fi credentials detected. Reconnecting...");
            saveWiFiCredentials(newSSID.c_str(), newPassword.c_str());
            strncpy(ssidValue, newSSID.c_str(), sizeof(ssidValue));
            strncpy(passwordValue, newPassword.c_str(), sizeof(passwordValue));
            connectToWiFi();  // Reconnect with new credentials
          }
        } else {
          Serial.println("Password not found or invalid.");
        }
      } else {
        Serial.println("SSID not found or invalid.");
      }
    }
  } else {
    Serial.println("Failed to read Firebase: " + firebaseData.errorReason());
  }
}

void saveCredentialsToFirebase() {
  if (tempSSID.isEmpty() || tempPassword.isEmpty()) {
    Serial.println("No credentials to save to Firebase");
    return;
  }

  // Example Firebase code (replace with actual implementation)
  String path = "devices/" + fullMachineName + "/credentials/";
  Firebase.setString(firebaseData, path + "ssid", tempSSID);
  Firebase.setString(firebaseData, path + "password", tempPassword);
  // Clear the temporary credentials after saving to Firebase
  tempSSID = "";
  tempPassword = "";
}

// Function to convert encryption type to string
String encryptionTypeToString(wifi_auth_mode_t encryptionType) {
  switch (encryptionType) {
    case WIFI_AUTH_OPEN: return "Open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA_WPA2_PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
    default: return "Unknown";
  }
}

void firebaseWifiScan() {
  // Scan for networks
  int numNetworks = WiFi.scanNetworks();
  Serial.println("Scanning for Wi-Fi networks...");

  FirebaseJson jsonObject;  // Create a FirebaseJson object to store the data

  for (int i = 0; i < numNetworks; ++i) {
    FirebaseJson networkObject;  // Create a JSON object for each network

    // Add network details
    networkObject.set("ssid", WiFi.SSID(i));
    networkObject.set("encryptionType", encryptionTypeToString(WiFi.encryptionType(i))); // Convert enum to string

    // Add the network object to the main JSON object with a numbered key
    jsonObject.set(String(i), networkObject);
  }

  WiFi.scanDelete();  // Clear the list of scanned networks

  // Send the JSON object to Firebase
  String path = "/devices/" + fullMachineName + "/wifiScan";  // Adjust path as needed
  if (Firebase.set(firebaseData, path, jsonObject)) {
    Serial.println("Wi-Fi scan results uploaded to Firebase.");
  } else {
    Serial.println("Failed to upload Wi-Fi scan results: " + firebaseData.errorReason());
  }
}


