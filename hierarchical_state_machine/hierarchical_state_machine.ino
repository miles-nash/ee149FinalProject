#include <Arduino.h>
#include <Wire.h>
#include <Ticker.h>
#include <Adafruit_MCP9808.h>
#include <Adafruit_AMG88xx.h>
#include <Adafruit_NeoPixel.h>

#define SCL 22 // SCL PIN on our ESP32 
#define SDA 23 // SDA PIN on our ESP32

#define CLK_PIN 12    // Rotary encoder clock pin
#define DT_PIN 27     // Rotary encoder data pin
#define SW_PIN 33     // Rotary encoder switch pin

#define FAN_PWM_PIN 21 // PWM output pin for fan

#define AMG_COLS 8    // IR Camera col
#define AMG_ROWS 8    // IR Camera row

#define PIR_PIN 32    // PIR Sensor Pin

#define NEO_PIN 13    // NeoPixel Pin
#ifdef __AVR__
 #include <avr/power.h> // Required for 16 MHz Adafruit Trinket
#endif
#define LED_COUNT 24

#include <WiFi.h>
#include <PubSubClient.h>

// WiFi
const char *ssid = ""; // Enter your Wi-Fi name
const char *password = "";  // Enter Wi-Fi password

// MQTT Broker
const char *mqtt_broker = "broker.emqx.io";
const char *topic = "IoTFan2/esp32";
const char *mqtt_username = "";
const char *mqtt_password = "";
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

// Variables
int power = 0; // Range 0-100
int current_power = 0;
int curr_temp; // Current temperature
int pref_temp = 16; // Preferred temperature
int camera_max; // max camera pixel temp

bool present = false;
bool sw_press = false;
bool prev_press = false;
bool camera_flag = false;
bool camera_active = false;

int clk_count = 0;
int curr_clk;
int prev_clk;
int dir = 0;
String currentDir;


unsigned long lastPressTime = 0;
unsigned long lastClickTime;
const unsigned long debounceDelay = 5; // 50 ms debounce delay

// Create Temperature Sensor object
Adafruit_MCP9808 tempsensor = Adafruit_MCP9808();

// AMG88xx IR Sensor
Adafruit_AMG88xx camera;
float pixels[AMG_COLS * AMG_ROWS]; // Sensor 1D data array

// Declare our NeoPixel strip object:
Adafruit_NeoPixel strip(LED_COUNT, NEO_PIN, NEO_GRB + NEO_KHZ800);

// Declare timers for presence, temp sensor, thermal camera, and MQTT
Ticker presentTimer;
Ticker tempTimer;
Ticker cameraTimer;
Ticker mqttTimer;
Ticker encoderTimer;

// States
enum MainState { OFF, MANUAL, AUTOMATIC };
enum SubState { SUB_OFF, SUB_ON };

MainState mainState = OFF;
SubState subState = SUB_OFF;


void setup() {

  Serial.begin(115200);

  // Rotaty Encoder
  pinMode(CLK_PIN, INPUT);
  pinMode(DT_PIN, INPUT);
  pinMode(SW_PIN, INPUT_PULLUP);

  // Fan PWN
  pinMode(FAN_PWM_PIN, OUTPUT);

  // PIR Sensor
  pinMode(PIR_PIN, INPUT);

  // Set up I2C communication pins
  Wire.begin(SDA, SCL);

  // Initialize Temperature Sensor
  while (!tempsensor.begin(0x18)) { // Default I2C address is 0x18
    Serial.println("Couldn't find MCP9808 sensor! Check wiring.");
    Serial.println("Try connecting the board to power.");
    delay(1000);
     // Halt execution
  }

  Serial.println("MCP9808 sensor initialized.");

 // initialize thermal camera
  while (!camera.begin()) {
    Serial.println("Could not find a valid AMG88xx sensor. Check wiring!");
    delay(1000);
  }
  
  Serial.println("AMG8833 Thermal Camera initialized!");

  
  // start the temp sensor
  tempsensor.setResolution(3); // Set resolution: 0 - low (0.5°C), 1 - medium (0.25°C), 2 - high (0.125°C), 3 - max (0.0625°C)
  updateCurrTemp(); // ensures an initial temperature value
  updateCameraMax(); // grabs an initial picture of the environment

  #if defined(__AVR_ATtiny85__) && (F_CPU == 16000000)
  clock_prescale_set(clock_div_1);
  #endif

  // configure wifi
  configureMQTT();

  // initialize the LED strip
  strip.begin();
  strip.show();

  // get first encoder reading
  //prev_clk = digitalRead(CLK_PIN);

  // set up polling timers
  presentTimer.attach(5.0, checkPresent); // checks if someone is present every 5 seconds
  tempTimer.attach(10.0, updateCurrTemp); // updates the temperature every 5 seconds
  mqttTimer.attach(10.0, publishTempState); // sets timer to publish mqtt data
  encoderTimer.attach(0.0001,readEncoder);

  lastClickTime = digitalRead(CLK_PIN);
  prev_clk = LOW;
}

