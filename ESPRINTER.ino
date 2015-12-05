#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <FS.h>
#include <ESP8266SSDP.h>
#include "PooledStrings.cpp"

#define BUTTON_PIN -1
#define MAX_WIFI_FAIL 50

char ssid[32], pass[64], webhostname[64];
MDNSResponder mdns;
ESP8266WebServer server(80);
WiFiServer tcp(23);
WiFiClient tcpclient;
String lastResponse;
String serialData;
String fileUploading = "";
String lastUploadedFile = "";

void setup() {
  Serial.begin(115200);
  delay(20);
  EEPROM.begin(512);
  delay(20);
  
#if (BUTTON_PIN != -1)
  pinMode(BUTTON_PIN, INPUT);
  if (digitalRead(button_pin) == 0) { // Clear wifi config
    Serial.println("M117 WIFI ERASE");
    EEPROM.put(0, FPSTR(STR_EEPROM_DUMMY));
    EEPROM.put(32, FPSTR(STR_EEPROM_DUMMY));
    EEPROM.commit();
  }
#endif

  EEPROM.get(0, ssid);
  EEPROM.get(32, pass);
  EEPROM.get(32+64, webhostname);

  uint8_t failcount = 0;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    failcount++;
    if (failcount % 2 == 0) {
      Serial.println("M117 WAIT WIFI " + String(MAX_WIFI_FAIL/2 - (failcount/2)));
    }
    
    if (failcount > MAX_WIFI_FAIL) { // 1 min
      Serial.println("M117 WIFI ERROR");
      WiFi.mode(WIFI_STA);
      WiFi.disconnect();
      delay(100);

      uint8_t num_ssids = WiFi.scanNetworks();
      // TODO: NONE? OTHER?
      String wifiConfigHtml = F("<html><body><h1>Select your WiFi network:</h1><br /><form method=\"POST\">");
      for (uint8_t i = 0; i < num_ssids; i++) {
         wifiConfigHtml += "<input type=\"radio\" id=\"" + WiFi.SSID(i) + "\"name=\"ssid\" value=\"" + WiFi.SSID(i) + "\" /><label for=\"" + WiFi.SSID(i) + "\">" + WiFi.SSID(i) + "</label><br />";
      }
      wifiConfigHtml += F("<input type=\"password\" name=\"password\" /><br />");
      wifiConfigHtml += F("<label for=\"webhostname\">ESPrinter Hostname: </label><input type=\"text\" id=\"webhostname\" name=\"webhostname\" value=\"esprinter\"/><br />");
      wifiConfigHtml += F("<i>(This would allow you to access your printer by name instead of IP address. I.e. http://esprinter.local/)</i>");
      wifiConfigHtml += F("<input type=\"submit\" value=\"Save and reboot\" /></form></body></html>");

      Serial.println("M117 FOUND " + String(num_ssids) + " WIFI");

      delay(5000);
      DNSServer dns;
      IPAddress apIP(192, 168, 1, 1);
      WiFi.mode(WIFI_AP);
      WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
      WiFi.softAP("ESPrinter");
      Serial.println("M117 WiFi -> ESPrinter");
      dns.setErrorReplyCode(DNSReplyCode::NoError);
      dns.start(53, "*", apIP);

      server.on("/", HTTP_GET, [&wifiConfigHtml]() {
        server.send(200, FPSTR(STR_MIME_TEXT_HTML), wifiConfigHtml);
      });

      server.on("/", HTTP_POST, []() {
        if (server.args() <= 0) {
          server.send(500, FPSTR(STR_MIME_TEXT_PLAIN), F("Got no data, go back and retry"));
          return;
        }
        for (uint8_t e = 0; e < server.args(); e++) {
          String argument = server.arg(e);
          urldecode(argument);
          if (server.argName(e) == "password") argument.toCharArray(pass, 64);//pass = server.arg(e);
          else if (server.argName(e) == "ssid") argument.toCharArray(ssid, 32);//ssid = server.arg(e);
          else if (server.argName(e) == "webhostname") argument.toCharArray(webhostname, 64);//ssid = server.arg(e);
        }
        
        EEPROM.put(0, ssid);
        EEPROM.put(32, pass);
        EEPROM.put(32+64, webhostname);
        EEPROM.commit();
        server.send(200, FPSTR(STR_MIME_TEXT_HTML), F("<h1>All set!</h1><br /><p>(Please reboot me.)</p>"));
        Serial.println("M117 SSID: " + String(ssid) + ", PASS: " + String(pass));
        delay(50);
        ESP.restart();
      });
      server.begin();
      Serial.print(FPSTR(STR_M117));
      Serial.println(WiFi.softAPIP().toString());
      for (;;) { // THIS ONE IS FOR WIFI AP SETTINGS PAGE
        server.handleClient();
        dns.processNextRequest();
        delay(1);
      }
    }
  }

  if (mdns.begin(webhostname, WiFi.localIP())) {
    MDNS.addService("http", "tcp", 80);
  }
    
  SSDP.setSchemaURL("description.xml");
  SSDP.setHTTPPort(80);
  SSDP.setName(webhostname);
  SSDP.setSerialNumber(WiFi.macAddress());
  SSDP.setURL("index.html");
  SSDP.begin();
    
  SPIFFS.begin();

  server.onNotFound(fsHandler);
  server.on("/rr_connect", handleConnect);
  server.on("/rr_disconnect", handleDisconnect);
  server.on("/rr_status", handleStatus);
  server.on("/rr_reply", handleReply);
  server.on("/rr_files", handleFiles);
  server.on("/rr_gcode", handleGcode);
  server.on("/rr_config", handleConfig);
  server.on("/rr_upload_begin", handleUploadStart);
  server.on("/rr_upload", handleUploadData);
  server.on("/rr_upload_end", handleUploadEnd);
  server.on("/rr_upload_cancel", handleUploadCancel);
  server.on("/rr_delete", handleDelete);
  server.on("/rr_fileinfo", handleFileinfo);
  server.on("/rr_mkdir", handleMkdir);
  server.on("/description.xml", HTTP_GET, [](){SSDP.schema(server.client());});

  Serial.print(FPSTR(STR_M117));
  Serial.println(WiFi.localIP().toString());

  // UNSUPPORTED STUFF
  server.on("/rr_move", handleUnsupported);
  server.begin();
  tcp.begin();
  tcp.setNoDelay(true);
}

