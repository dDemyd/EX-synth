#include <Arduino.h>
#include <Wire.h>
#include <I2S.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

// --- КОНФІГУРАЦІЯ ЕКРАНА ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire1, -1);

// --- ПІНИ ---
const int touchPins[4] = {26, 27, 28, 29};
const int buttonPins[8] = {6, 7, 8, 9, 10, 11, 12, 13};
const int ledPins[8] = {16, 17, 18, 19, 20, 21, 22, 23};
const int sensorPins[4] = {3, 4, 5, 22};
const int syncInPin = 20;

// --- АУДІО ТА АНАЛОГОВИЙ СИНТЕЗ ---
I2S i2sOutput(OUTPUT);
const int sampleRate = 44100;
const float noteFreqs[8] = {261.63, 293.66, 329.63, 349.23, 392.00, 440.00, 493.88, 523.25};
// Оптимізовані дволітерні назви
const char *noteNames[8] = {"DO", "RE", "MI", "FA", "SO", "LA", "SI", "NO"};

volatile float phaseA = 0;
volatile float phaseB = 0;
volatile float phaseSub = 0;
volatile float phaseLFO = 0;
volatile int currentOctave = 4;
volatile int potValues[4] = {50, 1, 30, 10};
volatile float currentFreq = 0;
volatile float targetFreq = 0;
volatile int noteToPlay = -1;
volatile bool noteTriggered = false;

// --- СИСТЕМА ЦИФРОВИХ ПАРАМЕТРІВ ---
const int NUM_PARAMS = 11;
int currentParam = 0;
const char *paramNames[NUM_PARAMS] = {
    "WAVE", "TONE", "GLIDE", "DETUNE",
    "SWING", "SUB-OSC", "ATTACK", "DECAY",
    "LFO RATE", "LFO DEPTH", "CLOCK DIV"};
int paramValues[NUM_PARAMS] = {1, 50, 30, 10, 0, 0, 0, 30, 40, 0, 1};

// --- СТАН И ТАЙМЕРИ ІНТЕРФЕЙСУ ---
bool isPopUpActive = false;
unsigned long popUpTimer = 0;
const unsigned long POPUP_DURATION = 2000;

unsigned long plusHoldTimer = 0;
unsigned long minusHoldTimer = 0;
const int HOLD_DELAY = 300;
const int REPEAT_INTERVAL = 50;
unsigned long lastRepeatTime = 0;

// Нова система режимів для Кнопки 4 (0: Off, 1: 2nd, 2: 3rd, 3: Rnd)
int muteMode = 0;
bool rollerActive = false;
unsigned long fxTouchTime = 0;
bool fxWasPressed = false;

