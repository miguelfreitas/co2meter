//
// A simple server implementation showing how to:
//  * serve static messages
//  * read GET and POST parameters
//  * handle missing pages / 404s
//

#include <Arduino.h>
#ifdef ESP32
#include <WiFi.h>
#include <AsyncTCP.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#endif
#include <FS.h>
#include "SPIFFS.h"
#include <ESPAsyncWebServer.h>
#include <HardwareSerial.h>
#include "MHZ19.h"
#include <EEPROM.h>

AsyncWebServer server(80);
MHZ19 myMHZ19;
//HardwareSerial Serial2(1);                             // ESP32 Example

const char* ssid = "co2meter";
const char* password = "meterco2";

#define RX_PIN 16
#define TX_PIN 17 
#define BAUDRATE 9600
#define EEPROM_SIZE 512
#define CO2_MAX_VALS ((EEPROM_SIZE-2)/2)

unsigned long getDataTimer = 0;

int co2_head;
int co2_tail;

void init_co2_storage()
{
    EEPROM.begin(EEPROM_SIZE);
    co2_head = EEPROM.read(EEPROM_SIZE - 2);
    co2_tail = EEPROM.read(EEPROM_SIZE - 1);
}

void save_co2_pointers_commit()
{
    EEPROM.write(EEPROM_SIZE - 2, co2_head);
    EEPROM.write(EEPROM_SIZE - 1, co2_tail);
    EEPROM.commit();
}

void store_co2_val(uint16_t val)
{
    EEPROM.write(co2_head*2 + 0, (uint8_t) (val & 0xff));
    EEPROM.write(co2_head*2 + 1, (uint8_t) (val >> 8));
    co2_head = (co2_head+1) % CO2_MAX_VALS;
    if (co2_head == co2_tail) {
        co2_tail = (co2_tail+1) % CO2_MAX_VALS;
    }
    save_co2_pointers_commit();
}

uint16_t read_co2_val(int pos)
{
    uint16_t val;
    int ptr = (pos + co2_tail) % CO2_MAX_VALS;
    val  = EEPROM.read(ptr*2 + 0);
    val |= EEPROM.read(ptr*2 + 1) << 8;
    return val;
}

uint8_t co2_list_size()
{
    if (co2_head >= co2_tail) {
        return co2_head - co2_tail;
    } else {
        return CO2_MAX_VALS - co2_tail + co2_head;
    }
}

void reset_co2_list()
{
    co2_head = co2_tail = 0;
    save_co2_pointers_commit();
}


void notFound(AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
}