void loop() {

  //client.loop();
  //readEncoder();
  /*	curr_clk = digitalRead(CLK_PIN);
  if (curr_clk != prev_clk  && curr_clk == 1){
    if (digitalRead(DT_PIN) != curr_clk) {
			if(clk_count > 1){
        clk_count --;
			  currentDir ="CCW";
      }
		} else {
      if(clk_count < 10){
        clk_count ++;
			  currentDir ="CW";
      }
		}
    Serial.print("Direction: ");
		Serial.print(currentDir);
		Serial.print(" | Counter: ");
		Serial.println(clk_count);
	}
  prev_clk = curr_clk;*/




  updateMainState();

  // State handling
  switch (mainState) {
    case OFF:
      power = 0;
      camera_flag = false;
      strip.clear();
      strip.show();
      updateDutyCycle();
      break;

    case MANUAL:
      handleManualState();
      lightLEDs(strip.Color(10, 55, 255));
      //power = 0;
      break;

    case AUTOMATIC:
      handleAutomaticState();
      lightLEDs(strip.Color(85, 255, 9));
      break;
  }
  //delay(0); //maybe needed?
}


void handleManualState() {

  //readEncoder();

 /* if (!present) {
    subState = SUB_OFF;
    power = 0;
  } else {
    power = clk_count;
    subState = SUB_ON;
  } */

  //double check if we need to update duty cycle...
  if (power != clk_count){
   // if(subState == SUB_ON){
     //     power = clk_count;

//    }
    power = clk_count;
    // Update substate
    //subState = (power > 0) ? SUB_ON : SUB_OFF;

    //print(subState);
    updateDutyCycle();
  }
  //delay(1);
}

void handleAutomaticState() {
  updateCurrTemp();
  if (!present) {
    subState = SUB_OFF;
    power = 0;
  } else {
    if (curr_temp > pref_temp) {
      int diff = curr_temp - pref_temp;
      if (diff >= 10) {
        power = 100;  // Max power
      }      
      else { 
        power = 10 * diff;
      }   // Proportional power
    } else {
      power = 0; // Turn off
    }
  }

  // Update substate
  subState = (power > 0) ? SUB_ON : SUB_OFF;
  updateDutyCycle();
  Serial.println("curr temp:" + String(curr_temp) + " pref temp: " + String(pref_temp) + " power:" + String(power));
  delay(1);
}

void readEncoder() {
  	curr_clk = digitalRead(CLK_PIN);
  if (curr_clk != prev_clk  && curr_clk == 1){
    if (digitalRead(DT_PIN) == curr_clk) {
			if(clk_count > 0){
        clk_count -= 10;
			  currentDir ="CCW";
      }
		} else {
      if(clk_count < 100){
        clk_count += 10;
			  currentDir ="CW";
      }
		}
    Serial.print("Direction: ");
		Serial.print(currentDir);
		Serial.print(" | Counter: ");
		Serial.println(clk_count);
	}
  prev_clk = curr_clk;
 }

void updateMainState() {

  bool buttonState = digitalRead(SW_PIN);

  if (buttonState != prev_press) {
    if (millis() - lastPressTime > debounceDelay) {
      lastPressTime = millis();
      
      if (buttonState == LOW) {
        //on press advances state and updates the led colors
        mainState = static_cast<MainState>((mainState + 1) % 3);
        if (mainState == OFF){
            presentTimer.detach(); // checks if someone is present every 5 seconds
            tempTimer.detach(); // updates the temperature every 5 seconds
            mqttTimer.detach(); // sets timer to publish mqtt data
            encoderTimer.detach();
        }
        if( mainState == MANUAL){
          clk_count = 0;
            presentTimer.attach(5.0, checkPresent); // checks if someone is present every 5 seconds
            tempTimer.attach(10.0, updateCurrTemp); // updates the temperature every 5 seconds
            mqttTimer.attach(10.0, publishTempState); // sets timer to publish mqtt data
            encoderTimer.attach(0.0001,readEncoder);
        }
        transitionLED(mainState);
      } 
    }
  }
  prev_press = buttonState;
}

void transitionLED(MainState nextState) {
  // switch statement to blink the led's during a transition
  switch (nextState) {
      case OFF:
        Serial.println("OFF");
        blink(strip.Color(255, 0, 0), 2, 200);
        break;
      case MANUAL:
        Serial.println("MANUAL");
        blink(strip.Color(10, 55, 255), 2, 200);
        break;
      case AUTOMATIC:
        Serial.println("AUTOMATIC");
        blink(strip.Color(85, 255, 9), 2, 200);
        break;
  }

}

