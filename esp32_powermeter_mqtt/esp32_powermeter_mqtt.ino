/***************************************************************************
   Power Meter - for devices with pulse output

   Host device is an ESP32 WROOM 32 board

   The device is connected to WiFi, enabling (simple) OTA
   updates over Arduino IDE.

   The mqtt client pushes on the sensors/pulseenergymonitor/# topics the:
   - instant Watts measurements
   - counted kWh (RAM)
   NOTE: replace "<...>" fields with your data
 ***************************************************************************/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>


/*#define DEBUG*/                 /* activates debug on serial line*/
#define IN_PIN_PEM 18             /* input pin for pulse sensor */
#define ISR_TYPE FALLING          /* isr triggering mode */
#define HOUR 3600.0               /* hour as constant */
#define PULSES_KWH_RES 1.0        /* 1 KW = 1000W, power meter has 1000 inpulses resolution */
#define SECOND_ISR_PERIOD 0.09   /* pulse period is ~90 ms, used to ignore second interrupt arrival bug*/
#define MAX_POWER 7040            /* for a peek of 32A => 7040W, ignor higher measurements */

/*mqtt declarations*/
const unsigned int uiUpdate = 60;         /*seconds between sending mqtt updates*/
const char* mqtt_server = "HOSTNAME";
const char* mqtt_user = "USER";
const char* mqtt_password = "PASS";
const char* power_topic = "sensors/pulseenergymonitor/watts";
const char* kWh_topic = "sensors/pulseenergymonitor/kWh";
const char* cmd_topic = "sensors/pulseenergymonitor/cmd";
const char* lwt_topic = "sensors/pulseenergymonitor/lwt";
const char* lwt_msg_off = "OFF";
const char* lwt_msg_on = "ON";

/* SSID and Password of WiFi router */
const char* ssid = "SSID";
const char* password = "PASSWORD";


/* variables an */
WiFiClient espClient;
PubSubClient client(espClient);



unsigned long ulPreviousMillis = 0;       /* will store last time the main loop run */
unsigned long ulCurrentMillis = 0;        /* will store last time the main loop run */
volatile double dPulsePeriod = 0.0;       /*period measured  between 2 impulses*/
volatile double dPower = 0.0;             /*instant power, in W*/
volatile unsigned long ulStartTime = 0;   /*time measured when firts pulse from pulsed energy sensor fires*/
volatile double dKWh = 0.0;               /*cumulative kWh*/
volatile unsigned int uiIsrCount = 0;     /*counter to hold the number of measirements*/
volatile double dTotalW = 0.0;            /*total Watts measured over a raporting cycle on mqtt*/



/***********************************************************
   interrupt routine,
   used to measure the pulse length and calculate the power
************************************************************/
void IRAM_ATTR isr()
{

  /*test if isr issue*/
  /*time measured in seconds, from the last measurement*/
  dPulsePeriod = (double)(millis() - ulStartTime) / 1000.0;

  /*ignore, if period is shorther than pulse length ~90ms*/
  if (dPulsePeriod <= SECOND_ISR_PERIOD)
  {
    /*ignor measurement*/
    dPulsePeriod = 0.0;

  }
  else /*no interrupt issue, compute the power*/
  {

    uiIsrCount++;
    dPower = (PULSES_KWH_RES / (double)dPulsePeriod) * HOUR;

    /*filter wrong readings*/
    if (dPower > MAX_POWER)
    {
      /*reading error, reset power*/
      dPower = 0.0;
    }

    /*cumulative power*/
    dTotalW += dPower;

    /*update kWh*/
    dKWh += ((dPulsePeriod / HOUR) * (dPower / 1000.0));

  }

}