// --- СЕКВЕНСОР ---
int sequence[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
int seqStep = 0;
int seqLength = 0;
bool isPlaying = false;
bool isRecording = false;
unsigned long lastStepTime = 0;
const int fixedBpm = 120;
unsigned long stepDurationMs = (60000 / fixedBpm) / 2;

bool lastRecSensorState = LOW;
String flashMessage = "";
unsigned long flashMessageTime = 0;

// --- ОБРОБКА СЕНСОРІВ МЕНЮ ТА КНОПОК ---
void updateTouchButtons()
{
  unsigned long now = millis();

  // 1. КНОПКА SELECT
  static bool lastSelectState = LOW;
  bool currentSelectState = (digitalRead(touchPins[0]) == HIGH);
  if (currentSelectState && !lastSelectState)
  {
    if (!isPopUpActive)
      isPopUpActive = true;
    else
      currentParam = (currentParam + 1) % NUM_PARAMS;
    popUpTimer = now;
    delay(50);
  }
  lastSelectState = currentSelectState;

  // 2. КНОПКА PLUS (+)
  if (digitalRead(touchPins[1]) == HIGH)
  {
    if (plusHoldTimer == 0)
    {
      plusHoldTimer = now;
      int step = (currentParam == 0 || currentParam == 10) ? 1 : 5;
      paramValues[currentParam] += step;
      if (currentParam == 0 && paramValues[0] > 3)
        paramValues[0] = 0;
      if (currentParam == 10 && paramValues[10] > 8)
        paramValues[10] = 1;
      if (paramValues[currentParam] > 99 && currentParam != 0 && currentParam != 10)
        paramValues[currentParam] = 99;
      if (!isPopUpActive)
        isPopUpActive = true;
      popUpTimer = now;
    }
    else if (now - plusHoldTimer > HOLD_DELAY)
    {
      if (now - lastRepeatTime > REPEAT_INTERVAL)
      {
        lastRepeatTime = now;
        int step = (currentParam == 0 || currentParam == 10) ? 1 : 5;
        paramValues[currentParam] += step;
        if (currentParam == 0 && paramValues[0] > 3)
          paramValues[0] = 0;
        if (currentParam == 10 && paramValues[10] > 8)
          paramValues[10] = 1;
        if (paramValues[currentParam] > 99 && currentParam != 0 && currentParam != 10)
          paramValues[currentParam] = 99;
        popUpTimer = now;
      }
    }
  }
  else
  {
    plusHoldTimer = 0;
  }

  // 3. КНОПКА MINUS (-)
  if (digitalRead(touchPins[2]) == HIGH)
  {
    if (minusHoldTimer == 0)
    {
      minusHoldTimer = now;
      int step = (currentParam == 0 || currentParam == 10) ? 1 : 5;
      paramValues[currentParam] -= step;
      if (currentParam == 0 && paramValues[0] < 0)
        paramValues[0] = 3;
      if (currentParam == 10 && paramValues[10] < 1)
        paramValues[10] = 8;
      if (paramValues[currentParam] < 0 && currentParam != 0 && currentParam != 10)
        paramValues[currentParam] = 0;
      if (!isPopUpActive)
        isPopUpActive = true;
      popUpTimer = now;
    }
    else if (now - minusHoldTimer > HOLD_DELAY)
    {
      if (now - lastRepeatTime > REPEAT_INTERVAL)
      {
        lastRepeatTime = now;
        int step = (currentParam == 0 || currentParam == 10) ? 1 : 5;
        paramValues[currentParam] -= step;
        if (currentParam == 0 && paramValues[0] < 0)
          paramValues[0] = 3;
        if (currentParam == 10 && paramValues[10] < 1)
          paramValues[10] = 8;
        if (paramValues[currentParam] < 0 && currentParam != 0 && currentParam != 10)
          paramValues[currentParam] = 0;
        popUpTimer = now;
      }
    }
  }
  else
  {
    minusHoldTimer = 0;
  }

  // 4. КНОПКА FX (Оновлена циклічна логіка)
  bool fxState = (digitalRead(touchPins[3]) == HIGH);
  if (fxState && !fxWasPressed)
  {
    fxTouchTime = now;
    fxWasPressed = true;
  }
  if (fxWasPressed)
  {
    if (fxState)
    {
      if (now - fxTouchTime > 300)
        rollerActive = true;
    }
    else
    {
      fxWasPressed = false;
      if (rollerActive)
      {
        rollerActive = false;
      }
      else
      {
        muteMode = (muteMode + 1) % 4; // Перемикаємо: Off -> 2nd -> 3rd -> Rnd
      }
    }
  }

  potValues[0] = paramValues[1];
  potValues[2] = paramValues[2];
  potValues[3] = paramValues[3];

  if (paramValues[0] == 0)
    potValues[1] = 0;
  if (paramValues[0] == 1)
    potValues[1] = 50;
  if (paramValues[0] == 2)
    potValues[1] = 99;
  if (paramValues[0] == 3)
    potValues[1] = 3;
}

// --- EEPROM ---
void saveSequenceToEEPROM(int slot)
{
  int startAddr = slot * 20;
  EEPROM.write(startAddr, seqLength);
  for (int i = 0; i < 16; i++)
    EEPROM.write(startAddr + 1 + i, sequence[i]);
  EEPROM.commit();
  flashMessage = "SAVED TO\nSLOT " + String(slot);
  flashMessageTime = millis();
}

void loadSequenceFromEEPROM(int slot)
{
  int startAddr = slot * 20;
  seqLength = EEPROM.read(startAddr);
  if (seqLength > 16 || seqLength < 0)
    seqLength = 0;
  for (int i = 0; i < 16; i++)
  {
    int val = EEPROM.read(startAddr + 1 + i);
    sequence[i] = (val == 255 || val > 7) ? -1 : val;
  }
  flashMessage = "LOADED\nSLOT " + String(slot);
  flashMessageTime = millis();
}

// =========================================================================
// 🧠 ЯДРО 0: ІНТЕРФЕЙС З НОВОЮ ВЕРСТКОЮ ТА СЕКВЕНСОРОМ
// =========================================================================

void drawInterface()
{
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (flashMessage != "" && (millis() - flashMessageTime < 2000))
  {
    display.setTextSize(2);
    display.setCursor(10, 15);
    if (flashMessage.indexOf('\n') != -1)
    {
      display.print(flashMessage.substring(0, flashMessage.indexOf('\n')));
      display.setCursor(10, 38);
      display.print(flashMessage.substring(flashMessage.indexOf('\n') + 1));
    }
    else
    {
      display.print(flashMessage);
    }
    display.display();
    return;
  }
  else if (flashMessage != "")
  {
    flashMessage = "";
  }

  if (isRecording)
  {
    display.setTextSize(1);
    display.setCursor(22, 2);
    display.print("RECORDING MODE");

    if (seqLength <= 8)
    {
      int boxW = 11, boxH = 16, startX = 12, startY = 24, gap = 3;
      for (int i = 0; i < 8; i++)
      {
        int x = startX + i * (boxW + gap);
        if (i < seqLength)
        {
          display.fillRect(x, startY, boxW, boxH, SSD1306_WHITE);
          display.setTextColor(SSD1306_BLACK);
          display.setCursor(x + 3, startY + 4);
          display.print(sequence[i] == 7 ? "NO" : String(sequence[i] + 1));
        }
        else
        {
          display.drawRect(x, startY, boxW, boxH, SSD1306_WHITE);
        }
      }
    }
    else
    {
      int boxW = 9, boxH = 11, startX = 18, gap = 4;
      for (int i = 0; i < 16; i++)
      {
        int row = i / 8;
        int col = i % 8;
        int x = startX + col * (boxW + gap);
        int y = 20 + row * (boxH + gap);
        if (i < seqLength)
        {
          display.fillRect(x, y, boxW, boxH, SSD1306_WHITE);
          display.setTextColor(SSD1306_BLACK);
          display.setCursor(x + 2, y + 2);
          display.print(sequence[i] == 7 ? "N" : String(sequence[i] + 1));
        }
        else
        {
          display.drawRect(x, y, boxW, boxH, SSD1306_WHITE);
        }
      }
    }
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(14, 52);
    display.print("Steps: ");
    display.print(seqLength);
    display.print(" / 16");
    display.display();
    return;
  }

  if (isPopUpActive)
  {
    int16_t x1, y1;
    uint16_t w, h;
    display.setTextSize(2);
    display.getTextBounds(paramNames[currentParam], 0, 0, &x1, &y1, &w, &h);
    int nameX = (SCREEN_WIDTH - w) / 2;
    display.setCursor(nameX, 2);
    display.print(paramNames[currentParam]);

    if (currentParam == 0)
    {
      const char *waveNames[4] = {"SINE", "TRIANGLE", "SAWTOOTH", "SQUARE"};
      display.setTextSize(1);
      display.getTextBounds(waveNames[paramValues[0]], 0, 0, &x1, &y1, &w, &h);
      int waveNameX = (SCREEN_WIDTH - w) / 2;
      display.setCursor(waveNameX, 24);
      display.print(waveNames[paramValues[0]]);

      int wX = 52;
      int wY = 46;
      if (paramValues[0] == 0)
        display.drawCircle(wX + 10, wY, 8, SSD1306_WHITE);
      if (paramValues[0] == 1)
        display.drawTriangle(wX, wY + 8, wX + 10, wY - 8, wX + 20, wY + 8, SSD1306_WHITE);
      if (paramValues[0] == 2)
      {
        display.drawLine(wX, wY + 8, wX + 14, wY - 8, SSD1306_WHITE);
        display.drawLine(wX + 14, wY - 8, wX + 14, wY + 8, SSD1306_WHITE);
      }
      if (paramValues[0] == 3)
        display.drawRect(wX, wY - 6, 20, 12, SSD1306_WHITE);
    }
    else
    {
      display.setTextSize(2);
      String valStr = String(paramValues[currentParam]);
      display.getTextBounds(valStr, 0, 0, &x1, &y1, &w, &h);
      int valX = (SCREEN_WIDTH - w) / 2;
      display.setCursor(valX, 22);
      display.print(valStr);

      int barX = 12;
      int barY = 44;
      int barW = 104;
      int barH = 8;
      display.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
      int fillWidth = 0;
      if (currentParam == 10)
        fillWidth = map(paramValues[currentParam], 1, 8, 2, barW - 4);
      else
        fillWidth = map(paramValues[currentParam], 0, 99, 0, barW - 4);
      display.fillRect(barX + 2, barY + 2, fillWidth, barH - 4, SSD1306_WHITE);
    }
    display.display();
    return;
  }

  // --- ГОЛОВНИЙ ЕКРАН (LIVE MODE) ---
  int tickW = 10, tickH = 2, gap = 4;
  int startTicksX = (SCREEN_WIDTH - (8 * tickW + 7 * gap)) / 2;
  for (int i = 0; i < 8; i++)
  {
    if (!isPlaying || (seqStep % 8 != i))
    {
      display.fillRect(startTicksX + i * (tickW + gap), 2, tickW, tickH, SSD1306_WHITE);
    }
  }

  // Назва ноти (2 букви, розмір 3)
  display.setTextSize(3);
  display.setCursor(4, 18);
  if (noteToPlay != -1)
    display.print(noteNames[noteToPlay]);
  else
    display.print("--");

  // ВЕРСТКА ІНДИКАТОРА MUTE (у вивільненому просторі справа від ноти)
  if (muteMode > 0)
  {
    display.setTextSize(1);
    display.setCursor(46, 18);
    display.print("MUTE");
    display.setCursor(46, 28);
    if (muteMode == 1)
      display.print("2ND");
    if (muteMode == 2)
      display.print("3RD");
    if (muteMode == 3)
      display.print("RND");
  }

  // Блок октави
  display.setTextSize(1);
  display.setCursor(76, 16);
  display.print("Octave");
  display.setTextSize(2);
  display.setCursor(84, 28);
  display.print(currentOctave);

  // ЗБІЛЬШЕНА ІКОНКА ФОРМИ ХВИЛІ (Розмір піднято до 14х14 пікселів)
  int iX = 110;
  int iY = 24;
  int iSize = 14;
  if (paramValues[0] == 0)
    display.drawCircle(iX + iSize / 2, iY + iSize / 2, iSize / 2, SSD1306_WHITE);
  if (paramValues[0] == 1)
    display.drawTriangle(iX, iY + iSize, iX + iSize / 2, iY, iX + iSize, iY + iSize, SSD1306_WHITE);
  if (paramValues[0] == 2)
  {
    display.drawLine(iX, iY + iSize, iX + iSize, iY, SSD1306_WHITE);
    display.drawLine(iX + iSize, iY, iX + iSize, iY + iSize, SSD1306_WHITE);
  }
  if (paramValues[0] == 3)
    display.drawRect(iX, iY, iSize, iSize, SSD1306_WHITE);

  // Нижня інфо-панель
  display.drawLine(0, 50, 128, 50, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(2, 54);
  display.print("T:");
  display.print(paramValues[1]);
  display.setCursor(34, 54);
  display.print("W:");
  display.print(paramValues[0]);
  display.setCursor(66, 54);
  display.print("G:");
  display.print(paramValues[2]);
  display.setCursor(98, 54);
  display.print("D:");
  display.print(paramValues[3]);
  display.display();
}

void setup()
{
  Wire1.setSDA(14);
  Wire1.setSCL(15);
  Wire1.begin();
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C, true, false);
  EEPROM.begin(256);

  for (int i = 0; i < 8; i++)
  {
    pinMode(buttonPins[i], INPUT_PULLUP);
    pinMode(ledPins[i], OUTPUT);
    digitalWrite(ledPins[i], LOW);
  }
  for (int i = 0; i < 4; i++)
    pinMode(sensorPins[i], INPUT_PULLDOWN);
  pinMode(syncInPin, INPUT_PULLDOWN);
  for (int i = 0; i < 4; i++)
    pinMode(touchPins[i], INPUT);

  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(15, 15);
  display.println("LOADED");
  display.setCursor(15, 38);
  display.println("SLOT 1");
  display.display();
  delay(1500);

  loadSequenceFromEEPROM(1);
}

void loop()
{
  updateTouchButtons();

  if (isPopUpActive && (millis() - popUpTimer > POPUP_DURATION))
    isPopUpActive = false;

  bool shift = digitalRead(sensorPins[3]);
  bool currentRecSensor = digitalRead(sensorPins[3]);
  if (currentRecSensor == HIGH && lastRecSensorState == LOW && !isPlaying)
  {
    delay(50);
    if (digitalRead(sensorPins[3]) == HIGH)
    {
      isRecording = !isRecording;
      isPopUpActive = false;
      if (isRecording)
      {
        seqLength = 0;
        for (int i = 0; i < 16; i++)
          sequence[i] = -1;
      }
      delay(200);
    }
  }
  lastRecSensorState = currentRecSensor;
  digitalWrite(ledPins[6], isRecording);

  int activeKey = -1;
  bool restKeyIsHeld = (digitalRead(buttonPins[7]) == LOW);

  for (int i = 0; i < 8; i++)
  {
    if (digitalRead(buttonPins[i]) == LOW)
    {
      if (i < 7)
      {
        if (restKeyIsHeld && !isRecording)
        {
          saveSequenceToEEPROM(i + 1);
          delay(400);
          return;
        }
        else if (shift && !isRecording)
        {
          loadSequenceFromEEPROM(i + 1);
          delay(400);
          return;
        }
      }
      activeKey = i;
      if (isRecording && seqLength < 16)
      {
        sequence[seqLength++] = i;
        if (seqLength <= 8)
          digitalWrite(ledPins[seqLength - 1], HIGH);
        delay(250);
        if (seqLength <= 8)
          digitalWrite(ledPins[seqLength - 1], LOW);
      }
    }
  }

  static int lastActiveKey = -1;
  if (activeKey != -1 && activeKey != 7)
  {
    if (activeKey != lastActiveKey && !isPlaying)
      noteTriggered = true;
  }
  lastActiveKey = activeKey;

  if (digitalRead(sensorPins[0]))
  {
    currentOctave = max(1, currentOctave - 1);
    delay(250);
  }
  if (digitalRead(sensorPins[1]))
  {
    currentOctave = min(7, currentOctave + 1);
    delay(250);
  }

  if (digitalRead(sensorPins[2]))
  {
    if (isRecording)
      isRecording = false;
    isPlaying = !isPlaying;
    seqStep = 0;
    delay(300);
  }

  // Керування кроками з урахуванням SWING та оновленого MUTE MODES
  if (isPlaying && seqLength > 0)
  {
    unsigned long dynamicInterval = stepDurationMs / paramValues[10];

    long swingOffset = 0;
    if (paramValues[4] > 0)
    {
      long maxOffset = dynamicInterval * 0.5f;
      long calculatedOffset = (maxOffset * paramValues[4]) / 100;
      if (seqStep % 2 == 0)
        swingOffset = calculatedOffset;
      else
        swingOffset = -calculatedOffset;
    }

    if (rollerActive)
      dynamicInterval = dynamicInterval / 4;

    if (millis() - lastStepTime > (dynamicInterval + swingOffset) || digitalRead(syncInPin))
    {
      seqStep = (seqStep + 1) % seqLength;
      lastStepTime = millis();
      noteTriggered = true;
      if (digitalRead(syncInPin))
        digitalWrite(ledPins[4], HIGH);
    }
    else
    {
      if (!digitalRead(syncInPin) && (seqStep % 8 != 4))
        digitalWrite(ledPins[4], LOW);
    }

    // Нова логіка визначення, чи глушити поточний крок
    bool shouldMute = false;
    if (muteMode == 1 && (seqStep % 2 == 0))
      shouldMute = true;
    if (muteMode == 2 && (seqStep % 3 == 0))
      shouldMute = true;
    if (muteMode == 3 && (random(0, 100) < 50))
      shouldMute = true; // 50% шанс глічу

    if (shouldMute)
      noteToPlay = -1;
    else
      noteToPlay = sequence[seqStep];
  }
  else if (!isPlaying)
  {
    noteToPlay = activeKey;
  }

  if (!isRecording)
  {
    for (int i = 0; i < 8; i++)
    {
      int activeStepIndicator = seqStep % 8;
      bool ledState = ((noteToPlay == i && noteToPlay != 7) || (isPlaying && activeStepIndicator == i));
      if (i != 4 && i != 6)
        digitalWrite(ledPins[i], ledState);
    }
  }

  static uint32_t displayTimer = 0;
  if (millis() - displayTimer > 100)
  {
    drawInterface();
    displayTimer = millis();
  }
}

// =========================================================================
// 🎚️ ЯДРО 1: АУДІО-РУШІЙ
// =========================================================================

float lastFilteredSample = 0.0f;

float generateWave(float p, int morph)
{
  if (morph == 3)
    return (p < PI) ? 1.0f : -1.0f;
  float sineSample = sinf(p);
  float triSample = (p < PI) ? -1.0f + (2.0f * p / PI) : 3.0f - (2.0f * p / PI);
  float sawSample = (p / PI) - 1.0f;

  if (morph < 50)
  {
    float factor = morph / 50.0f;
    return (sineSample * (1.0f - factor)) + (triSample * factor);
  }
  else
  {
    float factor = (morph - 50) / 49.0f;
    return (triSample * (1.0f - factor)) + (sawSample * factor);
  }
}

void setup1()
{
  i2sOutput.setBCLK(0);
  i2sOutput.setDATA(2);
  i2sOutput.begin(sampleRate);
}

void loop1()
{
  static int envState = 0;
  static float envLevel = 0.0f;

  if (noteTriggered)
  {
    noteTriggered = false;
    envState = 1;
    if (paramValues[6] == 0)
    {
      envLevel = 1.0f;
      envState = 2;
    }
  }

  if (envState == 1)
  {
    float attRate = 1.0f / (44.1f * (paramValues[6] + 0.5f));
    envLevel += attRate;
    if (envLevel >= 1.0f)
    {
      envLevel = 1.0f;
      envState = 2;
    }
  }
  else if (envState == 2)
  {
    float decRate = 1.0f / (44.1f * (paramValues[7] * 3.0f + 1.0f));
    envLevel -= decRate;
    if (envLevel <= 0.0f)
    {
      envLevel = 0.0f;
      envState = 0;
    }
  }
  else
  {
    envLevel = 0.0f;
  }

  if ((noteToPlay != -1 && noteToPlay != 7) || envLevel > 0.0f)
  {
    if (noteToPlay != -1 && noteToPlay != 7)
    {
      targetFreq = noteFreqs[noteToPlay] * powf(2.0f, (float)(currentOctave - 4));
    }

    if (potValues[2] == 0)
    {
      currentFreq = targetFreq;
    }
    else
    {
      float glideSpeed = map(potValues[2], 1, 99, 140, 3) / 1000.0f;
      currentFreq += (targetFreq - currentFreq) * glideSpeed;
    }

    float lfoRateHz = 0.1f + (paramValues[8] * 0.15f);
    float lfoPhaseInc = (2.0f * PI * lfoRateHz) / (float)sampleRate;
    phaseLFO += lfoPhaseInc;
    if (phaseLFO >= 2.0f * PI)
      phaseLFO -= 2.0f * PI;
    float lfoOut = sinf(phaseLFO);

    float lfoMod = lfoOut * (paramValues[9] / 100.0f) * 35.0f;
    float activeTone = potValues[0] + lfoMod;
    if (activeTone < 0.0f)
      activeTone = 0.0f;
    if (activeTone > 99.0f)
      activeTone = 99.0f;

    float phaseIncA = (2.0f * PI * currentFreq) / (float)sampleRate;
    phaseA += phaseIncA;
    if (phaseA >= 2.0f * PI)
      phaseA -= 2.0f * PI;
    float sampleA = generateWave(phaseA, potValues[1]);

    float detuneFactor = 1.0f + (potValues[3] * 0.00015f);
    float phaseIncB = (2.0f * PI * currentFreq * detuneFactor) / (float)sampleRate;
    phaseB += phaseIncB;
    if (phaseB >= 2.0f * PI)
      phaseB -= 2.0f * PI;
    float sampleB = generateWave(phaseB, potValues[1]);

    float phaseIncSub = (2.0f * PI * (currentFreq * 0.5f)) / (float)sampleRate;
    phaseSub += phaseIncSub;
    if (phaseSub >= 2.0f * PI)
      phaseSub -= 2.0f * PI;
    float sampleSub = generateWave(phaseSub, 1);
    float subVol = paramValues[5] / 100.0f;

    float mixedSignal = (sampleA + sampleB) * 0.5f;
    mixedSignal += sampleSub * subVol;
    mixedSignal *= 0.65f;

    float filteredSignal = (lastFilteredSample * 0.7f) + (mixedSignal * 0.3f);
    lastFilteredSample = filteredSignal;

    if (activeTone < 85.0f)
    {
      float filterFactor = activeTone / 85.0f;
      filteredSignal = (sinf(phaseA) * (1.0f - filterFactor)) + (filteredSignal * filterFactor);
    }

    filteredSignal *= envLevel;

    int16_t finalAudio = (int16_t)(filteredSignal * 2800.0f);
    i2sOutput.write(finalAudio);
    i2sOutput.write(finalAudio);
  }
  else
  {
    phaseA = 0;
    phaseB = 0;
    phaseSub = 0;
    lastFilteredSample = 0.0f;
    i2sOutput.write((int16_t)0);
    i2sOutput.write((int16_t)0);
  }
}