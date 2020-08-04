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
#else
#define RELAY_PIN 16
#define BUTTON_PIN 0
#define LED_PIN LED_BUILTIN
#endif

//Objects for captive portal/MQTT
AsyncWebServer server(80);
ESPEasyCfg captivePortal(&server, "Sonoff T4EU");
//Custom application parameters
ESPEasyCfgParameterGroup mqttParamGrp("MQTT");
ESPEasyCfgParameter<String> mqttServer("mqttServer", "MQTT server", "");
ESPEasyCfgParameter<String> mqttUser("mqttUser", "MQTT username", "homeassistant");
ESPEasyCfgParameter<String> mqttPass("mqttPass", "MQTT password", "");
ESPEasyCfgParameter<int> mqttPort("mqttPort", "MQTT port", 1883);
ESPEasyCfgParameter<String> mqttName("mqttName", "MQTT name", "T4EU");

ESPEasyCfgParameterGroup switchParamGrp("Switch");
ESPEasyCfgParameter<uint32_t> swLongPress("swLongPress", "Long press duration (ms)", 2000);
ESPEasyCfgEnumParameter swMode("swMode", "Switch mode", "BASIC;SMART");

//MQTT objects
WiFiClient espClient;                                   // TCP client
PubSubClient client(espClient);                         // MQTT object
const unsigned long mqttPostingInterval = 10L * 1000L;  // Delay between updates, in milliseconds
static unsigned long mqttLastPostTime = 0;              // Last time you sent to the server, in milliseconds
static char mqttRelayService[128];                      // Relay MQTT service name
static char mqttLedService[128];                        // LED MQTT service name
static char mqttStatusService[128];                     // Status MQTT service name
uint32_t lastMQTTConAttempt = 0;                        // Last MQTT connection attempt
enum class MQTTConState {Connecting, Connected, Disconnected, NotUsed};
MQTTConState mqttState = MQTTConState::Disconnected;

//OTA variables
size_t content_len;

//Switch management
enum class SwitchState {IDLE, PRESSED_SHORT, PRESSED_LONG, RELEASED_SHORT, RELEASED_LONG};
SwitchState swState = SwitchState::IDLE;
uint16_t ledOnValue = 0;

/**
 * Call back on parameter change
 */
void newState(ESPEasyCfgState state) {
  if(state == ESPEasyCfgState::Reconfigured){
    Serial.println("ESPEasyCfgState::Reconfigured");
    //client.disconnect();
    //Don't use MQTT if server is not filled
    if(mqttServer.getValue().isEmpty()){
      mqttState = MQTTConState::NotUsed;
      swMode.setValue("BASIC");
    }else{
      client.disconnect();
    }
  }else if(state == ESPEasyCfgState::Connected){
    Serial.println("ESPEasyCfgState::Connected");
    Serial.print("DNS: ");
    Serial.println(WiFi.dnsIP());
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
  root["LED"] = ledOnValue;
  serializeJson(root, str);
}

/**
 * Callback of MQTT
 */
void callback(char* topic, byte* payload, unsigned int length) {
  String data;
  for (unsigned int i = 0; i < length; i++) {
    data += (char)payload[i];
  }
  if(strcmp(topic, mqttRelayService) == 0){
    if(data == "ON"){
      digitalWrite(RELAY_PIN, HIGH);
    }else{
      digitalWrite(RELAY_PIN, LOW);
    }
  }else if(strcmp(topic, mqttLedService) == 0){
    ledOnValue = data.toInt();
    if(ledOnValue > 1023){
      ledOnValue = 1023;
    }else if(ledOnValue < 0){
      ledOnValue = 0;
    }
  }
}

/**
 * Callback of the update request
 */
void handleUpdate(AsyncWebServerRequest *request) {
  char* html = "<form method='POST' action='/doUpdate' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Update'></form>";
  request->send(200, "text/html", html);
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
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT);
  digitalWrite(RELAY_PIN, 0);

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
  captivePortal.addParameterGroup(&switchParamGrp);
  captivePortal.setStateHandler(newState);
  captivePortal.begin();
  server.begin();

  //MQTT services
  //Build MQTT service names
  snprintf(mqttStatusService, 128, "%s/Status", mqttName.getValue().c_str());
  snprintf(mqttRelayService, 128, "%s/Relay", mqttName.getValue().c_str());
  snprintf(mqttLedService, 128, "%s/Led", mqttName.getValue().c_str());
  //Setup MQTT client callbacks and port
  client.setServer(mqttServer.getValue().c_str(), mqttPort.getValue());
  client.setCallback(callback);
  if(mqttServer.getValue().isEmpty()){
    mqttState = MQTTConState::NotUsed;
  }

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
    client.publish(mqttStatusService, msg.c_str());
  }
}

void reconnect() {
  //Don't use MQTT if server is not filled
  if(mqttServer.getValue().isEmpty()){
    return;
  }
  // Loop until we're reconnected
  if (!client.connected() && ((millis()-lastMQTTConAttempt)>5000)) {
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
      client.subscribe(mqttRelayService);
      client.subscribe(mqttLedService);
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
  static int previousLedValue = 0;
  uint32_t now = millis();
  //Keep captive portal active
  captivePortal.loop();
  
  int ledValue = ledOnValue;

  //Touch switch logic 
  bool changed = false;
  swState = getSwitchState(changed, now);
  if(changed){
    //Switch state changed, send to MQTT
    mqttLastPostTime = 0;
    if(swMode.toString() == "BASIC"){
      Serial.println("Basic mode");
      //Basic mode, no smart bulb
      if(swState == SwitchState::RELEASED_SHORT)
        digitalWrite(RELAY_PIN, !digitalRead(RELAY_PIN));
        if(!digitalRead(RELAY_PIN))
          ledValue = 0;
    }else if(swMode.toString() == "SMART"){
      Serial.println("Smart mode");
      Serial.print("LED -> ");
      Serial.println(ledValue);
      //Smart bulb
      if(mqttState != MQTTConState::Connected){
        //Act like the basic mode
        if(swState == SwitchState::RELEASED_SHORT)
          digitalWrite(RELAY_PIN, !digitalRead(RELAY_PIN));
          if(!digitalRead(RELAY_PIN))
            ledValue = 0;
      }else{
        //Long press, toggle relay
        if(swState == SwitchState::PRESSED_LONG){
          digitalWrite(RELAY_PIN, !digitalRead(RELAY_PIN));
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
  if(previousLedValue != ledValue)
    analogWrite(LED_PIN, ledValue);
  previousLedValue = ledValue;
  yield();
}