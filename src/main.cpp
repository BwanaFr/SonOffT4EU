#include <Arduino.h>
#define ATOMIC_FS_UPDATE
#include <Updater.h>

//Includes for the webserver and captive portal
#include <ESPAsyncWebServer.h>
#include <ESPEasyCfg.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#ifndef TEST
#define RELAY_PIN 12
#define BUTTON_PIN 0
#define LED_PIN 13
#define BTN_LED_PIN 5
#else
#define RELAY_PIN 5
#define BUTTON_PIN 0
#define LED_PIN LED_BUILTIN
#define BTN_LED_PIN 16
#endif

const int LED_ON_BIT = 1<<31;
const int LED_PWM_MAX = 1023;

//Objects for captive portal/MQTT
AsyncWebServer server(80);
ESPEasyCfg captivePortal(&server, "Sonoff T4EU");
//Custom application parameters
ESPEasyCfgParameterGroup mqttParamGrp("MQTT");
ESPEasyCfgParameter<String> mqttServer("mqttServer", "MQTT server", "", "Name or IP of MQTT broker. Empty if not using MQTT.");
ESPEasyCfgParameter<String> mqttUser("mqttUser", "MQTT username", "homeassistant");
ESPEasyCfgParameter<String> mqttPass("mqttPass", "MQTT password", "");
ESPEasyCfgParameter<int> mqttPort("mqttPort", "MQTT port", 1883);
ESPEasyCfgParameter<String> mqttName("mqttName", "MQTT name", "T4EU", "", "{\"required\":\"\"}");

ESPEasyCfgParameterGroup switchParamGrp("Switch");
ESPEasyCfgParameter<uint32_t> swLongPress("swLongPress", "Long press duration (ms)", 2000, "", "{\"min\":\"100\",\"max\":\"60000\"}");
ESPEasyCfgEnumParameter swMode("swMode", "Switch mode", "BASIC;SMART");
ESPEasyCfgParameter<int> wifiLedOnValue("ledValue", "Wifi LED on value", LED_ON_BIT | LED_PWM_MAX);
ESPEasyCfgParameter<int> buttonLedOnValue("btnLedValue", "Button LED on value", LED_ON_BIT | LED_PWM_MAX);

//MQTT objects
WiFiClient espClient;                                   // TCP client
PubSubClient client(espClient);                         // MQTT object
const unsigned long mqttPostingInterval = 10L * 1000L;  // Delay between updates, in milliseconds
static unsigned long mqttLastPostTime = 0;              // Last time you sent to the server, in milliseconds
String mqttRelayService;                                // Relay MQTT service name
String mqttModeService;                                 // Mode MQTT service name
String mqttStatusService;                               // Status MQTT service name
String mqttWifiLedStatusService;                        // Wifi LED status service name
String mqttButtonLedStatusService;                      // Button LED status service name

uint32_t lastMQTTConAttempt = 0;                        // Last MQTT connection attempt
enum class MQTTConState {Connecting, Connected, Disconnected, NotUsed};
MQTTConState mqttState = MQTTConState::Disconnected;

//OTA variables
size_t content_len;

//Switch management
enum class SwitchState {IDLE, PRESSED_SHORT, PRESSED_LONG, RELEASED_SHORT, RELEASED_LONG};
SwitchState swState = SwitchState::IDLE;
const int WIFI_LED_RATE = 500;
const int MQTT_LED_RATE = 100;
const int AP_LED_RATE = 2000;
const int NO_LED_RATE = -1;
int ledBlinkRate = WIFI_LED_RATE;
uint32_t lastBlinkTime = 0;
int previousWifiLedValue = -1;


/**
 * Tests if the LED parameter is ON
 */
bool ledParamToOn(int ledParamValue) {
  return (ledParamValue & LED_ON_BIT) != 0;
}

/**
 * Get the led brightness 0-1023
 */
