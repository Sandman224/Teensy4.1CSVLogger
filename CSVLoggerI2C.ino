// CSVLogger v0.8 for Colorado State University formula SAE team car
// Written by Aidan Farley
// For use with SensorReader v0.3
#include <Arduino.h>
#include "SdFat.h"
#include "RingBuf.h"
#include <i2c_driver.h>
#include <i2c_driver_wire.h>
#include <TimeLib.h>

void receiveEvent(int data);
void requestEvent();

// Use Teensy SDIO
#define SD_CONFIG SdioConfig(FIFO_SDIO)

// Interval between logging points
// CHECK FOR SPARE MICROS BEFORE DECREASING
//#define LOG_INTERVAL_USEC 2000 // use for 0.004 second log interval
//#define LOG_INTERVAL_USEC 1500 // use for 0.003 second log interval
#define LOG_INTERVAL_USEC 1000 // use for 0.002 second log interval

// Predefine file size
#define LOG_FILE_SIZE 1073741824 // Approx 30GB for use with 32GB card
// Set value close to size of microSD card used
// File must be pre-allocated to avoid huge delays searching for free clusters

// Ram write buffer size definition
#define RING_BUF_CAPACITY 400 * 512 // Default, space to hold more than 800 ms of data for 10 byte lines at 25 ksps.
//#define RING_BUF_CAPACITY 850*512 // highest reccomended value for 1MB RAM

#define BUFFER_LENGTH 64 // set I2C buffer length to 64 bits

boolean debug = true; // set to false during normal use

SdFs sd;
FsFile file;
int ledPin = 13;     // Pin for optional LED logging active indicator
int buttonPin = 33;  // Record switch pin
int buttonState = 0; // initial state to not record when circuit is open
int recording = 0;
String ec = "";              // initializing error code string
String data = "";            // initializing data string
char fileName[50];           // initializing fileName char
char TIME_HEADER[] = "T";    // Header tag for serial time sync message
void (*resetFunc)(void) = 0; // declare reset function at address 0 (not currently used)

void setup()
{
  // set the Time library to use Teensy 4.1's RTC to keep time
  setSyncProvider(getTeensy3Time);

  // initialize the digital pins
  pinMode(ledPin, OUTPUT);
  pinMode(buttonPin, INPUT);

  // power on blink
  digitalWrite(ledPin, HIGH);
  delay(200);
  digitalWrite(ledPin, LOW);
  delay(500);

  Serial.begin(115200);
  Serial1.begin(250000);

  delay(100);

  if (timeStatus() != timeSet)
  {
    Serial.println("Unable to sync with the RTC");
  }
  else
  {
    Serial.println("RTC has set the system time");
  }

  if (Serial.available())
  {
    time_t t = processSyncMessage();
    if (t != 0)
    {
      Teensy3Clock.set(t); // set the RTC
      setTime(t);
    }
  }

  // check the value is a valid time (greater than Jan 1 2022)
  if (Teensy3Clock.get() < 1641038400)
  {
    ec = "RTC time not set";
    errorState();
  }

  if (Serial)
  {
    Serial.print("Time: ");
    Serial.print(hour());
    printDigits(minute());
    printDigits(second());
    Serial.print(" ");
    Serial.print(month());
    Serial.print("/");
    Serial.print(day());
    Serial.print("/");
    Serial.print(year());
    Serial.println();
  }

  delay(50);
  Wire.begin(9);                // join i2c bus with address #9
  Wire.onReceive(receiveEvent); // register event
  Wire.onRequest(requestEvent);
  // trigger sensor teensy to send data
  recording = 1;
  Wire.write(recording);

  if (data.length() > 0)
  {
    Serial.println("Sensors connected!");
  }
  else
  {
    Serial.println("Waiting for sensors...");
    while (data.length() == 0)
    {
      digitalWrite(ledPin, LOW);
      delay(500);
      digitalWrite(ledPin, HIGH);
      delay(500);
      digitalWrite(ledPin, LOW);
      Wire.write(recording); // continue sending trigger untill recieves data
    }
    Serial.println("Sensors connected!");
  }
  // trigger sensor teensy to stop sending data
  recording = 0;
  Wire.write(recording);

  for (int i = 0; i < 3; i++)
  {
    digitalWrite(ledPin, LOW);
    delay(80);
    digitalWrite(ledPin, HIGH);
    delay(80);
    digitalWrite(ledPin, LOW);
  }
}