void loop() {
  server.handleClient();
  delay(1);

  while (Serial.available() > 0) {
    char character = Serial.read();
    if (character == '\n' || character == '\r') {
      if (serialData.startsWith(FPSTR(STR_OK))) {
          serialData = "";
          continue;
      }
      tcpclient.write(serialData.c_str(), strlen(serialData.c_str()));
      tcpclient.flush();
      delay(1);
      lastResponse = String(serialData);
      serialData = "";
    } else {
      serialData.concat(character);
    }
  }

  // DISCONNECT ALL IF SOMEONE IS ALLREADY CONNECTED
  if (tcp.hasClient()) {
      if (tcpclient && tcpclient.connected()) {
          WiFiClient otherGuy = tcp.available();
          otherGuy.stop();
      } else {
          tcpclient = tcp.available();
      }
  }

  // PUSH FRESH DATA FROM TELNET TO SERIAL
  if (tcpclient && tcpclient.connected()) {
    while (tcpclient.available()) {
      uint8_t data = tcpclient.read();
      tcpclient.write(data); // ECHO BACK TO SEE WHATCHA TYPIN
      Serial.write(data);
    }
  }
}




void fsHandler() {
  String path = server.uri();
  if (path.endsWith("/")) path += F("index.html");
  File dataFile = SPIFFS.open(path, "r");
  if (!dataFile) {
    server.send(404, FPSTR(STR_MIME_APPLICATION_JSON), "{\"err\": \"404: " + server.uri() + " NOT FOUND\"}");
    return;
  }
  server.sendHeader(FPSTR(STR_CONTENT_LENGTH), String(dataFile.size()));
  String dataType = FPSTR(STR_MIME_TEXT_PLAIN);
  //if (path.endsWith(".gz")) server.sendHeader(F("Content-Encoding"), "gzip");
  if (path.endsWith(".html")) dataType = FPSTR(STR_MIME_TEXT_HTML);
  else if (path.endsWith(".css")) dataType = F("text/css");
  else if (path.endsWith(".js")) dataType = F("application/javascript");
  else if (path.endsWith(".js.gz")) dataType = F("application/javascript");
  else if (path.endsWith(".css.gz")) dataType = F("text/css");
  else if (path.endsWith(".gz")) dataType = F("application/x-gzip");
  server.streamFile(dataFile, dataType);
  dataFile.close();
}






