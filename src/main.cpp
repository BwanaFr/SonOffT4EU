#include <Arduino.h>


//Includes for the webserver and captive portal
#include <ESPAsyncWebServer.h>
#include <ESPEasyCfg.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#define RELAY_PIN 12
#define BUTTON_PIN 0
#define LED_PIN 13

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

//MQTT objects
WiFiClient espClient;                                   // TCP client
PubSubClient client(espClient);                         // MQTT object
const unsigned long mqttPostingInterval = 10L * 1000L;  // Delay between updates, in milliseconds
static unsigned long mqttLastPostTime = 0;              // Last time you sent to the server, in milliseconds
static char mqttRelayService[128];                      // Relay MQTT service name
static char mqttStatusService[128];                     // Status MQTT service name
uint32_t lastMQTTConAttempt = 0;                        //Last MQTT connection attempt
enum class MQTTConState {Connecting, Connected, Disconnected, NotUsed};
MQTTConState mqttState = MQTTConState::Disconnected;

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
    }else{
      mqttState = MQTTConState::Connecting;
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
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  captivePortal.setLedPin(LED_BUILTIN);
  captivePortal.setLedActiveLow(true);
   //Register custom parameters
  mqttPass.setInputType("password");
  mqttParamGrp.add(&mqttServer);
  mqttParamGrp.add(&mqttUser);
  mqttParamGrp.add(&mqttPass);
  mqttParamGrp.add(&mqttPort);
  mqttParamGrp.add(&mqttName);
  captivePortal.addParameterGroup(&mqttParamGrp);
  captivePortal.setStateHandler(newState);
  captivePortal.begin();
  server.begin();

  //MQTT services
  //Build MQTT service names
  snprintf(mqttStatusService, 128, "%s/Status", mqttName.getValue().c_str());
  snprintf(mqttRelayService, 128, "%s/Relay", mqttName.getValue().c_str());
  //Setup MQTT client callbacks and port
  client.setServer(mqttServer.getValue().c_str(), mqttPort.getValue());
  client.setCallback(callback);
  if(mqttServer.getValue().isEmpty()){
    mqttState = MQTTConState::NotUsed;
  }

  //Serve HTTP pages
  server.on("/values", HTTP_GET, [=](AsyncWebServerRequest *request){
      String json;
      publishValuesToJSON(json);
      AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
      response->addHeader("Access-Control-Allow-Origin", "*");
      request->send(response);
    });
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

void loop() {
  uint32_t now = millis();
  captivePortal.loop();
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

  yield();
}