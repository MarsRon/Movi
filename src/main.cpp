/****************************************************
 * LoraDataViz - Morse Code Input System
 * Hardware:
 * - ESP-12E NodeMCU (ESP8266)
 * - 16x2 LCD (LiquidCrystal_I2C)
 * - 3-way channel select (Analog Voltage Divider on A0)
 * - Send button (D3)
 * - Buzzer & LED (D4)
 ****************************************************/

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <espnow.h>

/* =======================
   Pin Definitions
   ======================= */

/*
  PIN USAGE:
  A0 (ADC0) CHANNEL SELECT (ANALOG INPUT)
  D1 (5) LCD SCL
  D2 (4) LCD SDA
  D3 (0) SEND BUTTON (INPUT PULL_UP)
  D4 (2) BUZZER + LED (OUTPUT PWM)
  D5 (14) LORA SCLK
  D6 (12) LORA MISO
  D7 (13) LORA MOSI
  D8 (15) LORA CS
*/

// Adjust these depending on your specific board mapping
#define PIN_CHANNEL_SELECT A0 // Analog Input
#define PIN_BUTTON_SEND 0     // D3 (GPIO 0 on NodeMCU)
#define PIN_BUZZER_LED 2      // D4 (GPIO 2 on NodeMCU)

/* =======================
   Constants & Settings
   ======================= */
#define LCD_ADDRESS 0x27
#define LCD_COLUMNS 16
#define LCD_ROWS 2

// REPLACE WITH RECEIVER MAC Address
// universal mac address
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Timing thresholds from flowchart
const unsigned long DOT_DURATION_MAX = 200;  // 0 - 0.2s is a dot
const unsigned long DASH_DURATION_MAX = 600; // 0.2 - 0.6s is a dash
const unsigned long CHAR_TIMEOUT = 700;      // 0.7s blank means end of char
const unsigned long BLANK_TIMEOUT = 10000;   // 10s blank means clear screen
const unsigned long DEBOUNCE = 30;           // 30ms debounce

/* =======================
   Objects
   ======================= */
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLUMNS, LCD_ROWS);

/* =======================
   Variables & States
   ======================= */
enum StateEnum
{
  STATE_SELECT_CHANNEL,
  STATE_INPUT_WAIT,
  STATE_INPUT_PRESS
};

enum ChannelEnum
{
  CHANNEL_NONE,
  CHANNEL_1,
  CHANNEL_2
};

StateEnum currentState = StateEnum::STATE_SELECT_CHANNEL;

String inputSequence = ""; // Stores ".-" sequence
String textSequence = "";  // Stores recorded text sequence

// Morse Code Lookup Table (Simple version)
const struct
{
  const char *code;
  char letter;
} morseTable[] = {
    {".-", 'A'},
    {"-...", 'B'},
    {"-.-.", 'C'},
    {"-..", 'D'},
    {".", 'E'},
    {"..-.", 'F'},
    {"--.", 'G'},
    {"....", 'H'},
    {"..", 'I'},
    {".---", 'J'},
    {"-.-", 'K'},
    {".-..", 'L'},
    {"--", 'M'},
    {"-.", 'N'},
    {"---", 'O'},
    {".--.", 'P'},
    {"--.-", 'Q'},
    {".-.", 'R'},
    {"...", 'S'},
    {"-", 'T'},
    {"..-", 'U'},
    {"...-", 'V'},
    {".--", 'W'},
    {"-..-", 'X'},
    {"-.--", 'Y'},
    {"--..", 'Z'},
    // Add numbers if needed
};

// Structure example to send data
// Must match the receiver structure
typedef struct struct_message
{
  char morse_sequence[8];
  char text_sequence[32];
  int test;
} struct_message;

// Create a struct_message called txData
struct_message txData;
struct_message rxData;

unsigned long lastChannelCheck = 0;
ChannelEnum currentChannel = ChannelEnum::CHANNEL_NONE;

/* =======================
   Function Prototypes
   ======================= */
void initHardware();
ChannelEnum getChannelSelection();
bool detectButtonChange(ChannelEnum channel);
void handleChannelChange(ChannelEnum channel);
void handleMorseInput();
void processCharacter();
char decodeMorse(String sequence);
void displayText(String textSeq);
void clearText();
void displayInput(String inputSeq);
void clearInput();
void feedback(bool state);
void displayError();
// Callback when data is sent
void OnDataSent(uint8_t *mac_addr, uint8_t sendStatus);
// Callback function that will be executed when data is received
void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len);