void handleConnect() {
  // ALL PASSWORDS ARE VALID! YAY!
  // TODO: NO, SERIOUSLY, CONSIDER ADDING AUTH HERE. LATER MB?
  server.send(200, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_0));
}

void handleDisconnect() {
  // TODO: DEAUTH?..
  server.send(200, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_0));
}

void handleStatus() {
  String type = (server.args() < 1) ? "1" : server.arg(0);
  Serial.print(FPSTR(STR_M408_S));
  Serial.println(type);
  Serial.setTimeout(5000); // 2s
  serialData = Serial.readStringUntil('\n');
  if (serialData.startsWith(FPSTR(STR_OK))) serialData = Serial.readStringUntil('\n');
  lastResponse = String(serialData);
  server.send(200, FPSTR(STR_MIME_APPLICATION_JSON), lastResponse);
}

void handleReply() {
  server.send(200, FPSTR(STR_MIME_TEXT_PLAIN), lastResponse);
}

void handleFiles() {
  String dir = "";
  if (server.args() > 0) {
    dir = server.arg(0);
  }
  urldecode(dir);
  Serial.print(FPSTR(STR_M20_S2_P));
  Serial.println(dir);
  Serial.setTimeout(5000);
  serialData = Serial.readStringUntil('\n');
  if (serialData.startsWith(FPSTR(STR_OK))) serialData = Serial.readStringUntil('\n');
  lastResponse = String(serialData);
  server.send(200, FPSTR(STR_MIME_APPLICATION_JSON), lastResponse);
}

void handleGcode() {
  String gcode = "";
  if (server.args() > 0) {
    gcode = server.arg(0);
  } else {
    server.send(500, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_500_EMPTY_COMMAND));
    return;
  }
  urldecode(gcode);
  Serial.println(gcode);
  server.send(200, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_BUFF_16));
}

void handleConfig() {
  Serial.print(FPSTR(STR_M408_S));
  Serial.println('5');
  Serial.setTimeout(5000);
  serialData = Serial.readStringUntil('\n');
  if (serialData.startsWith(FPSTR(STR_OK))) serialData = Serial.readStringUntil('\n');
  lastResponse = String(serialData);
  server.send(200, FPSTR(STR_MIME_APPLICATION_JSON), lastResponse);
}

void handleUploadStart() {
  if (server.args() > 0) {
    fileUploading = server.arg(0);
  } else {
    server.send(500, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_500_NO_FILENAME_PROVIDED));
    return;
  }
  bool compat = false;
  if (server.args() > 1) {
    compat = (server.arg(1) == FPSTR(STR_TRUE));
  }
  urldecode(fileUploading);
  lastUploadedFile = fileUploading;
  // TODO: CHECK FOR VALID SERVER RESPONSE!!!! IMPORTANT!
  if (!compat) {
    Serial.println(FPSTR(STR_M575_P1_B460800_S0)); // CHANGE BAUDRATE ON 3DPRINTER
    Serial.flush();
    delay(200);
    Serial.end();
    delay(200);
    Serial.begin(460800);
    delay(200);
    Serial.flush();
  }
  Serial.print(FPSTR(STR_M28));
  Serial.println(fileUploading);
  Serial.flush();
  server.send(200, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_0));
}

