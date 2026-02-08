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

// Timing thresholds from flowchart
const unsigned long DOT_DURATION_MAX = 200;  // 0 - 0.2s is a dot
const unsigned long DASH_DURATION_MAX = 600; // 0.2 - 0.6s is a dash
const unsigned long CHAR_TIMEOUT = 700;      // 0.7s blank means end of char
const unsigned long DEBOUNCE = 10;           // 10ms debounce

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
unsigned long pressStartTime = 0;
unsigned long releaseStartTime = 0;
bool buttonWasPressed = false;

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
void feedback(bool state);
void displayError();

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
  ChannelEnum channel = getChannelSelection();
  bool channelChanged = detectButtonChange(channel);
  if (channelChanged)
    handleChannelChange(channel);

  // Skip if still selecting channel
  if (currentState == StateEnum::STATE_SELECT_CHANNEL)
  {
    delay(100);
    return;
  }

  // sanity check
  if (channel == ChannelEnum::CHANNEL_NONE)
  {
    Serial.println("CHANNEL UNSELECTED WTF");
    return;
  }

  // Handle input
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

  // TODO: LORA INIT
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
    lcd.setCursor(0, 1);
    lcd.print(inputSequence);
  }

  // 3. Waiting (Gap Check)
  else if (
      !isPressed && inputSequence.length() > 0)
  {
    // "A blank of CHAR_TIMEOUT or above?"
    if (millis() - releaseStartTime >= CHAR_TIMEOUT)

      processCharacter();
  }
}

void processCharacter()
{
  // "The corresponding alphabet exist?"
  char letter = decodeMorse(inputSequence);

  lcd.clear();
  lcd.setCursor(0, 0);

  if (letter != '?')
  {
    // YES: "Transform... Send information"
    lcd.print("Sent: ");
    lcd.print(letter);
    Serial.print("SENT_CHAR:");
    Serial.println(letter); // Send to "other display"

    // TODO: SEND TO LORA
  }
  else
  {
    // NO: "Display Error for 3 seconds"
    displayError();
  }

  // Reset for next input
  inputSequence = "";
  lcd.clear();
  lcd.print("Input: ");
  releaseStartTime = millis(); // Reset timer
}

void displayError()
{
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Error");
  // delay(3000);
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