// RingBuf for File type FsFile.
RingBuf<FsFile, RING_BUF_CAPACITY> rb;

void logData()
{
  // Initialize the SD
  if (!sd.begin(SD_CONFIG))
  {
    sd.initErrorHalt(&Serial);
  }
  // Get filename
  getFileName();

  // Open or create file - truncate existing file.
  if (!file.open(fileName, O_RDWR | O_CREAT | O_TRUNC))
  {
    Serial.println("open failed\n");
    return;
  }
  // File pre-allocation
  if (!file.preAllocate(LOG_FILE_SIZE))
  {
    Serial.println("preAllocate failed\n");
    file.close();
    return;
  }
  // initialize the RingBuf.
  rb.begin(&file);
  Serial.println("Switch off to stop");

  // Max RingBuf used bytes. Useful to understand RingBuf overrun.
  size_t maxUsed = 0;

  // Min spare micros in loop.
  int32_t minSpareMicros = INT32_MAX;

  // Start time.
  uint32_t logTime = micros();
  // Log data until Serial input, circuit open or file full.

  while (buttonState == HIGH)
  {
    recording = 1;
    buttonState = digitalRead(buttonPin);
    // Amount of data in ringBuf.
    size_t n = rb.bytesUsed();

    if ((n + file.curPosition()) > (LOG_FILE_SIZE - 20))
    {
      ec = "File full";
      break;
    }
    if (n > maxUsed)
    {
      maxUsed = n;
    }
    if (n >= 512 && !file.isBusy())
    {
      // Not busy only allows one sector before possible busy wait.
      // Write one sector from RingBuf to file.
      if (512 != rb.writeOut(512))
      {
        ec = "writeOut failed";
        break;
      }
    }
    // Time for next point.
    logTime += LOG_INTERVAL_USEC;
    int32_t spareMicros = logTime - micros();
    if (spareMicros < minSpareMicros)
    {
      minSpareMicros = spareMicros;
    }
    if (spareMicros <= 0)
    {
      ec = "Rate too fast ";
      ec += spareMicros;
      break;
    }
    // Wait until time to log data.
    while (micros() < logTime)
    {
    }
    if (data.length() > 0)
    {
      logTime += LOG_INTERVAL_USEC;

      // Print data into the RingBuf
      rb.print(logTime);
      rb.print("ns");
      rb.write(',');
      rb.println(data);
      // rb.sync(); //EXPIREMENTAL BYPASSES RINGBUF REQUIRES FAST SD CARD CLASS 3 OR ABOVE
      if (debug == true)
      {
        Serial.println(data);
      }
    }
    if (rb.getWriteError())
    {
      // Error caused by too few free bytes in RingBuf.
      ec = "WriteError";
      break;
    }
  }
  recording = 0;

  // Write any RingBuf data to file.
  rb.sync();
  file.truncate();
  file.rewind();

  // finished saving flashes
  for (int i = 0; i <= 10; i++)
  {
    digitalWrite(ledPin, LOW);
    delay(20);
    digitalWrite(ledPin, HIGH);
    delay(10);
    digitalWrite(ledPin, LOW);
  }

  if (ec.length() > 0)
  {
    Serial.println("LOGGING STOPPED");
    errorState();
  }

  // Print first twenty lines of file.
  if ((debug == true))
  {
    Serial.println("Data");
    for (uint8_t n = 0; n < 20 && file.available();)
    {
      int c = file.read();
      if (c < 0)
      {
        break;
      }
      Serial.write(c);
      if (c == '\n')
        n++;
    }
  }
  Serial.print("Log file: ");
  Serial.println(fileName);
  Serial.print("fileSize: ");
  Serial.print((uint32_t)file.fileSize());
  Serial.println(" bytes");

  if ((debug == true))
  {
    Serial.print("maxBytesUsed: ");
    Serial.println(maxUsed);
    Serial.print("minSpareMicros: ");
    Serial.println(minSpareMicros);
  }
  file.close();
}