void handleUploadData() {
  if (fileUploading == "") {
    server.send(500, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_500_NOT_UPLOADING_FILES));
    return;
  }
  String data = "";
  if (server.args() > 0) {
    data = server.arg(0);
  } else {
    server.send(500, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_500_NO_DATA_RECEIVED));
    return;
  }
  urldecode(data);
  Serial.println(data);
  Serial.flush();
  server.send(200, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_0));
}

void handleUploadEnd() {
  if (fileUploading == "") {
    server.send(500, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_500_NOT_UPLOADING_FILES));
    return;
  }
  bool compat = false;
  if (server.args() > 0) {
    compat = (server.arg(0) == FPSTR(STR_TRUE));
  }
  Serial.print(FPSTR(STR_M29));
  Serial.println(fileUploading);
  if (!compat) {
    Serial.println(FPSTR(STR_M575_P1_B115200_S0)); // CHANGE BAUDRATE ON 3DPRINTER
    Serial.flush();
    delay(200);
    Serial.end();
    delay(200);
    Serial.begin(115200);
    delay(200);
    Serial.flush();
  }
  fileUploading = "";
  server.send(200, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_0));
}

void handleUploadCancel() {
  // IS SENT AFTER UPLOAD END
  Serial.print(FPSTR(STR_M30));
  Serial.println(lastUploadedFile);
  server.send(200, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_0));
}

void handleDelete() {
  String fileName = "";
  if (server.args() > 0) {
    fileName = server.arg(0);
  } else {
    server.send(500, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_500_NO_FILENAME_PROVIDED));
    return;
  }
  urldecode(fileName);
  Serial.print(FPSTR(STR_M30));
  Serial.println(fileName);
  server.send(200, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_0));
}

void handleFileinfo() {
  String fileName = "";
  if (server.args() > 0) {
    fileName = server.arg(0);
    urldecode(fileName);
  }
  Serial.print(FPSTR(STR_M36));
  if (fileName == "") {
    Serial.println();
  } else {
    Serial.println(fileName);
  }
  Serial.setTimeout(5000);
  serialData = Serial.readStringUntil('\n');
  if (serialData.startsWith(FPSTR(STR_OK))) serialData = Serial.readStringUntil('\n');
  lastResponse = String(serialData);
  server.send(200, FPSTR(STR_MIME_APPLICATION_JSON), lastResponse);
}

void handleMkdir() {
  String dirName = "";
  if (server.args() < 2 || server.arg(1) != FPSTR(STR_TRUE)) { // 2 ARGS FOR COMPATMODE OR NOPE
    server.send(200, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_UNSUPPORTED_OPERATION));
    return;
  }
  dirName = server.arg(0);
  urldecode(dirName);
  if (dirName == "") {
    server.send(500, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_500_NO_DIR_NAME_PROVIDED));
    return;
  }
  Serial.print(FPSTR(STR_M32));
  Serial.println(dirName);
  server.send(200, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_0));
}





void handleUnsupported() {
  server.send(200, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_UNSUPPORTED_OPERATION));
}

void urldecode(String &input) { // LAL ^_^
  input.replace("%0A", String('\n'));
  input.replace("%20", " ");
  input.replace("+", " ");
  input.replace("%21", "!");
  input.replace("%22", "\"");
  input.replace("%23", "#");
  input.replace("%24", "$");
  input.replace("%25", "%");
  input.replace("%26", "&");
  input.replace("%27", "\'");
  input.replace("%28", "(");
  input.replace("%29", ")");
  input.replace("%30", "*");
  input.replace("%31", "+");
  input.replace("%2C", ",");
  input.replace("%2E", ".");
  input.replace("%2F", "/");
  input.replace("%2C", ",");
  input.replace("%3A", ":");
  input.replace("%3A", ";");
  input.replace("%3C", "<");
  input.replace("%3D", "=");
  input.replace("%3E", ">");
  input.replace("%3F", "?");
  input.replace("%40", "@");
  input.replace("%5B", "[");
  input.replace("%5C", "\\");
  input.replace("%5D", "]");
  input.replace("%5E", "^");
  input.replace("%5F", "-");
  input.replace("%60", "`");
}