int ledParamToBrightness(int ledParamValue) {
  return ledParamValue & ~LED_ON_BIT;
}

/**
 * Convert state and brightness to param value
 */
int toLedParam(int brightness, bool on){
  if(on){
    return brightness | LED_ON_BIT;
  }else{
    return brightness;
  }
}

/**
 * Convert the led param value to PWM value
 */
int ledParamToPWM(int ledParamValue) {
  int ret = LED_PWM_MAX;
  if(ledParamToOn(ledParamValue)){
    ret = LED_PWM_MAX - ledParamToBrightness(ledParamValue);
  }
  return ret;
}

/**
 * Sets the LED value
 * @param payload JSON payload to set state/brightness
 * @param param Parameter to be saved in flash
 **/
void setLedValue(const String& payload, ESPEasyCfgParameter<int>& param)
{
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, payload);

  // Test if parsing succeeds.
  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.c_str());
    return;
  }
  const char* state = doc["state"];
  //Gets brightness
  int brightness = ledParamToBrightness(param.getValue());
  bool ledOn = ledParamToOn(param.getValue());
  
  if(doc.containsKey("brightness")){
    brightness = doc["brightness"];
    if(brightness<0){
      brightness = 0;
    }else if(brightness>LED_PWM_MAX){
      brightness = LED_PWM_MAX;
    }
  }
  if(state != NULL){
    if((strcmp(state, "OFF") == 0) || (strcmp(state, "off") == 0)){
      ledOn = false;
    }else if((strcmp(state, "ON") == 0) || (strcmp(state, "on") == 0)){
      ledOn = true;
    }
  }
  
  brightness = toLedParam(brightness, ledOn);
  if(brightness != param.getValue()){
    param.setValue(brightness);
    captivePortal.saveParameters();
  }
}

/**
 * Callback of MQTT
 */
void callback(char* topic, byte* payload, unsigned int length) {
  String data;
  for (unsigned int i = 0; i < length; i++) {
    data += (char)payload[i];
  }
  String strTopic(topic);
  if(strTopic == mqttRelayService){
    if(data == "ON"){
      digitalWrite(RELAY_PIN, HIGH);
    }else{
      digitalWrite(RELAY_PIN, LOW);
    }
  }else if(strTopic == (mqttWifiLedStatusService + "/set")){
    setLedValue(data, wifiLedOnValue);
  }else if(strTopic == (mqttButtonLedStatusService + "/set")){
    setLedValue(data, buttonLedOnValue);
  }else if(strTopic == mqttModeService){
    if(data == "BASIC" || data == "SMART"){
      swMode.setValue(data.c_str());
      captivePortal.saveParameters();
    }
  }
  mqttLastPostTime = 0;
}

void configureMQTTServices(){
  if(!mqttServer.getValue().isEmpty()){
      //Build MQTT service names
    mqttStatusService =  mqttName.getValue() +  "/Status";
    mqttRelayService =  mqttName.getValue() + "/Relay";
    mqttModeService =  mqttName.getValue() + "/Mode";
    mqttWifiLedStatusService = mqttName.getValue() + "/WifiLED";
    mqttButtonLedStatusService = mqttName.getValue() + "/ButtonLED";

    //Setup MQTT client callbacks and port
    client.setServer(mqttServer.getValue().c_str(), mqttPort.getValue());
    client.setCallback(callback);
    mqttState = MQTTConState::Connecting;
  }else{
    mqttState = MQTTConState::NotUsed;
  }
}

/**
 * Call back on parameter change
 */
void newState(ESPEasyCfgState state) {
  if(state == ESPEasyCfgState::Reconfigured){
    //Don't use MQTT if server is not filled
    if(mqttServer.getValue().isEmpty()){
      mqttState = MQTTConState::NotUsed;
      swMode.setValue("BASIC");
      ledBlinkRate = NO_LED_RATE;
    }else{
      configureMQTTServices();
      mqttLastPostTime = 0;
      client.disconnect();
    }
  }else if(state == ESPEasyCfgState::Connected){
    if(mqttServer.getValue().isEmpty()){
      ledBlinkRate = NO_LED_RATE;
      previousWifiLedValue = -1;
    }
  }else if(state == ESPEasyCfgState::AP){
    ledBlinkRate = AP_LED_RATE;
  }
}

