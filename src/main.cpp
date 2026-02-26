/****************************************************
 * LoraDataViz - Morse Code Input System
 * Hardware:
 * - ESP-12E NodeMCU (ESP8266)
 * - 16x2 LCD (LiquidCrystal_I2C)
 * - 3-way channel select (Analog Voltage Divider on A0)
 * - Send button (D3)
 * - LED (D4) GPIO2
 * - Buzzer (D5) PGIO14
 ****************************************************/

#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <espnow.h>

/* =======================
   Pin Definitions
   ======================= */

/*
  PIN USAGE:
  A0 (ADC0)   CHANNEL SELECT (ANALOG INPUT)
  D1 (GPIO5)  LCD SCL
  D2 (GPIO4)  LCD SDA
  D3 (GPIO0)  SEND BUTTON (INPUT PULL_UP)
  D4 (GPIO2)  LED
  D5 (GPIO14) BUZZER (OUTPUT PWM)
*/

// Adjust these depending on your specific board mapping
#define PIN_CHANNEL_SELECT A0 // Analog Input
#define PIN_BUTTON_SEND 0     // D3 (GPIO 0)
#define PIN_LED 2             // D4 (GPIO 2)
#define PIN_BUZZER 14         // D5 (GPIO 14)

/* =======================
   Constants & Settings
   ======================= */
#define LCD_ADDRESS 0x27
#define LCD_COLUMNS 16
#define LCD_ROWS 2

// REPLACE WITH RECEIVER MAC Address
// universal mac address
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Timing thresholds
const unsigned long DOT_DURATION_MAX = 200;  // 0 - 0.2s is a dot
const unsigned long DASH_DURATION_MAX = 600; // 0.2 - 0.6s is a dash
const unsigned long CHAR_TIMEOUT = 700;      // 0.7s blank means end of char
const unsigned long BLANK_TIMEOUT = 5000;    // 5s blank means clear screen
const unsigned long DEBOUNCE = 30;           // 30ms debounce
const unsigned long ADC_POLL_TIME = 200;     // Throttle ADC to allow Wi-Fi to use the ADC
const unsigned long FEEDBACK_POLL_TIME = 5;  // Feedback tick every 5ms

/* =======================
   Objects
   ======================= */
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLUMNS, LCD_ROWS);

/* =======================
   Variables & States
   ======================= */
enum StateEnum
{
  SELECTING_CHANNEL,
  IDLE,
  RX,
  TX
};
StateEnum state = StateEnum::SELECTING_CHANNEL;

enum ChannelEnum
{
  CHANNEL_NONE,
  CHANNEL_1,
  CHANNEL_2
};
ChannelEnum currentChannel = ChannelEnum::CHANNEL_NONE;

enum FeedbackType
{
  TX_FB = 1000,
  RX_FB = 1500,
  CHARACTER = 3000,
  ERROR = 2000
};

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
  ChannelEnum channel = ChannelEnum::CHANNEL_NONE;
  bool clear_input = false;
  bool clear_text = false;
  bool sent_input = false;
  bool sent_text = false;
  char input_sequence[8];
  char text_sequence[32];
} struct_message;

// Create a struct_message called txData
struct_message txData;
struct_message rxData;

volatile bool newMsgReceived = false;
volatile bool isGivingFeedback = false;
volatile bool usingTimeoutFeedback = false;

unsigned long lastFeedbackCheckTime = 0;
unsigned long lastChannelCheckTime = 0;
unsigned long stopFeedbackTime = 0;

/* =======================
   Function Prototypes
   ======================= */
void initHardware();
ChannelEnum getChannelSelection();
bool detectChannelChange(ChannelEnum channel);
void handleChannelChange(ChannelEnum channel);
void handleMorseInput();
void processCharacter();
char decodeMorse(String sequence);
void displayText(String textSeq, bool isTx = true);
void clearText(bool isTx = true);
void displayInput(String inputSeq);
void clearInput();
void setFeedback(bool active, FeedbackType feedbackToneType = FeedbackType::TX_FB);
void timeoutFeedback(unsigned long duration, FeedbackType feedbackToneType = FeedbackType::TX_FB);
void displayError();
void OnDataSent(uint8_t *mac, uint8_t sendStatus);
void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len);

/* =======================
   Setup
   ======================= */