/* =======================
   Setup
   ======================= */
void setup()
{
  Serial.begin(115200);
  initHardware();

  Serial.println("<SYSTEM READY>");
  lcd.setCursor(1, 1);
  lcd.print("<SYSTEM READY>");
  delay(1000);

  ChannelEnum channel = getChannelSelection();
  handleChannelChange(channel);
}

/* =======================
   Main Loop
   ======================= */
void loop()
{
  // 1. Throttle the ADC read to once every 200ms to allow Wi-Fi to use the ADC
  if (millis() - lastChannelCheck > 200)
  {
    currentChannel = getChannelSelection();
    bool channelChanged = detectButtonChange(currentChannel);

    if (channelChanged)
    {
      handleChannelChange(currentChannel);
    }

    lastChannelCheck = millis();
  }

  // 2. Skip if still selecting channel
  if (currentState == StateEnum::STATE_SELECT_CHANNEL)
  {
    delay(50); // Small delay to yield to background tasks
    return;
  }

  // 3. Sanity check
  if (currentChannel == ChannelEnum::CHANNEL_NONE)
  {
    Serial.println("CHANNEL UNSELECTED WTF");
    delay(10);
    return;
  }

  // 4. Handle input
  handleMorseInput();
}

/* =======================
   Functions
   ======================= */

