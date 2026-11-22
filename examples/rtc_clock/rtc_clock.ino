#include <Wire.h>
#include <RTClib.h>

RTC_DS3231 rtc;

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  if (!rtc.begin()) {
    Serial.println("DS3231 missing. Check SDA, SCL and power.");
    while (true) delay(1000);
  }
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    Serial.println("RTC initialized from the build time.");
  }
}

void loop() {
  char timestamp[] = "YYYY-MM-DD hh:mm:ss";
  Serial.println(rtc.now().toString(timestamp));
  delay(1000);
}
