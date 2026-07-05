#include <Arduino.h>
#include <Wire.h>
#include <I2S.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

// --- НАЛАГОДЖЕННЯ ЧЕРЕЗ SERIAL ---
// 1 = вмикає діагностичні логи (фронти клавіш, події запису/save/load,
// heartbeat зі станом кнопок) у Serial @115200. 0 = повністю компілюється в ніщо.
#define DEBUG_LOG 0

// --- КОНФІГУРАЦІЯ ЕКРАНА ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire1, -1);

// --- ПІНИ ---
// УВАГА: ledPins[4] (GP20) та ledPins[6] (GP22) НЕ використовуються як
// світлодіоди — ці піни зайняті входами: GP20 = syncInPin, GP22 = sensorPins[3]
// (сенсор запису). Вони конфігуруються як входи й НЕ керуються як LED.
// Примітка (#6): на стоковому Raspberry Pi Pico GP23 (ledPins[7]) та GP29
// (touchPins[3]) зарезервовані (SMPS / VSYS sense) — валідно лише на «голому» RP2040.
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
// Лічильник тригерів нот. Ядро 0 інкрементує (єдиний виробник), ядро 1
// відстежує останнє значення — так тригер не губиться й не задвоюється.
volatile uint32_t noteTriggerSeq = 0;

// --- СИСТЕМА ЦИФРОВИХ ПАРАМЕТРІВ ---
const int NUM_PARAMS = 11;
// Іменовані індекси параметрів (порядок збігається з paramNames / paramValues)
enum ParamIndex
{
  P_WAVE = 0,
  P_TONE,
  P_GLIDE,
  P_DETUNE,
  P_SWING,
  P_SUBOSC,
  P_ATTACK,
  P_DECAY,
  P_LFO_RATE,
  P_LFO_DEPTH,
  P_CLOCKDIV
};
int currentParam = 0;
const char *paramNames[NUM_PARAMS] = {
    "WAVE", "TONE", "GLIDE", "DETUNE",
    "SWING", "SUB-OSC", "ATTACK", "DECAY",
    "LFO RATE", "LFO DEPTH", "CLOCK DIV"};
// volatile: пишеться ядром 0 (updateTouchButtons), читається ядром 1 (loop1)
volatile int paramValues[NUM_PARAMS] = {1, 50, 30, 10, 0, 0, 0, 30, 40, 0, 1};

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
bool syncPrev = false; // попередній рівень syncInPin (детекція фронту)
const int fixedBpm = 120;
unsigned long stepDurationMs = (60000 / fixedBpm) / 2;

bool recSensorPrev = false;      // попередній стан сенсора запису (для детекції фронту)
bool recConsumedAsShift = false; // під час цього торкання сенсор використали як SHIFT
char flashMessage[24] = ""; // без String — уникаємо фрагментації кучі
unsigned long flashMessageTime = 0;

int recBlinkLed = -1;          // LED-фідбек при записі кроку (-1 = вимкнено)
unsigned long recBlinkOff = 0; // час згасання recBlinkLed (неблокуюче)

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
      if (currentParam == 0 && paramValues[P_WAVE] > 3)
        paramValues[P_WAVE] = 0;
      if (currentParam == 10 && paramValues[P_CLOCKDIV] > 8)
        paramValues[P_CLOCKDIV] = 1;
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
        if (currentParam == 0 && paramValues[P_WAVE] > 3)
          paramValues[P_WAVE] = 0;
        if (currentParam == 10 && paramValues[P_CLOCKDIV] > 8)
          paramValues[P_CLOCKDIV] = 1;
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
      if (currentParam == 0 && paramValues[P_WAVE] < 0)
        paramValues[P_WAVE] = 3;
      if (currentParam == 10 && paramValues[P_CLOCKDIV] < 1)
        paramValues[P_CLOCKDIV] = 8;
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
        if (currentParam == 0 && paramValues[P_WAVE] < 0)
          paramValues[P_WAVE] = 3;
        if (currentParam == 10 && paramValues[P_CLOCKDIV] < 1)
          paramValues[P_CLOCKDIV] = 8;
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

  potValues[0] = paramValues[P_TONE];
  potValues[2] = paramValues[P_GLIDE];
  potValues[3] = paramValues[P_DETUNE];

  if (paramValues[P_WAVE] == 0)
    potValues[1] = 0;
  if (paramValues[P_WAVE] == 1)
    potValues[1] = 50;
  if (paramValues[P_WAVE] == 2)
    potValues[1] = 99;
  if (paramValues[P_WAVE] == 3)
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
  snprintf(flashMessage, sizeof(flashMessage), "SAVED TO\nSLOT %d", slot);
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
  snprintf(flashMessage, sizeof(flashMessage), "LOADED\nSLOT %d", slot);
  flashMessageTime = millis();
}