/**
 * Print value to a JSON string
 */
void publishValuesToJSON(String& str){
  StaticJsonDocument<210> root;
  root["relay"] = (bool)digitalRead(RELAY_PIN);
  switch(swState){
    case SwitchState::IDLE:
      root["switch"] = "IDLE";
      break;
    case SwitchState::PRESSED_SHORT:
      root["switch"] = "PRESSED_SHORT";
      break;
    case SwitchState::PRESSED_LONG:
      root["switch"] = "PRESSED_LONG";
      break;
    case SwitchState::RELEASED_SHORT:
      root["switch"] = "RELEASED_SHORT";
      break;
    case SwitchState::RELEASED_LONG:
      root["switch"] = "RELEASED_LONG";
      break;
  }
  root["mode"] = swMode.toString();
  serializeJson(root, str);
}

/**
 * Prints LED value to JSON
 **/
void publishLedToJSON(String& str, int brightness) {
  str = "";
  const size_t capacity = JSON_OBJECT_SIZE(2) + 30;
  StaticJsonDocument<capacity> root;
  root["brightness"] = ledParamToBrightness(brightness);
  root["state"] = ledParamToOn(brightness) ? "ON" : "OFF";
  serializeJson(root, str);
}

/**
 * Callback of the update request
 */
void handleUpdate(AsyncWebServerRequest *request) {
  String html = "Last firmware build :";
  html += __DATE__;
  html += "&nbsp;";
  html += __TIME__;
  html += "<form method='POST' action='/doUpdate' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Update'></form>";
  request->send(200, "text/html", html.c_str());
}

/**
 * Callback of update the file upload
 */
void handleDoUpdate(AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (!index){
    Serial.println("Update");
    content_len = request->contentLength();
    // if filename includes spiffs, update the spiffs partition
    int cmd = (filename.indexOf("spiffs") > -1) ? U_FS : U_FLASH;
    Update.runAsync(true);
    if (!Update.begin(content_len, cmd)) {
      Update.printError(Serial);
    }
  }

  if (Update.write(data, len) != len) {
    Update.printError(Serial);
  } else {
    Serial.printf("Progress: %d%%\n", (Update.progress()*100)/Update.size());
  }

  if (final) {
    AsyncWebServerResponse *response = request->beginResponse(302, "text/plain", "Please wait while the device reboots");
    response->addHeader("Refresh", "20");  
    response->addHeader("Location", "/");
    request->send(response);
    if (!Update.end(true)){
      Update.printError(Serial);
    } else {
      Serial.println("Update complete");
      Serial.flush();
      ESP.restart();
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BTN_LED_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT);
  digitalWrite(RELAY_PIN, 0);
  byte mac[6];                     // the MAC address of your Wifi shield
  WiFi.macAddress(mac);
  String clientId = "May MAC is : ";
  clientId += String(mac[0], HEX);
  clientId += String(mac[1], HEX);
  clientId += String(mac[2], HEX);
  clientId += String(mac[3], HEX);
  clientId += String(mac[4], HEX);
  clientId += String(mac[5], HEX);
  Serial.println(clientId);

  captivePortal.setLedPin(0xFF);
  captivePortal.setLedActiveLow(true);
   //Register custom parameters
  mqttPass.setInputType("password");
  mqttParamGrp.add(&mqttServer);
  mqttParamGrp.add(&mqttUser);
  mqttParamGrp.add(&mqttPass);
  mqttParamGrp.add(&mqttPort);
  mqttParamGrp.add(&mqttName);
  captivePortal.addParameterGroup(&mqttParamGrp);
  switchParamGrp.add(&swLongPress);
  switchParamGrp.add(&swMode);
  wifiLedOnValue.setHidden(true);
  buttonLedOnValue.setHidden(true);
  switchParamGrp.add(&wifiLedOnValue);
  switchParamGrp.add(&buttonLedOnValue);
  captivePortal.addParameterGroup(&switchParamGrp);
  captivePortal.setStateHandler(newState);
  captivePortal.begin();
  server.begin();

  //MQTT services
  configureMQTTServices();
  

  //Serve HTTP pages (JSON values)
  server.on("/values", HTTP_GET, [=](AsyncWebServerRequest *request){
      String json;
      publishValuesToJSON(json);
      AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
      response->addHeader("Access-Control-Allow-Origin", "*");
      request->send(response);
    });
  //OTA update
  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){handleUpdate(request);});
  server.on("/doUpdate", HTTP_POST,
    [](AsyncWebServerRequest *request) {},
    [](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data,
                  size_t len, bool final) {handleDoUpdate(request, filename, index, data, len, final);}
  );
  Serial.print("Version ");
  Serial.print(" ");
  Serial.print(__DATE__);
  Serial.print(" ");
  Serial.println(__TIME__);
}