void setup() {

    Serial.begin(115200);
    //Serial.begin(9600);

    init_co2_storage();

    // salva um zero para marcar o boot
    store_co2_val(0x00);

    if(SPIFFS.begin()) {
        Serial.println("SPIFFS Initialize....ok");
    } else {
        Serial.println("SPIFFS Initialization...failed");
    }

    Serial2.begin(BAUDRATE, SERIAL_8N1, RX_PIN, TX_PIN);   // ESP32 Example
    myMHZ19.begin(Serial2);                // *Important, Pass your Stream reference here
    myMHZ19.autoCalibration(false);         // Turn auto calibration OFF

    /*
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    if (WiFi.waitForConnectResult() != WL_CONNECTED) {
        Serial.printf("WiFi Failed!\n");
        return;
    }
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    */

    // Connect to Wi-Fi network with SSID and password
    Serial.print("Setting AP (Access Point)…");
    // Remove the password parameter, if you want the AP (Access Point) to be open
    WiFi.softAP(ssid, password);
    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(IP);

    server.serveStatic("/canvaschart.css", SPIFFS, "/canvaschart.css");
    server.serveStatic("/canvaschartpainter.js", SPIFFS, "/canvaschartpainter.js");
    server.serveStatic("/chart.js", SPIFFS, "/chart.js");
    server.serveStatic("/excanvas.js", SPIFFS, "/excanvas.js");
    server.serveStatic("/jquery.min.js", SPIFFS, "/jquery.min.js");
    server.serveStatic("/multimetro.html", SPIFFS, "/multimetro.html");

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncResponseStream *response = request->beginResponseStream("text/html");

        response->print(
        "<!DOCTYPE html><html><head><title>co2 meter</title>"
          "<script LANGUAGE=\"JavaScript\">"
          "   function getURL_setDateTime() {"
          "       var n = new Date();"
          "       var url = \"/mark?val=\" + (10000 + n.getHours() * 100 + n.getMinutes());"
          "       return url;"
          "   }"
          "   function goURL_setDateTime() {window.location = getURL_setDateTime();}"
          "</script>"
        "</head><body>"
        "<h2>Hello CO2 ");
        
        response->printf("%d", myMHZ19.getCO2(true, true));

        response->print(
        " ppm</h2>"
        "<p><a href=\"/data\">co2 data</a></p>"
        "<br/><br/>"
        "<p><a href=\"/multimetro.html\">co2 graph</a></p>"
        "<br/><br/>"
        "<p><button onclick=\"goURL_setDateTime();\">Mark time</button></p>"
        "<br/><br/>"
        "<hr/>"
        "<br/><br/>"
        "<p><a href=\"/calibrate\">calibrate</a></p>"
        "<br/><br/>"
        "<p><a href=\"/reset_list\">reset list</a></p>"
        "<br/><br/>"
        "</body></html>"
        );
        request->send(response);
    });

    server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request){
        int i;

        AsyncResponseStream *response = request->beginResponseStream("text/plain");
        
        for(i=0;i<co2_list_size();i++) {
            response->printf("%d,", read_co2_val(i));
        }
        response->print("\n");
        request->send(response);
    });

    server.on("/multimetroData", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncResponseStream *response = request->beginResponseStream("text/js");
        int i;
        int miny = 5000;
        int maxy = 0;
        int hlabels=10;
        int vlabels=11;
        int time_idx = -1, time_val = 0;
        int lastval;

        response->print("data={");

        if(co2_list_size()) {
            response->printf("\"label\":[\"CO2 %d ppm\"],\n", read_co2_val(co2_list_size()-1));
        } else {
            response->print("\"label\":[\"No data\"],\n");
        }
        response->print("\"color\":[\"red\"],\n");

        for(i=0;i<co2_list_size();i++) {
            int val = read_co2_val(i);
            if (val > 10000) {
                time_idx = i;
                time_val = val - 10000;
            }
            if (val < 10000 && val > maxy ) {
                maxy = val;
            }
            if (val >= 400 && val < miny ) {
                miny = val;
            }
        }
        response->printf("\"miny\":[\"%d\"],\n", miny);
        response->printf("\"maxy\":[\"%d\"],\n", maxy);

        response->print("\"ascandata\":[");

        lastval = miny;
        for(i=0;i<co2_list_size();i++) {
            int val = read_co2_val(i);
            // clipa zeros and time marks for graph
            if (val > maxy || val < miny) {
                val = lastval;
            }
            response->printf("%d,", val);
            lastval = val;
        }
        response->print("],");

        response->printf("\"gridx\":[\"%d\"],\n", hlabels);
        response->printf("\"gridy\":[\"%d\"],\n", vlabels);

        response->print("\"hlabels\":[");
        for(i=0;i<hlabels;i++) {
            if( time_idx >= 0 ) {
                int ref_hour = time_val / 100;
                int ref_min = time_val % 100;
                int cur_min = (i * (co2_list_size())/(hlabels-1) - time_idx) + ref_min;
                int cur_hour = ref_hour;
                while( cur_min < 0 ) {
                    cur_min += 60;
                    cur_hour -= 1;
                }
                cur_hour += cur_min / 60;
                while( cur_hour < 0 ) {
                    cur_hour += 24;
                }
                response->printf("\"%02d:%02d\",", cur_hour % 24, cur_min % 60);
            } else {
                response->printf("%d,", i * (co2_list_size())/(hlabels-1) );
            }
        }
        response->print("],");

        response->print("\"vlabels\":[");
        for(i=0;i<vlabels;i++) {
            response->printf("%d,", i * (miny-maxy)/(vlabels-1) + maxy );
        }
        response->print("],");

        response->print("}\n");
        request->send(response);
    });

    // Send a GET request to <IP>/mark?val=xxxxx
    server.on("/mark", HTTP_GET, [](AsyncWebServerRequest *request){
        String message;
        int val;
        if (request->hasParam("val")) {
            message = request->getParam("val")->value();
        } else {
            message = "10000";
        }
        val = message.toInt();

        store_co2_val(val);
        request->send(200, "text/plain", message );
    });

    server.on("/calibrate", HTTP_GET, [](AsyncWebServerRequest *request){
        myMHZ19.calibrateZero();
        request->send(200, "text/plain",
        "Calibrate"
        );
    });

    server.on("/reset_list", HTTP_GET, [](AsyncWebServerRequest *request){
        reset_co2_list();
        request->send(200, "text/plain",
        "Reset list"
        );
    });


    /*
    // Send a GET request to <IP>/get?message=<message>
    server.on("/get", HTTP_GET, [] (AsyncWebServerRequest *request) {
        String message;
        if (request->hasParam(PARAM_MESSAGE)) {
            message = request->getParam(PARAM_MESSAGE)->value();
        } else {
            message = "No message sent";
        }
        request->send(200, "text/plain", "Re");
    });
    */

    server.onNotFound(notFound);

    server.begin();
}

void loop() {
    if (millis() - getDataTimer >= 60000) 
    {

        Serial.println("------------------");

        /* get sensor readings as signed integer */        
        int16_t CO2Unlimited = myMHZ19.getCO2(true, true);
        int16_t CO2limited = myMHZ19.getCO2(false, true);

        if(myMHZ19.errorCode != RESULT_OK)
            Serial.println("Error found in communication ");

        else
        {
            Serial.print("CO2 PPM Unlim: ");
            Serial.println(CO2Unlimited);

            Serial.print("CO2 PPM Lim: "); 
            Serial.println(CO2limited);

            /* Command 134 is limited by background CO2 and your defined range. These thresholds can provide a software alarm */

            if (CO2Unlimited - CO2limited >= 10 || CO2Unlimited - CO2limited <= -10)      // Check if CO2 reading difference is greater less than 10ppm
            {  
                Serial.print("Alert! CO2 out of range ");
                Serial.print(CO2limited);
                Serial.println(" threshold passed");   
             /* Sanity check vs Raw CO2 (has Span/Zero failed) or straight to your Alarm code */
           } else {
                store_co2_val(CO2Unlimited);
           }
        }
        getDataTimer = millis(); // Update interval
    }
}