// =========================================================================
// 🧠 ЯДРО 0: ІНТЕРФЕЙС З НОВОЮ ВЕРСТКОЮ ТА СЕКВЕНСОРОМ
// =========================================================================

void drawInterface()
{
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (flashMessage[0] != '\0' && (millis() - flashMessageTime < 2000))
  {
    display.setTextSize(2);
    display.setCursor(10, 15);
    char *nl = strchr(flashMessage, '\n');
    if (nl != nullptr)
    {
      *nl = '\0';
      display.print(flashMessage);
      display.setCursor(10, 38);
      display.print(nl + 1);
      *nl = '\n'; // відновлюємо роздільник рядків
    }
    else
    {
      display.print(flashMessage);
    }
    display.display();
    return;
  }
  else if (flashMessage[0] != '\0')
  {
    flashMessage[0] = '\0';
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
      display.getTextBounds(waveNames[paramValues[P_WAVE]], 0, 0, &x1, &y1, &w, &h);
      int waveNameX = (SCREEN_WIDTH - w) / 2;
      display.setCursor(waveNameX, 24);
      display.print(waveNames[paramValues[P_WAVE]]);

      int wX = 52;
      int wY = 46;
      if (paramValues[P_WAVE] == 0)
        display.drawCircle(wX + 10, wY, 8, SSD1306_WHITE);
      if (paramValues[P_WAVE] == 1)
        display.drawTriangle(wX, wY + 8, wX + 10, wY - 8, wX + 20, wY + 8, SSD1306_WHITE);
      if (paramValues[P_WAVE] == 2)
      {
        display.drawLine(wX, wY + 8, wX + 14, wY - 8, SSD1306_WHITE);
        display.drawLine(wX + 14, wY - 8, wX + 14, wY + 8, SSD1306_WHITE);
      }
      if (paramValues[P_WAVE] == 3)
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
  if (paramValues[P_WAVE] == 0)
    display.drawCircle(iX + iSize / 2, iY + iSize / 2, iSize / 2, SSD1306_WHITE);
  if (paramValues[P_WAVE] == 1)
    display.drawTriangle(iX, iY + iSize, iX + iSize / 2, iY, iX + iSize, iY + iSize, SSD1306_WHITE);
  if (paramValues[P_WAVE] == 2)
  {
    display.drawLine(iX, iY + iSize, iX + iSize, iY, SSD1306_WHITE);
    display.drawLine(iX + iSize, iY, iX + iSize, iY + iSize, SSD1306_WHITE);
  }
  if (paramValues[P_WAVE] == 3)
    display.drawRect(iX, iY, iSize, iSize, SSD1306_WHITE);

  // Нижня інфо-панель
  display.drawLine(0, 50, 128, 50, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(2, 54);
  display.print("T:");
  display.print(paramValues[P_TONE]);
  display.setCursor(34, 54);
  display.print("W:");
  display.print(paramValues[P_WAVE]);
  display.setCursor(66, 54);
  display.print("G:");
  display.print(paramValues[P_GLIDE]);
  display.setCursor(98, 54);
  display.print("D:");
  display.print(paramValues[P_DETUNE]);
  display.display();
}

void setup()
{
#if DEBUG_LOG
  Serial.begin(115200);
#endif
  Wire1.setSDA(14);
  Wire1.setSCL(15);
  Wire1.begin();
  bool oledOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3C, true, false);
  EEPROM.begin(256);

  for (int i = 0; i < 8; i++)
  {
    pinMode(buttonPins[i], INPUT_PULLUP);
    // Пропускаємо ledPins[4]=GP20 та ledPins[6]=GP22 — вони налаштовуються
    // як входи (sync / сенсор запису) нижче й не є світлодіодами.
    if (i != 4 && i != 6)
    {
      pinMode(ledPins[i], OUTPUT);
      digitalWrite(ledPins[i], LOW);
    }
  }
  for (int i = 0; i < 4; i++)
    pinMode(sensorPins[i], INPUT_PULLDOWN);
  pinMode(syncInPin, INPUT_PULLDOWN);
  for (int i = 0; i < 4; i++)
    pinMode(touchPins[i], INPUT_PULLDOWN); // тач активний-HIGH -> підтяжка донизу проти «плаваючого» входу

  if (!oledOk)
  {
    // OLED не знайдено: сигналізуємо блиманням LED0 і продовжуємо
    // (аудіо-ядро працює й без екрана).
    for (int i = 0; i < 6; i++)
    {
      digitalWrite(ledPins[0], HIGH);
      delay(120);
      digitalWrite(ledPins[0], LOW);
      delay(120);
    }
  }

  randomSeed(rp2040.hwrand32()); // апаратний ГВЧ -> недетермінований патерн RND-mute

  // Брендований сплеш замість дубля "LOADED SLOT 1" —
  // саме повідомлення про завантаження покаже loadSequenceFromEEPROM(1).
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(3);
  display.setCursor(30, 12);
  display.println("EX");
  display.setTextSize(2);
  display.setCursor(22, 44);
  display.println("SYNTH");
  display.display();
  delay(1500);

  loadSequenceFromEEPROM(1);
}

void loop()
{
  updateTouchButtons();

  if (isPopUpActive && (millis() - popUpTimer > POPUP_DURATION))
    isPopUpActive = false;

  // Сенсор запису (GP22) має подвійну роль:
  //  - коротке торкання (натиснув-відпустив без клавіші) -> перемикання REC;
  //  - утримання = SHIFT-модифікатор для завантаження слота (див. цикл клавіш).
  bool recNow = (digitalRead(sensorPins[3]) == HIGH);
  bool shift = recNow;
  if (recNow && !recSensorPrev)
    recConsumedAsShift = false; // почалося нове торкання
  if (!recNow && recSensorPrev && !recConsumedAsShift && !isPlaying)
  {
    // «чисте» торкання без клавіші -> перемикаємо режим запису
    isRecording = !isRecording;
    isPopUpActive = false;
    if (isRecording)
    {
      seqLength = 0;
      for (int i = 0; i < 16; i++)
        sequence[i] = -1;
    }
  }
  recSensorPrev = recNow;

  int activeKey = -1;
  bool restKeyIsHeld = (digitalRead(buttonPins[7]) == LOW);

  // Неблокуюче сканування клавіш із дебаунсом: приймаємо зміну стану лише
  // після того, як сирий рівень протримався стабільним KEY_DEBOUNCE_MS.
  // Дії SAVE/LOAD/запис кроку — по фронту стабільного натискання (justPressed).
  static bool keyStable[8] = {false, false, false, false, false, false, false, false};
  static bool keyLastRaw[8] = {false, false, false, false, false, false, false, false};
  static unsigned long keyChangeTime[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  const unsigned long KEY_DEBOUNCE_MS = 25;
  unsigned long nowKeys = millis();

  for (int i = 0; i < 8; i++)
  {
    bool raw = (digitalRead(buttonPins[i]) == LOW);
    if (raw != keyLastRaw[i])
    {
      keyLastRaw[i] = raw;
      keyChangeTime[i] = nowKeys; // рівень «тремтить» — перезапускаємо відлік стабільності
    }

    bool justPressed = false;
    if ((nowKeys - keyChangeTime[i]) >= KEY_DEBOUNCE_MS && raw != keyStable[i])
    {
      keyStable[i] = raw;
      justPressed = raw; // стабільний перехід у «натиснуто»
#if DEBUG_LOG
      Serial.printf("[KEY] t=%lu i=%d %s\n", nowKeys, i, raw ? "DOWN" : "up");
#endif
    }
    bool pressed = keyStable[i];

    if (!pressed)
      continue;

    if (i < 7 && restKeyIsHeld && !isRecording)
    {
      if (justPressed)
      {
        saveSequenceToEEPROM(i + 1);
#if DEBUG_LOG
        Serial.printf("[SAVE] t=%lu key=%d -> slot %d\n", millis(), i, i + 1);
#endif
      }
      continue; // не трактуємо як ноту
    }
    if (i < 7 && shift && !isRecording)
    {
      if (justPressed)
      {
        loadSequenceFromEEPROM(i + 1);
        recConsumedAsShift = true; // придушуємо перемикання REC при відпусканні
#if DEBUG_LOG
        Serial.printf("[LOAD] t=%lu key=%d -> slot %d\n", millis(), i, i + 1);
#endif
      }
      continue;
    }

    activeKey = i;
    if (isRecording && justPressed && seqLength < 16)
    {
      sequence[seqLength++] = i;
#if DEBUG_LOG
      Serial.printf("[REC] t=%lu -> step %d = note %d (len=%d)\n", millis(), seqLength - 1, i, seqLength);
#endif
      int led = seqLength - 1;
      if (led < 8 && led != 4 && led != 6)
      {
        digitalWrite(ledPins[led], HIGH);
        recBlinkLed = led;
        recBlinkOff = millis() + 150;
      }
    }
  }

  // Неблокуюче згасання LED-фідбеку запису кроку
  if (recBlinkLed != -1 && millis() >= recBlinkOff)
  {
    digitalWrite(ledPins[recBlinkLed], LOW);
    recBlinkLed = -1;
  }

  static int lastActiveKey = -1;
  if (activeKey != -1 && activeKey != 7)
  {
    if (activeKey != lastActiveKey && !isPlaying)
      noteTriggerSeq++;
  }
  lastActiveKey = activeKey;

  // Октава та play/pause — по фронту натискання, без блокуючих delay()
  static bool octDownPrev = false, octUpPrev = false, playPrev = false;
  bool octDownNow = digitalRead(sensorPins[0]);
  if (octDownNow && !octDownPrev)
    currentOctave = max(1, currentOctave - 1);
  octDownPrev = octDownNow;

  bool octUpNow = digitalRead(sensorPins[1]);
  if (octUpNow && !octUpPrev)
    currentOctave = min(7, currentOctave + 1);
  octUpPrev = octUpNow;

  bool playNow = digitalRead(sensorPins[2]);
  if (playNow && !playPrev)
  {
    if (isRecording)
      isRecording = false;
    isPlaying = !isPlaying;
    seqStep = 0;
  }
  playPrev = playNow;

  // Керування кроками з урахуванням SWING та оновленого MUTE MODES
  if (isPlaying && seqLength > 0)
  {
    int clockDiv = (paramValues[P_CLOCKDIV] < 1) ? 1 : paramValues[P_CLOCKDIV]; // захист від ділення на 0
    unsigned long dynamicInterval = stepDurationMs / clockDiv;

    // Roller застосовуємо ДО розрахунку свінгу, щоб зсув брався від
    // реального (поділеного) інтервалу й не викликав переповнення.
    if (rollerActive)
      dynamicInterval = dynamicInterval / 4;

    long swingOffset = 0;
    if (paramValues[P_SWING] > 0)
    {
      long maxOffset = dynamicInterval * 0.5f;
      long calculatedOffset = (maxOffset * paramValues[P_SWING]) / 100;
      if (seqStep % 2 == 0)
        swingOffset = calculatedOffset;
      else
        swingOffset = -calculatedOffset;
    }

    // Зовнішній SYNC: реагуємо лише на фронт LOW->HIGH. Інакше утримання
    // лінії у HIGH прокручувало б секвенсор щоразу в loop() (runaway).
    bool syncNow = digitalRead(syncInPin);
    bool syncEdge = (syncNow && !syncPrev);
    syncPrev = syncNow;

    long stepInterval = (long)dynamicInterval + swingOffset;
    if (stepInterval < 1)
      stepInterval = 1; // страхування від нульового/від'ємного інтервалу

    if ((long)(millis() - lastStepTime) > stepInterval || syncEdge)
    {
      seqStep = (seqStep + 1) % seqLength;
      lastStepTime = millis();
      noteTriggerSeq++;
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

#if DEBUG_LOG
  static unsigned long dbgHb = 0;
  if (millis() - dbgHb > 2000)
  {
    dbgHb = millis();
    Serial.print("[HB] btn=");
    for (int i = 0; i < 8; i++)
      Serial.print(digitalRead(buttonPins[i]) == LOW ? '1' : '0');
    Serial.printf(" recSns=%d isRec=%d isPlay=%d actKey=%d note=%d\n",
                  (int)digitalRead(sensorPins[3]), isRecording, isPlaying, activeKey, noteToPlay);
  }
#endif

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

  static uint32_t lastTriggerSeq = 0;
  if (noteTriggerSeq != lastTriggerSeq)
  {
    lastTriggerSeq = noteTriggerSeq;
    envState = 1;
    if (paramValues[P_ATTACK] == 0)
    {
      envLevel = 1.0f;
      envState = 2;
    }
  }

  if (envState == 1)
  {
    float attRate = 1.0f / (44.1f * (paramValues[P_ATTACK] + 0.5f));
    envLevel += attRate;
    if (envLevel >= 1.0f)
    {
      envLevel = 1.0f;
      envState = 2;
    }
  }
  else if (envState == 2)
  {
    float decRate = 1.0f / (44.1f * (paramValues[P_DECAY] * 3.0f + 1.0f));
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
      // powf дуже дорогий на RP2040 (без FPU). Кешуємо множник октави
      // й перераховуємо його лише при зміні октави, а не щосемпла.
      static int cachedOctave = -999;
      static float octaveMul = 1.0f;
      if (currentOctave != cachedOctave)
      {
        octaveMul = powf(2.0f, (float)(currentOctave - 4));
        cachedOctave = currentOctave;
      }
      targetFreq = noteFreqs[noteToPlay] * octaveMul;
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

    float lfoRateHz = 0.1f + (paramValues[P_LFO_RATE] * 0.15f);
    float lfoPhaseInc = (2.0f * PI * lfoRateHz) / (float)sampleRate;
    phaseLFO += lfoPhaseInc;
    if (phaseLFO >= 2.0f * PI)
      phaseLFO -= 2.0f * PI;
    float lfoOut = sinf(phaseLFO);

    float lfoMod = lfoOut * (paramValues[P_LFO_DEPTH] / 100.0f) * 35.0f;
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
    float subVol = paramValues[P_SUBOSC] / 100.0f;

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