void publishValuesToMQTT(){
  //Publish to MQTT clients
  if(client.connected()){
    String msg;
    publishValuesToJSON(msg);
    client.publish(mqttStatusService.c_str(), msg.c_str());
    
    publishLedToJSON(msg, wifiLedOnValue.getValue());
    client.publish(mqttWifiLedStatusService.c_str(), msg.c_str());

    publishLedToJSON(msg, buttonLedOnValue.getValue());
    client.publish(mqttButtonLedStatusService.c_str(), msg.c_str());
  }
}

void reconnect() {
  //Don't use MQTT if server is not filled
  if(mqttServer.getValue().isEmpty()){
    ledBlinkRate = NO_LED_RATE;
    previousWifiLedValue = -1;
    return;
  }
  // Loop until we're reconnected
  if (!client.connected() && ((millis()-lastMQTTConAttempt)>5000)) {
    if(captivePortal.getState() == ESPEasyCfgState::Connected){
      ledBlinkRate = MQTT_LED_RATE;
    }else if(captivePortal.getState() == ESPEasyCfgState::AP){
      ledBlinkRate = AP_LED_RATE;
    }else{
      ledBlinkRate = WIFI_LED_RATE;
    }
    
    mqttState = MQTTConState::Connecting;
    IPAddress mqttServerIP;
    int ret = WiFi.hostByName(mqttServer.getValue().c_str(), mqttServerIP);
    if(ret != 1){
      Serial.print("Unable to resolve hostname: ");
      Serial.print(mqttServer.getValue().c_str());
      Serial.println(" try again in 5 seconds");
      lastMQTTConAttempt = millis();
      return;
    }
    Serial.print("Attempting MQTT connection to ");
    Serial.print(mqttServer.getValue().c_str());
    Serial.print(':');
    Serial.print(mqttPort.getValue());
    Serial.print('(');
    Serial.print(mqttServerIP);
    Serial.print(")...");
    // Create a Client ID baased on MAC address
    byte mac[6];                     // the MAC address of your Wifi shield
    WiFi.macAddress(mac);
    String clientId = "SonoffT4EU-";
    clientId += String(mac[3], HEX);
    clientId += String(mac[4], HEX);
    clientId += String(mac[5], HEX);
    // Attempt to connect
    client.setServer(mqttServerIP, mqttPort.getValue());
    if((ret == 1) && (client.connect(clientId.c_str(), mqttUser.getValue().c_str(), mqttPass.getValue().c_str()))) {
      Serial.println("connected");
      mqttState = MQTTConState::Connected;
      client.subscribe(mqttRelayService.c_str());
      client.subscribe((mqttWifiLedStatusService + "/set").c_str());
      client.subscribe((mqttButtonLedStatusService + "/set").c_str());
      client.subscribe(mqttModeService.c_str());
      ledBlinkRate = NO_LED_RATE; 
      previousWifiLedValue = -1;
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      client.disconnect();
      mqttState = MQTTConState::Disconnected;
      lastMQTTConAttempt = millis();
    }
  }
}
/**
 * Simple function to read switch press duration
 */