void blink(uint32_t color, int numBlinks, int interval) {
  for (int i = 0; i < numBlinks; i++){
    fillStrip(color);
    delay(interval);
    fillStrip(strip.Color(0,0,0));
    delay(interval);
  }
}

void fillStrip(uint32_t color) {
  for (int i = 0; i < LED_COUNT; i++) {
    strip.setPixelColor(i, color);
  }
  strip.show();
}

void lightLEDs(uint32_t color){

  // maps the power input to the number of leds
  int ledsToLight = map(power, 0, 100, 0, LED_COUNT);
   // Serial.println("power, lights: ");
    //Serial.println(power);
    //Serial.println(ledsToLight);

  // turns off lights starting at the top
  for(int i = LED_COUNT - 1; i >= ledsToLight; i--){
    strip.setPixelColor(i, 0);
    //strip.show();
    //delay(1);
  }
  // turns on lights starting at bottom
  for(int i = 0; i < ledsToLight; i++){
    strip.setPixelColor(i, color);
    //delay(1);
  }
  delay(1);
  strip.show();
}

void updateDutyCycle() {
  // Simulate duty cycle with LED brightness
  if(power == 0) {
    analogWrite(FAN_PWM_PIN, 0);
    current_power = 0;
  } else {
    if (current_power == 0) {
       analogWrite(FAN_PWM_PIN, 255);
       delay(50);
    }
    analogWrite(FAN_PWM_PIN, map(power, 0, 100, 120, 255));
    current_power = power;
  }
  
}

void updateCameraMax() {

  camera.readPixels(pixels);

  int maxTemp = pixels[0];
  /*for(int i = 1; i < AMG_COLS * AMG_ROWS; i++) {
    if (pixels[i] > maxTemp) {
      maxTemp = pixels[i];
    }
  }*/
  //column 3 & 4, row 5 & 6 (others are getting affected by camera temperature + housing...)
  maxTemp = max(max(pixels[5*8 + 3], pixels[5*8 + 4]), max(pixels[6*8 + 3], pixels[6*8 + 4])); //only lets us do 2 per max
  camera_max = maxTemp;
}

void updateCurrTemp() {
    // updates the temp sensor
    tempsensor.wake();
    //delay(500); //may or may not be needed
    curr_temp = tempsensor.readTempC();
    tempsensor.shutdown();
    Serial.println("Updated temp reading: " + curr_temp);
    
}

void checkPresent() {

  Serial.println("Checked Presence");
  bool pir_present = digitalRead(PIR_PIN) == HIGH;

  if(pir_present){
    //camera_flag = true; // flag camera
    Serial.println("Motion Detected");
    cameraTimer.attach(2.0, updateCameraMax);
  } 

  if(!camera_flag) {
     //cameraTimer.detach();
  }

  //bool camera_present = (camera_max > curr_temp + 2);
  bool camera_present = (camera_max > 18);
  // if the pir detects presence (true) or if camera is flagged and detects presence
  present = pir_present || (camera_present && camera_flag);
  Serial.println(present);

  // Reset camera_flag if no presence is detected
  camera_flag = present ? camera_flag : false;
  
}

void publishTempState() {

  String output = " Current Temp: " + String(curr_temp) + "\n Camera Max: " +  String(camera_max) + " \n Present? " + String(present);
  client.publish(topic, output.c_str());
  Serial.println("MQTT: " + output);
  //add some state + power setting...
}

void configureMQTT() {
    //WIFI + MQTT Set Up
  WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.println("Connecting to WiFi..");
    }
    Serial.println("Connected to the Wi-Fi network");
    //connecting to a mqtt broker
    client.setServer(mqtt_broker, mqtt_port);
    client.setCallback(callback);
    while (!client.connected()) {
        String client_id = "esp32-client-";
        client_id += String(WiFi.macAddress());
        Serial.printf("The client %s connects to the public MQTT broker\n", client_id.c_str());
        if (client.connect(client_id.c_str(), mqtt_username, mqtt_password)) {
            Serial.println("Public EMQX MQTT broker connected");
        } else {
            Serial.print("failed with state ");
            Serial.print(client.state());
            delay(2000);
        }
    }
    // Publish and subscribe
    client.publish(topic, "Hi, I'm your ESP32 smart fan ^^");
    //client.subscribe(topic);
}

void callback(char *topic, byte *payload, unsigned int length) {

    
    Serial.print("Message arrived in topic: ");
    //Serial.println(topic);
    //Serial.print("Message:");
    String message;

    for (int i = 0; i < length; i++) {
        message += (char) payload[i];
    }

    int received_temp = message.toInt();
    if(received_temp > 0) {
      //attempt to read incoming message to update preference temperature
      pref_temp = received_temp;
      //Serial.println("Preferance Updated: " + String(received_temp));
    }

    //Serial.println();
    //Serial.println("-----------------------");
}