void initHardware()
{
  // LCD
  Wire.begin();
  lcd.init();
  lcd.backlight();

  // Buttons
  pinMode(PIN_BUTTON_SEND, INPUT_PULLUP);

  // Buzzer & LED
  pinMode(PIN_BUZZER_LED, OUTPUT);
  digitalWrite(PIN_BUZZER_LED, LOW);

  // ESP NOW INIT
  // Set device as a Wi-Fi Station
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(); // IMPORTANT: Disconnects from saved networks so the channel doesn't drift
  delay(100);

  // Init ESP-NOW
  if (esp_now_init() != 0)
  {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // Once ESPNow is successfully Init, we will register for Send CB to
  // get the status of Trasnmitted packet
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  // Register peer
  esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
}

ChannelEnum getChannelSelection()
{
  int sensorValue = analogRead(PIN_CHANNEL_SELECT);
  ChannelEnum channel;

  // Logic assumes a voltage divider switch:
  // ~0-50    = None
  // ~300-350 = Ch2
  // ~600-650 = Ch1
  // But we'll give leeway
  // 0~100 = None
  // 100~400 = Ch2
  // 400~700 = Ch1
  if (sensorValue > 100 && sensorValue <= 400)
    channel = ChannelEnum::CHANNEL_2;
  else if (sensorValue > 400 && sensorValue <= 700)
    channel = ChannelEnum::CHANNEL_1;
  else
    channel = ChannelEnum::CHANNEL_NONE;

  return channel;
}

ChannelEnum previousChannel = ChannelEnum::CHANNEL_NONE;
bool detectButtonChange(ChannelEnum channel)
{
  if (channel != previousChannel)
  {
    previousChannel = channel;
    return true;
  }
  return false;
}

void handleChannelChange(ChannelEnum channel)
{
  switch (channel)
  {
  case ChannelEnum::CHANNEL_1:
  case ChannelEnum::CHANNEL_2:
    Serial.print("Selected Channel: ");
    Serial.println(channel);
    lcd.clear();
    lcd.setCursor(6, 1);
    lcd.print("CH");
    lcd.print(channel);
    delay(500);
    lcd.clear();
    displayText("");
    currentState = StateEnum::STATE_INPUT_WAIT;
    break;
  case ChannelEnum::CHANNEL_NONE:
    Serial.println("Selected Channel: None");
    lcd.clear();
    lcd.setCursor(0, 1);
    lcd.print("<SELECT CHANNEL>");
    currentState = StateEnum::STATE_SELECT_CHANNEL;
    break;
  }
}

unsigned long pressStartTime = 0;
unsigned long releaseStartTime = 0;
bool buttonWasPressed = false;
void handleMorseInput()
{
  bool isPressed = (digitalRead(PIN_BUTTON_SEND) == LOW); // LOW because INPUT_PULLUP

  // 1. Button JUST Pressed
  if (isPressed && !buttonWasPressed)
  {
    pressStartTime = millis();
    feedback(true); // LED/Buzzer ON
    buttonWasPressed = true;
    currentState = StateEnum::STATE_INPUT_PRESS;
  }

  // 2. Button JUST Released
  else if (!isPressed && buttonWasPressed)
  {
    unsigned long duration = millis() - pressStartTime;
    feedback(false); // LED/Buzzer OFF
    buttonWasPressed = false;
    releaseStartTime = millis();
    currentState = StateEnum::STATE_INPUT_WAIT;

    // Logic: 0 - 0.3s is Dot, >0.3s is Dash
    if (duration > DEBOUNCE && duration <= DOT_DURATION_MAX)
    {
      inputSequence += ".";
    }
    else if (duration > DOT_DURATION_MAX && duration <= DASH_DURATION_MAX)
    {
      inputSequence += "-";
    }

    // Update screen "Display the dot and dash on screen"
    displayInput(inputSequence);
  }

  // 3. Waiting (Gap Check)
  else if (!isPressed)
  {
    // "A blank of CHAR_TIMEOUT or above?"
    unsigned long duration = millis() - releaseStartTime;
    if (duration >= CHAR_TIMEOUT && inputSequence.length() > 0)
    {
      processCharacter();
    }
    else if (duration >= BLANK_TIMEOUT && textSequence.length() > 0)
    {
      clearText();
      clearInput();
    }
  }
}

void processCharacter()
{
  // "The corresponding alphabet exist?"
  char letter = decodeMorse(inputSequence);

  if (letter != '?')
  {
    // YES: "Transform... Send information"
    textSequence += letter;
    displayText(textSequence);

    Serial.print("SENT MORSE:");
    Serial.print(inputSequence);
    Serial.print("\t");
    Serial.print("SENT TEXT:");
    Serial.println(textSequence);

    // Set values to send
    // Safely convert the String to a char array for transmission
    strncpy(txData.morse_sequence, inputSequence.c_str(), sizeof(txData.morse_sequence) - 1);
    txData.morse_sequence[sizeof(txData.morse_sequence) - 1] = '\0'; // Force null termination for safety
    strncpy(txData.text_sequence, textSequence.c_str(), sizeof(txData.text_sequence) - 1);
    txData.text_sequence[sizeof(txData.text_sequence) - 1] = '\0'; // Force null termination for safety

    // Send message via ESP-NOW
    esp_now_send(broadcastAddress, (uint8_t *)&txData, sizeof(txData));
  }
  else
    displayError();

  // Reset for next input
  clearInput();
  releaseStartTime = millis(); // Reset timer
}

void displayText(String textSeq)
{
  lcd.setCursor(0, 0);
  lcd.print("TX:");
  lcd.print(textSeq);
}

void clearText()
{
  textSequence = "";
  displayText("             ");
  Serial.println("Cleared text");
}

void displayInput(String inputSeq)
{
  lcd.setCursor(0, 1);
  lcd.print(inputSeq);
}

void clearInput()
{
  inputSequence = "";
  displayInput("                ");
}

void displayError()
{
  displayInput("<Error>");
  delay(300);
  clearInput();
}

char decodeMorse(String sequence)
{
  for (unsigned int i = 0; i < sizeof(morseTable) / sizeof(morseTable[0]); i++)
  {
    if (sequence == morseTable[i].code)
    {
      return morseTable[i].letter;
    }
  }
  return '?'; // Not found
}

void feedback(bool state)
{
  // "Set the buzzer and led light simultaneously"
  if (state)
  {
    // digitalWrite(PIN_BUZZER_LED, HIGH); // LED ON
    tone(PIN_BUZZER_LED, 1000); // Buzzer Tone (if supported on pin)
  }
  else
  {
    // digitalWrite(PIN_BUZZER_LED, LOW); // LED OFF
    noTone(PIN_BUZZER_LED); // Buzzer OFF
  }
}

// Callback when data is sent
void OnDataSent(uint8_t *mac_addr, uint8_t sendStatus)
{
  Serial.print("Last Packet Send Status: ");
  if (sendStatus == 0)
  {
    Serial.println("Delivery success");
  }
  else
  {
    Serial.println("Delivery fail");
  }
}

// Callback function that will be executed when data is received
void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len)
{
  // Safety check: Only accept data if it perfectly matches our struct size
  if (len == sizeof(rxData))
  {
    memcpy(&rxData, incomingData, sizeof(rxData));

    Serial.print("Bytes received: ");
    Serial.println(len);

    Serial.print("Morse sequence: ");
    Serial.println(rxData.morse_sequence);

    Serial.print("Text sequence: ");
    Serial.println(rxData.text_sequence);
    Serial.println("-----------------");
  }
  else
  {
    Serial.println("RX Error: Payload size mismatch!");
  }
}