SwitchState getSwitchState(bool &changed, uint32_t now){
  static SwitchState lastSwState = SwitchState::IDLE;
  static uint32_t lastPress = 0;
  static bool lastState = false;
  SwitchState ret = SwitchState::IDLE;
  bool swState = !digitalRead(BUTTON_PIN);
  if(swState){
    if(swState != lastState){
      //Rising edge
      lastPress = now;
    }
    uint32_t duration = now - lastPress;
    if(duration>swLongPress.getValue()){
      ret = SwitchState::PRESSED_LONG;
    }else{
      ret = SwitchState::PRESSED_SHORT;
    }
  }else{
    if(swState != lastState){
      //Falling edge
      if(lastSwState == SwitchState::PRESSED_SHORT){
        ret = SwitchState::RELEASED_SHORT;
      }else{
        ret = SwitchState::RELEASED_LONG;
      }
    }
  }
  if(lastSwState != ret){
    changed = true;
  }else{
    changed = false;
  }
  lastState = swState;
  lastSwState = ret;
  return ret;
}

void loop() {
  static int previousButtonLedValue = -1;
  uint32_t now = millis();
  //Keep captive portal active
  captivePortal.loop();

  int wifiLedValue = wifiLedOnValue.getValue();
  //Touch switch logic 
  bool changed = false;
  swState = getSwitchState(changed, now);
  if(changed){
    //Switch state changed, send to MQTT
    mqttLastPostTime = 0;
    if(swMode.toString() == "BASIC"){
      //Basic mode, no smart bulb
      if(swState == SwitchState::RELEASED_SHORT)
        digitalWrite(RELAY_PIN, !digitalRead(RELAY_PIN));
        if(!digitalRead(RELAY_PIN))
          wifiLedValue = 0;
    }else if(swMode.toString() == "SMART"){
      //Smart bulb
      if(mqttState != MQTTConState::Connected){
        //Act like in basic mode
        if(swState == SwitchState::RELEASED_SHORT)
          digitalWrite(RELAY_PIN, !digitalRead(RELAY_PIN));
          if(!digitalRead(RELAY_PIN))
            wifiLedValue = 0;
      }else{
        //Force relay closed
        digitalWrite(RELAY_PIN, true);
        //Long press, change to BASIC mode
        if(swState == SwitchState::PRESSED_LONG){
          digitalWrite(RELAY_PIN, false);
          swMode.setValue("BASIC");
          captivePortal.saveParameters();
        }
      }
    }
  }

  //MQTT management
  if (mqttState != MQTTConState::NotUsed){
      if(!client.loop()) {
        //Not connected of problem with updates
        reconnect();
      }else{
        //Ok, we can publish
        if((now-mqttLastPostTime)>mqttPostingInterval){
          publishValuesToMQTT();
          mqttLastPostTime = now;
        }
      }
  }
  if(ledBlinkRate>0){
    if((now - lastBlinkTime) > static_cast<uint32_t>(ledBlinkRate)){
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      lastBlinkTime = now;
    }
  }else{
    if(previousWifiLedValue != wifiLedValue){
      analogWrite(LED_PIN, ledParamToPWM(wifiLedValue));
      previousWifiLedValue = wifiLedValue;
    }
  }

  int buttonLedValue = buttonLedOnValue.getValue();
  if(previousButtonLedValue != buttonLedValue){
      analogWrite(BTN_LED_PIN, ledParamToPWM(buttonLedValue));
      previousButtonLedValue = buttonLedValue;
  }
  
  yield();
}