/***********************************************
   MQTT receiver callback
   coomad list, on sensors/pulseenergymonitor/cmd
   c - clear RAM stored kWh
   r - reset the system
************************************************/
void callback(char* topic, byte* payload, unsigned int length)
{
  byte comma = 0;
#ifdef DEBUG
  Serial.print("Message in topic: ");
  Serial.println(topic);
  Serial.print("Message ");

  for (int i = 0; i < length; i++)
  {
    Serial.print((char)payload[i]);
  }

  Serial.println("");
#endif

  /*reciver handler*/
  if ((char)payload[0] == 'c')
  {
    /*reset kWh counter*/
    dKWh = 0.0;
  }
  else if ((char)payload[0] == 'r')
  {
#ifdef DEBUG
    Serial.println("Warm Restart");
#endif
    ESP.restart();
  }

#ifdef DEBUG
  Serial.println();
#endif

}


/***************************************
   init routine
****************************************/
void setup()
{
#ifdef DEBUG
  Serial.begin(115200);
  Serial.println("Scanning...");
#endif

  /*set pin as output*/
  pinMode(IN_PIN_PEM, INPUT_PULLDOWN);

  /*attach function to isr routine*/
  attachInterrupt(IN_PIN_PEM, isr, ISR_TYPE);

  /*get current time for temporary variables*/
  ulStartTime = millis();
  ulPreviousMillis = ulStartTime;

  /*Connect to your WiFi router*/
  WiFi.begin(ssid, password);
#ifdef DEBUG
  Serial.println("");
#endif

  /*Wait for connection*/
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
#ifdef DEBUG
    Serial.print(".");
#endif

  }

  /*set-up mqtt*/
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

  /*connect to mqtt server, retry if needed*/
  while (!client.connected())
  {
#ifdef DEBUG
    Serial.println("Connecting to MQTT...");
#endif
    if (client.connect("pulseenergymonitor", mqtt_user, mqtt_password, lwt_topic, 0, 0, lwt_msg_off ))
    {
#ifdef DEBUG
      Serial.println("Connected to MQTT");
#endif
      client.publish(lwt_topic, lwt_msg_on, true);
    }
    else
    {
#ifdef DEBUG
      Serial.print("failed state ");
      Serial.print(client.state());
#endif
      delay(2000);
    }
  }

  /*subscribe for commands*/
  client.subscribe(cmd_topic);

  /*OTA*/
  ArduinoOTA
  .onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else /* U_SPIFFS*/
      type = "filesystem";
#ifdef DEBUG
    Serial.println("Start updating " + type);
#endif
  })
  .onEnd([]() {
#ifdef DEBUG
    Serial.println("\nEnd");
#endif
  })
  .onProgress([](unsigned int progress, unsigned int total) {
#ifdef DEBUG
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
#endif
  })
  .onError([](ota_error_t error) {
#ifdef DEBUG
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
#endif
  });

  ArduinoOTA.begin();

#ifdef DEBUG
  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
#endif


}


/*********************************
   Main loop
**********************************/
void loop()
{

  client.loop();
  ArduinoOTA.handle();

#ifdef DEBUG
  Serial.printf(String(dPulsePeriod).c_str());
  Serial.printf(" , ");
  Serial.printf(String(dPower).c_str());
  Serial.printf("\n");
#endif

  /*get time now*/
  ulCurrentMillis = millis();

  /*check if elapsed seconds defined between sending mqtt updates*/
  if (ulCurrentMillis - ulPreviousMillis >= (uiUpdate * 1000))
  {
    ulPreviousMillis = ulCurrentMillis;

    /*compute average of power*/
    dPower = dTotalW / uiIsrCount ;

    /*publish data over mqtt*/
    client.publish(power_topic, String(dPower).c_str(), true);
    client.publish(kWh_topic, String(dKWh).c_str(), true);

    /*delete accumulated values, for new mqtt update*/
    dPower = 0.0;
    dTotalW = 0.0;
    uiIsrCount = 0;
    dKWh = 0.0;
  }


  /*reconect on Mqtt loss*/
  if (client.state() < 0)
  {
#ifdef DEBUG
    Serial.println("No MQTT Connection, reconnect");
#endif

    delay (3000);
    setup();
  }

  /*wait, time base for checking the measurements*/
  delay(1000);


}