void setup()
{
  Serial.begin(115200);
  initHardware();

  Serial.println("<SYSTEM READY>");
  lcd.setCursor(2, 0);
  lcd.print("LoraDataViz");
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
  unsigned long currentMillis = millis();

  // 1. Throttle the ADC to allow Wi-Fi to use the ADC
  if (currentMillis - lastChannelCheckTime > ADC_POLL_TIME)
  {
    lastChannelCheckTime = currentMillis;
    currentChannel = getChannelSelection();
    if (detectChannelChange(currentChannel))
      handleChannelChange(currentChannel);
  }

  // shut off feedback
  if (currentMillis - lastFeedbackCheckTime > FEEDBACK_POLL_TIME)
  {
    if (usingTimeoutFeedback && currentMillis >= stopFeedbackTime)
    {
      Serial.println("Timeout feedback OFF");
      usingTimeoutFeedback = false;
      setFeedback(false);
    }
    lastFeedbackCheckTime = currentMillis;
  }

  // 2. Skip if still selecting channel
  if (state == StateEnum::SELECTING_CHANNEL)
  {
    delay(50);
    return;
  }

  // Safely check for incoming messages and update the UI
  if (newMsgReceived)
  {
    newMsgReceived = false; // Lower the flag immediately

    if (rxData.channel == currentChannel)
    {
      state = StateEnum::RX;

      Serial.print("Morse Sequence: ");
      Serial.print(rxData.input_sequence);
      Serial.print("\t");
      Serial.print("Letter: ");
      Serial.println(rxData.text_sequence);

      // Update the LCD
      if (rxData.clear_text) // just started new transmission
      {
        clearInput();
        clearText(false);
      }
      if (rxData.clear_input)
      {
        clearInput();
      }
      displayInput(rxData.input_sequence);
      displayText(rxData.text_sequence, false);

      if (rxData.sent_input)
      {
        // get last character of rxData.input_sequence
        char lastChar = rxData.input_sequence[strlen(rxData.input_sequence) - 1];
        if (lastChar == '.')
        {
          timeoutFeedback(100, FeedbackType::RX_FB);
        }
        else if (lastChar == '-')
        {
          timeoutFeedback(300, FeedbackType::RX_FB);
        }
      }
      else if (rxData.sent_text)
      {
        timeoutFeedback(100, FeedbackType::CHARACTER);
      }
    }
    else
    {
      Serial.print("Message from other channel. Channel: ");
      Serial.println(rxData.channel);
    }
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
  lcd.clear();

  // Buttons
  pinMode(PIN_BUTTON_SEND, INPUT_PULLUP);

  // Buzzer & LED
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_LED, LOW);

  // ESP-NOW
  // Set device as a Wi-Fi Station
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(); // IMPORTANT: Disconnects from saved networks so the channel doesn't drift
  delay(100);

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
  // TODO: channel
  esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_COMBO, CHANNEL_1, NULL, 0);
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
bool detectChannelChange(ChannelEnum channel)
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
  txData.channel = channel;
  switch (channel)
  {
  case ChannelEnum::CHANNEL_1:
  case ChannelEnum::CHANNEL_2:
    state = StateEnum::IDLE;

    lcd.clear();
    lcd.setCursor(6, 1);
    lcd.print("CH");
    lcd.print(channel);

    // Serial.print("Selected Channel: ");
    // Serial.println(channel);

    delay(500);

    lcd.clear();
    displayText("");

    break;

  case ChannelEnum::CHANNEL_NONE:
    state = StateEnum::SELECTING_CHANNEL;

    lcd.clear();
    lcd.setCursor(0, 1);
    lcd.print("<SELECT CHANNEL>");

    // Serial.println("Selected Channel: None");

    break;
  }
}