void errorState()
{
  Serial.println("");
  Serial.print("ERROR: ");
  Serial.println(ec);
  if (ec.indexOf("RTC time not set") < 0)
  {
    Serial.println("Input any key to clear");
    while (ec > 0)
    {
      digitalWrite(ledPin, LOW);
      delay(500);
      digitalWrite(ledPin, HIGH);
      delay(500);
      digitalWrite(ledPin, LOW);
      if (Serial.available())
      {
        Serial.flush();
        Serial.println("Error cleared - Continuing...");
        Serial.println("");
        ec = "";
        buttonState = digitalRead(buttonPin);
        delay(1000);
        break;
      }
    }
  }
  else
  {
    Serial.println("SYNC TIME IMMEDIATELY");
    Serial.println("Check if the clock battery is dead or disconnected");
    Serial.println("Plug in computer with correct time and reflash code");
    Serial.println("Time is set from time of code compile on computer");
    while (ec > 0)
    {
      digitalWrite(ledPin, LOW);
      delay(500);
      digitalWrite(ledPin, HIGH);
      delay(500);
      digitalWrite(ledPin, LOW);
      delay(200);
      digitalWrite(ledPin, HIGH);
      delay(200);
      digitalWrite(ledPin, LOW);
      delay(200);
      digitalWrite(ledPin, HIGH);
      delay(200);
      digitalWrite(ledPin, LOW);
    }
  }
}

void clearSerialInput()
{
  for (uint32_t m = micros(); micros() - m < 10000;)
  {
    if (Serial.read() >= 0)
    {
      m = micros();
    }
  }
}

void printDigits(int digits)
{
  // utility function for digital clock display: prints preceding colon and leading 0
  Serial.print(":");
  if (digits < 10)
    Serial.print('0');
  Serial.print(digits);
}

time_t getTeensy3Time()
{
  return Teensy3Clock.get();
}

unsigned long processSyncMessage()
{
  unsigned long pctime = 0L;
  const unsigned long DEFAULT_TIME = 1641038400; // Jan 1 2022

  if (Serial.find(TIME_HEADER))
  {
    pctime = Serial.parseInt();
    return pctime;
    if (pctime < DEFAULT_TIME)
    {              // check the value is a valid time (greater than Jan 1 2022)
      pctime = 0L; // return 0 to indicate that the time is not valid
    }
  }
  return pctime;
}

void getFileName()
{
  String dateName = String(year()) + "-" +
                    String(month()) + "-" +
                    String(day()) + "-" +
                    String(hour()) + "-" +
                    String(minute()) + "-" +
                    String(second()) + ".csv";
  dateName.toCharArray(fileName, 50);
}

void receiveEvent(int test)
{
  if (recording == 1)
  {
    data = "";
    while (Wire.available() > 0)
    {
      // loop through all characters
      char i = Wire.read(); // receive byte as a character
      data += i;
    }
  }
}

void requestEvent()
{
  Wire.write(recording); // respond with message of 1 byte
                         // as expected by master
}

void loop()
{
  clearSerialInput();
  digitalWrite(ledPin, LOW);
  Serial.println("Switch on to start");
  buttonState = digitalRead(buttonPin);

  while (buttonState == LOW)
  {
    buttonState = digitalRead(buttonPin);
  }
  if (buttonState == HIGH)
  {
    digitalWrite(ledPin, HIGH);
    clearSerialInput();
    logData();
  }
}
