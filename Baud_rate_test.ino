/*
  T-SIM7000G V1.5 - Modem Baud Scanner
  Upload this sketch.
  Open Serial Monitor @ 115200 baud.
*/

#include <HardwareSerial.h>

HardwareSerial SerialAT(1);

#define MODEM_RX      26
#define MODEM_TX      27
#define MODEM_PWRKEY   4

long baudRates[] = {
  9600,
  19200,
  38400,
  57600,
  115200
};

void powerOnModem()
{
  pinMode(MODEM_PWRKEY, OUTPUT);

  digitalWrite(MODEM_PWRKEY, LOW);
  delay(100);

  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(1200);

  digitalWrite(MODEM_PWRKEY, LOW);

  delay(6000);
}

void setup()
{
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("==================================");
  Serial.println("SIM7000 BAUD SCANNER");
  Serial.println("==================================");

  Serial.println("Powering modem...");
  powerOnModem();

  for(int i=0;i<5;i++)
  {
    long baud = baudRates[i];

    Serial.println();
    Serial.print("Trying ");
    Serial.print(baud);
    Serial.println(" baud");

    SerialAT.end();
    delay(500);

    SerialAT.begin(
      baud,
      SERIAL_8N1,
      MODEM_RX,
      MODEM_TX);

    delay(1000);

    while(SerialAT.available())
      SerialAT.read();

    SerialAT.println("AT");

    unsigned long start = millis();

    bool gotReply = false;

    while(millis()-start < 3000)
    {
      while(SerialAT.available())
      {
        char c = SerialAT.read();
        Serial.write(c);
        gotReply = true;
      }
    }

    if(!gotReply)
    {
      Serial.println("No response");
    }
  }

  Serial.println();
  Serial.println("Scan Complete");
}

void loop()
{
}