unsigned long pressStartTime = 0;
unsigned long releaseStartTime = 0;
volatile bool buttonWasPressed = false;
void handleMorseInput()
{
  unsigned long now = millis();
  bool isPressed = (digitalRead(PIN_BUTTON_SEND) == LOW); // LOW because INPUT_PULLUP

  // 1. Button JUST Pressed
  if (isPressed && !buttonWasPressed)
  {
    if (inputSequence.length() < 16)
    {
      pressStartTime = now;
      buttonWasPressed = true;
      Serial.println("Button pressed feedback ON");
      setFeedback(true);

      if (state == StateEnum::RX)
      {
        clearInput();
        clearText();
      }
      state = StateEnum::TX;
    }
  }

  // 2. Button JUST Released
  else if (!isPressed && buttonWasPressed)
  {
    if (inputSequence.length() < 16)
    {
      unsigned long duration = now - pressStartTime;
      Serial.println("Button released feedback OFF");
      setFeedback(false);
      buttonWasPressed = false;
      releaseStartTime = now;

      if (inputSequence.length() == 0)
      {
        txData.clear_input = true;
      }
      else
      {
        txData.clear_input = false;
      }

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

      // Set values to send
      // Safely convert the String to a char array for transmission
      strncpy(txData.input_sequence, inputSequence.c_str(), sizeof(txData.input_sequence) - 1);
      txData.input_sequence[sizeof(txData.input_sequence) - 1] = '\0'; // Force null termination for safety
      txData.sent_input = true;
      // Send message via ESP-NOW
      esp_now_send(broadcastAddress, (uint8_t *)&txData, sizeof(txData));
      txData.sent_input = false;
    }
  }

  // 3. Waiting (Gap Check)
  else if (!isPressed)
  {
    // "A blank of CHAR_TIMEOUT or above?"
    unsigned long duration = now - releaseStartTime;
    if (duration >= CHAR_TIMEOUT && inputSequence.length() > 0)
    {
      processCharacter();
    }
    else if (duration >= BLANK_TIMEOUT && textSequence.length() > 0 && state != StateEnum::RX)
    {
      state = StateEnum::IDLE;
      clearText();
      clearInput();
      strncpy(txData.input_sequence, "", sizeof(txData.input_sequence) - 1);
      strncpy(txData.text_sequence, "", sizeof(txData.text_sequence) - 1);
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
    if (textSequence.length() == 0)
    {
      txData.clear_text = true;
    }
    else
    {
      txData.clear_text = false;
    }

    textSequence += letter;
    displayText(textSequence);

    Serial.print("MORSE:");
    Serial.print(inputSequence);
    Serial.print("\t");
    Serial.print("LETTER:");
    Serial.print(letter);
    Serial.print("\t");
    Serial.print("SENT:");
    Serial.println(textSequence);

    timeoutFeedback(100, FeedbackType::CHARACTER);

    // Set values to send
    // Safely convert the String to a char array for transmission
    strncpy(txData.text_sequence, textSequence.c_str(), sizeof(txData.text_sequence) - 1);
    txData.text_sequence[sizeof(txData.text_sequence) - 1] = '\0'; // Force null termination for safety

    // Send message via ESP-NOW
    txData.sent_text = true;
    esp_now_send(broadcastAddress, (uint8_t *)&txData, sizeof(txData));
    txData.sent_text = false;
  }
  else
    displayError();

  // Reset for next input
  clearInput();
  releaseStartTime = millis(); // Reset timer
}

void displayText(String textSeq, bool isTx)
{
  lcd.setCursor(0, 0);
  if (isTx)
    lcd.print("TX:");
  else
    lcd.print("RX:");
  lcd.print(textSeq);
}

void clearText(bool isTx)
{
  textSequence = "";
  displayText("             ", isTx);
  // Serial.println("Cleared text");
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

void setFeedback(bool active, FeedbackType feedbackToneType)
{
  // "Set the buzzer and led light simultaneously"
  if (active)
  {
    // Serial.println("Feedback ON");
    isGivingFeedback = true;
    digitalWrite(PIN_LED, HIGH);        // LED ON
    tone(PIN_BUZZER, feedbackToneType); // Buzzer Tone
  }
  else
  {
    // Serial.println("Feedback OFF");
    isGivingFeedback = false;
    digitalWrite(PIN_LED, LOW); // LED OFF
    noTone(PIN_BUZZER);         // Buzzer OFF
  }
}

void timeoutFeedback(unsigned long duration, FeedbackType feedbackToneType)
{
  usingTimeoutFeedback = true;
  stopFeedbackTime = millis() + duration;
  Serial.println("Timeout Feedback ON");
  setFeedback(true, feedbackToneType);
}

// Callback when data is sent
void OnDataSent(uint8_t *mac, uint8_t sendStatus)
{
  if (sendStatus != 0)
  {
    Serial.println("Packet Delivery fail");
  }
}

// Callback function that will be executed when data is received
void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len)
{
  // Safety check: Only accept data if it perfectly matches our struct size
  if (len == sizeof(rxData))
  {
    memcpy(&rxData, incomingData, sizeof(rxData));
    newMsgReceived = true; // Raise the flag!
  }
  else
  {
    Serial.println("RX Error: Payload size mismatch!");
  }
}