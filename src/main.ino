#include <string.h>
#include <LittleFS.h>

#include <NTPClient.h>
#include <WiFiUdp.h>

#include <ESP8266WiFi.h>

#include <ESPAsyncWebSrv.h>
#include <ESPAsyncTCP.h>

#include <MFRC522.h> // biblioteca responsável pela comunicação com o módulo RFID-RC522
#include <MFRC522Extended.h> // biblioteca responsável pela comunicação com o módulo RFID-RC522
#include <SPI.h> // biblioteca para comunicação do barramento SPI
#include <Adafruit_GFX.h> // Core graphics library
#include <Adafruit_ST7735.h> // Hardware-specific library for ST7735

#define MASTERCARD "0935D9AC"
#define RESETCARD "6375457A"

AsyncWebServer server(80);

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);

// Configuracao do RFID
constexpr uint8_t RFID_RST_PIN = D3;
// constexpr uint8_t RFID_SDA_PIN = D4;
constexpr uint8_t RFID_SDA_PIN = D0; // SDA do RFID posto em D0 no lugar do D4 para usar o led da placa que fica no GPIO2

// Configuracao do display
constexpr uint8_t DISPLAY_CS = D8;
constexpr uint8_t DISPLAY_DC = D2; // A0/DC;
constexpr uint8_t DISPLAY_RST = D1;
constexpr uint8_t DISPLAY_SDA = D7; // sda/mosi;
constexpr uint8_t DISPLAY_SCK = D5;

constexpr uint8_t SD_CARD_CS = D4;

// Instancia do display
Adafruit_ST7735 display = Adafruit_ST7735(DISPLAY_CS,  DISPLAY_DC, DISPLAY_RST); 

// Instancia do RFID
MFRC522 rfid(RFID_SDA_PIN, RFID_RST_PIN);
MFRC522::MIFARE_Key key;

// Linha atual para escrita no display
bool sdCardSuccess = 0;
int currentDisplayLine = 0;
int configReset = 0;
String configWiFiName = "DoorAccessAP";
String configWiFiPassword = "AccessPointConfig";
String configWiFiName2 = "DoorAccessAP";
String configWiFiPassword2 = "AccessPointConfig";

unsigned long previousMillis = 0;
bool willRestart = 0;

// RFID callback
void (*rfidReadCallback)(String) = NULL;
struct RFIDCallbackContext {
  unsigned long ms;
  String rfid;
  String name;
  String role;
  String session;
  String req_login;
  short accesslvl;
} rfidReadContext;

void setup() {
  Serial.begin(9600);

  SPI.begin();

  pinMode(SD_CARD_CS, OUTPUT);
  digitalWrite(SD_CARD_CS, LOW);

 // Inicia display
  displayInit();

  // Inicia rfid
  rfid.PCD_Init();

  printHeader();
  
  sdCardSuccess = LittleFS.begin();
  //sdCardSuccess = SD.begin(SD_CARD_CS);

  getConfig();
  
  printSubHeader();
  printMessageDefault();

  clearRfidReadCallback();

  server.on("/dooraccess", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("ENDPOINT - NETWORK DISCOVERY");
    request->send(200, "text/plain", "NodeMCU Door Access API");
  });

  if (configReset == 1) {
    WiFi.disconnect();
    WiFi.softAP(configWiFiName2, configWiFiPassword2);

    server.on("/ssid", HTTP_GET, [](AsyncWebServerRequest *request) {
      Serial.println("ENDPOINT - WIFI SETUP");
      AsyncWebParameter* ssid;
      AsyncWebParameter* pass;

      if (request->hasParam("ssid")) {
        ssid = request->getParam("ssid");
      }

      if (request->hasParam("pass")) {
        pass = request->getParam("pass");
      }

      if (ssid && pass) {
        Serial.println("VALID WIFI SETUP");
        saveConfig("wifi_reset", "2");
        saveConfig("wifi_nome", ssid->value());
        saveConfig("wifi_senha", pass->value());
        request->send(204);

        Serial.println("Salvo com sucesso");
        clearMessageInFiveSeconds(true);
      } else {
        Serial.println("INVALID WIFI SETUP");
        request->send(400);
      }
    });

  } else {
    if (configReset == 2) {
      WiFi.begin(configWiFiName, configWiFiPassword);
    } else {
      WiFi.begin();
    }

    int8_t res = WiFi.waitForConnectResult();
    if (res == WL_CONNECTED) {
      Serial.println(WiFi.localIP());
      if (configReset == 2) {
        saveConfig("wifi_reset", "0");
        configReset = 0;
      }
    } else {
      saveConfig("wifi_reset", "1");
      ESP.restart();
      return;
    }

    timeClient.begin();
    timeClient.setTimeOffset(-3);

    server.on("/login", HTTP_GET, [](AsyncWebServerRequest *request) {
      Serial.println("ENDPOINT - LOGIN");
      if (checkRfidReadCallback()) {
        request->send(409);
        return;
      }

      rfidReadContext.ms = millis();
      rfidReadCallback = doLogin;
      request->send(204);
    });

    server.on("/login_rfid", HTTP_GET, [](AsyncWebServerRequest *request) {
      Serial.println("ENDPOINT - LOGIN RFID");
      
      if (!rfidReadContext.ms) {
        request->send(400);
      } else if (rfidReadContext.rfid.equals("")) {
        request->send(204);
      } else {
        String line = "";
        line.concat(rfidReadContext.rfid);
        line.concat(",");
        line.concat(char(rfidReadContext.accesslvl + '0'));
        line.concat(",");
        line.concat(rfidReadContext.name);
        line.concat(",");
        line.concat(rfidReadContext.role);
        line.concat(",");
        line.concat(rfidReadContext.session);

        Serial.println("LOGIN - USER TO CSV");

        request->send(200, "text/csv", line);
        clearRfidReadCallback();
      }
    });

    server.on("/logs", HTTP_GET, [](AsyncWebServerRequest *request) {
      Serial.println("ENDPOINT - LOGS");
      if (!authenticateUser(request)) {
        request->send(403);
        return;
      }

      request->send(LittleFS, "log.csv", "text/csv");
    });

    server.on("/users", HTTP_GET, [](AsyncWebServerRequest *request) {
      Serial.println("ENDPOINT - USERS");
      if (!authenticateUser(request)) {
        request->send(403);
        return;
      }

      request->send(LittleFS, "lista.csv", "text/csv");
    });

    server.on("/user_regedit", HTTP_GET, [](AsyncWebServerRequest *request) {
      Serial.println("ENDPOINT - USER REGISTER/EDIT");
      if (!authenticateUser(request)) {
        request->send(403);
        return;
      }

      String rfid = "";
      String name = "";
      String role = "";
      short accesslvl = 1;
      bool create = false;

      if (request->hasParam("rfid")) {
        AsyncWebParameter* param = request->getParam("rfid");
        rfid = param->value();
      } else {
        create = true;
      }
      
      if (request->hasParam("name")) {
        AsyncWebParameter* param = request->getParam("name");
        name = param->value();
      }

      if (request->hasParam("role")) {
        AsyncWebParameter* param = request->getParam("role");
        role = param->value();
      }

      if (request->hasParam("mod")) {
        AsyncWebParameter* param = request->getParam("mod");
        String value = param->value();
        if (value.equals("1")) {
          accesslvl = 2;
        }
      }

      if (name.equals("") || role.equals("")) {
        request->send(400);
        return;
      }
      
      if (create) {
        clearRfidReadCallback();

        rfidReadContext.name = name;
        rfidReadContext.role = role;
        rfidReadContext.accesslvl = accesslvl;
        rfidReadContext.ms = millis();
        rfidReadContext.req_login = getLoginUser(request);
        rfidReadCallback = createRfidUser;
        request->send(204);
      } else {
        String adm = getLoginUser(request);

        String session = "";
        String dummy = "";
        uint8_t oldaccesslvl = getUser(rfid, &dummy, &dummy, &session);
        if (oldaccesslvl == 3) {
          request->send(501);
          return;
        }
        
        saveLog(rfid, accesslvl + 10, adm);
        saveUser(rfid, name, role, session, accesslvl);
        request->send(204);
      }
    });

    server.on("/user_regedit_rfid", HTTP_GET, [](AsyncWebServerRequest *request) {
      Serial.println("ENDPOINT - USER REGISTER/EDIT RFID");
      
      if (!rfidReadContext.ms) {
        request->send(400);
      } else if (rfidReadContext.rfid.equals("")) {
        request->send(204);
      } else {
        String line = "";
        line.concat(rfidReadContext.rfid);
        line.concat(",");
        line.concat(char(rfidReadContext.accesslvl + '0'));
        line.concat(",");
        line.concat(rfidReadContext.name);
        line.concat(",");
        line.concat(rfidReadContext.role);
        line.concat(",");
        line.concat(rfidReadContext.session);

        Serial.println("REGISTER - USER TO CSV");

        request->send(200, "text/csv", line);
        clearRfidReadCallback();
      }
    });

    server.on("/user_del", HTTP_GET, [](AsyncWebServerRequest *request) {
      Serial.println("ENDPOINT - USER DELL");
      if (!authenticateUser(request)) {
        request->send(403);
        return;
      }

      AsyncWebParameter* rfid;

      if (request->hasParam("rfid")) {
        rfid = request->getParam("rfid");
      }

      if (rfid) {
        String value = rfid->value();
        String adm = getLoginUser(request);
        saveLog(value, 10, adm);
        
        String name = "";
        String role = "";
        String session = "";

        uint8_t accesslvl = getUser(value, &name, &role, &session);
        if (accesslvl == 3) {
          request->send(501);
          return;
        }

        saveUser(value, name, role, session, 0);

        request->send(204);
      } else {
        request->send(400);
      }
    });
  }

  server.begin();
}

String getLoginUser(AsyncWebServerRequest *request) {
  Serial.println("GET LOGIN USER");

  if (!request->hasHeader("X-RFID")) {
    Serial.println("NO X-RFID HEADER");
    return "";
  }

  AsyncWebHeader *header = request->getHeader("X-RFID");
  String rfid = header->value();
  Serial.println(rfid);
  return rfid;
}

bool authenticateUser(AsyncWebServerRequest *request) {
  Serial.println("AUTHENTICATE");

  if (!request->hasHeader("X-RFID")) {
    Serial.println("NO X-RFID HEADER");
    return false;
  }

  AsyncWebHeader *header = request->getHeader("X-RFID");
  String rfid = header->value();
  String session = "";
  String dummy = "";

  uint8_t accesslvl = getUser(rfid, &dummy, &dummy, &session);
  if (accesslvl <= 1) {
    Serial.println("LOW ACCESS LEVEL");
    return false;
  }

  unsigned long session_ms = atol(session.c_str());
  unsigned long expires = timeClient.getEpochTime() + 604800;

  if (session_ms == 0) {
    Serial.println("NO SESSION FOUND");
    return false;
  }

  if (session_ms > expires) {
    Serial.println("SESSION EXPIRED");
    return false;
  }

  return request->authenticate(rfid.c_str(), session.c_str());
}

bool checkRfidReadCallback() {
  if (rfidReadContext.ms) {
    return true;
  } else {
    clearRfidReadCallback();
    return false;
  }
}

void clearRfidReadCallback() {
  rfidReadContext.rfid = "";
  rfidReadContext.name = "";
  rfidReadContext.role = "";
  rfidReadContext.session = "";
  rfidReadContext.req_login = "";
  rfidReadContext.accesslvl = 0;
  rfidReadContext.ms = 0;
  rfidReadCallback = NULL;
}

void clearRfidReadCallbackPartial() {
  rfidReadCallback = NULL;
}

void createRfidUser(String rfid) {
  String dummy = "";
  uint8_t oldaccesslvl = getUser(rfid, &dummy, &dummy, &dummy);
  if (oldaccesslvl >= 1) {
    Serial.println("USER ALREADY EXISTS");
    clearRfidReadCallback();
    return;
  }

  saveUser(rfid, rfidReadContext.name, rfidReadContext.role, "0", rfidReadContext.accesslvl);

  if (rfidReadContext.accesslvl == 3) {
    saveLog(rfid, 13, rfid);

    clearMessage();
    displayPrint("Cartao admin registrado.", ST77XX_CYAN);
    clearMessageInFiveSeconds(false);
  } else {
    saveLog(rfid, rfidReadContext.accesslvl + 10, rfidReadContext.req_login);

    clearMessage();
    displayPrint("Cartao registrado.", ST77XX_CYAN);
    clearMessageInFiveSeconds(false);
  }

  rfidReadContext.rfid = rfid;
}

void doLogin(String rfid) {
  Serial.println("LOGIN - CALLBACK FUNCTION");

  String dummy = "";
  rfidReadContext.accesslvl = getUser(rfid, &rfidReadContext.name, &rfidReadContext.role, &dummy);

  if (rfidReadContext.accesslvl <= 1) {
    Serial.println("LOGIN - NOT A MODERATOR");
    clearRfidReadCallback();
    return;
  }

  unsigned long session = timeClient.getEpochTime();
  char sess[50];
  sprintf(sess, "%lu", session);
  saveUserSession(rfid, sess);

  Serial.println("LOGIN - SAVED USER SESSION");
  
  rfidReadContext.session = sess;
  rfidReadContext.rfid.concat(rfid);

  clearMessage();
  displayPrint("Login realizado.", ST77XX_CYAN);
  clearMessageInFiveSeconds(false);
}

void clearMessageInFiveSeconds(bool reset) {
  willRestart = reset;
  previousMillis = millis();
}

void loop() {
  timeClient.update();

  if (isCardPresent()) {
    String tag = getCardId();
    String name = "";
    String role = "";
    String session = "";

    uint8_t accessType = getUser(tag, &name, &role, &session); // tipo de acesso

    if (rfidReadCallback) {
      rfidReadCallback(tag);
      clearRfidReadCallbackPartial();
    } else {
      if (tag.equals(RESETCARD)) {
        saveConfig("wifi_reset", "1");

        LittleFS.remove("lista.csv");
        LittleFS.remove("log.csv");
        ESP.restart();
      }

      if (accessType > 0) {
        digitalWrite(SD_CARD_CS, HIGH);
      }

      printMessageAccess(tag, name, accessType);
      clearMessageInFiveSeconds(false);
      delay(1000);
      digitalWrite(SD_CARD_CS, LOW);
    }
  }

  if (previousMillis) {
    unsigned long currentMillis = millis();

    if ((currentMillis - previousMillis) >= 3000) {
      previousMillis = 0;
      printMessageDefault();
      if (willRestart) {
        ESP.restart();
      }
    }
  }

  if (rfidReadContext.ms) {
    unsigned long currentMillis = millis();

    if ((currentMillis - rfidReadContext.ms) >= 15000) {
      clearRfidReadCallback();
      printMessageDefault();
    }
  }
}

void displayInit() {
  display.initR(INITR_BLACKTAB); // Init ST7735S chip, black tab
  display.setTextColor(ST7735_WHITE);
  display.setRotation(1);
  display.setTextWrap(true);

  Serial.println("Display iniciado.");
}

void printHeader() {
  display.fillScreen(ST77XX_BLACK); // Pinta a tela de preto
  displayPrint("", ST77XX_WHITE);
  displayPrint("Door Access", ST77XX_GREEN);
  displayPrint("", ST77XX_WHITE);
}

void printSubHeader() {
  if (sdCardSuccess) {
    Serial.println("SDCard iniciado.");
    displayPrint("--------------------------", ST77XX_GREEN);
    displayPrint("", ST77XX_WHITE);
  } else {
    Serial.println("Inicialização do SDCard falhou!");
    displayPrint("FALHA: SD CARD", ST77XX_RED);
    displayPrint("--------------------------", ST77XX_RED);
  }
}

void printMessageDefault() {
  clearMessage();

  if (configReset == 1) {
    String wifiName = "WiFi: ";
    String wifiPassword = "Senha: ";
    String wifiIp = "http://192.168.4.1";
    
    wifiName.concat(configWiFiName2);
    wifiPassword.concat(configWiFiPassword2);
    //wifiIp.concat(WiFi.localIP().toString()); // WiFi.localIP()

    displayPrint(wifiName, ST77XX_WHITE);
    displayPrint(wifiPassword, ST77XX_WHITE);
    displayPrint("", ST77XX_WHITE);
    displayPrint(wifiIp, ST77XX_CYAN);
    return;
  } else {
    displayPrint("Aproxime seu cartao", ST77XX_WHITE);
    displayPrint("do leitor...", ST77XX_WHITE);
  }
}

void printMessageAccess(String RFID, String Name, short newAccesslvl) {
  clearMessage();

  if (newAccesslvl == 0) {
    saveLog(RFID, 0, "");

    displayPrint("Entrada recusada", ST77XX_ORANGE);
    Serial.println("Acesso negado:");
    Serial.println(RFID);
  } else {
    saveLog(RFID, 1, "");

    String userName = "Nome: ";
    String spaces = "      ";

    userName.concat(Name.substring(0, 20));
    spaces.concat(Name.substring(20, 40));

    displayPrint("Entrada liberada", ST77XX_GREEN);
    displayPrint("", ST77XX_WHITE);
    displayPrint(userName, ST77XX_WHITE);
    displayPrint(spaces, ST77XX_WHITE);

    Serial.println("Acesso permitido:");
    Serial.println(RFID);
  }
}

void clearMessage() {
  display.fillRect(0, 50, 160, 80, ST7735_BLACK);
  currentDisplayLine = 5;
}

//função para controlar linhas e automatizar impressão no display
void displayPrint(String string, uint16_t color) {
  const char *text = string.c_str();

  if (currentDisplayLine > 12) {
    display.fillScreen(ST77XX_BLACK); //Pinta a tela de preto
    currentDisplayLine = 0;
  }
  
  display.setCursor(0, currentDisplayLine * 10);
  display.setTextColor(color);
  display.print(text);
  
  currentDisplayLine++;
}

String getCardId() {
  String cardId = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    cardId += String(rfid.uid.uidByte[i] < 0x10 ? "0" : ""); // Adiciona o zero se o id é menor que 16 ou seja 00, 01, 02, ..., 0E, 0F;
    cardId += String(rfid.uid.uidByte[i], HEX);
  }
  
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
    
  cardId.toUpperCase();
  
  return cardId;
}

boolean isCardPresent() {
  if (rfid.PICC_IsNewCardPresent()){
    if (rfid.PICC_ReadCardSerial()) {
      return true;
    }
  }
  return false;
}

uint8_t getConfig() {
  String current = ""; // config label buffer
  String currentvalue = ""; // config value buffer

  String path = "config.txt";
  File config = LittleFS.open(path, "r");

  if (config) { // caso for possivel abrir o arquivo
    Serial.println("lendo config.txt");
    
    while (config.available()) {
      current = "";
      currentvalue = "";
      char c = '\0';
      
      // READ KEY
      do {
        c = char(config.read());

        if ((c == ':') || (c == '\n')) {
          break;
        } else {
          current.concat(c);
        }

      } while (config.available());

      if (!config.available()) {
        break;
      }

      if (c == '\n') {
        continue;
      }

      c = '\0';

      // READ VALUE
      do {
        c = char(config.read());

        if (c == '\n') {
          break;
        } else {
          currentvalue.concat(c);
        }

      } while (config.available());

      Serial.println("CONFIG");
      Serial.println(current);
      Serial.println(currentvalue);

      if (current.equals("wifi_reset")) {
        configReset = currentvalue.toInt();
        Serial.println(configReset);
      } else if (current.equals("wifi_nome")) {
        configWiFiName = currentvalue;
      } else if (current.equals("wifi_senha")) {
        configWiFiPassword = currentvalue;
      }
    }

    config.close();
    Serial.println("fechando config.txt");
  } else {
    File create = LittleFS.open(path, "w");
    create.close();
    Serial.println("nenhuma config file");
  }

  return 0;
}

uint8_t saveConfig(String ConfigToSearch, String ValueToSave) {
  String newFile = "";
  String current = ""; // config label buffer
  bool currentFound = 0;

  String path = "config.txt";
  File config = LittleFS.open(path, "r+");

  // caso for possivel abrir o arquivo
  if (config) {
    Serial.println("lendo config.txt");
    config.seek(0);
    
    while (config.available()) {
        Serial.println(current);
        current = "";
        char c = '\0';

        do {
          c = char(config.read());
          newFile.concat(c);

          if ((c == ':') || (c == '\n')) {
            break;
          } else {
            current.concat(c);
          }

        } while (config.available());

        if (current == ConfigToSearch) {
          currentFound = 1;
          Serial.println("SAVING");
          Serial.println(ValueToSave);
          newFile.concat(ValueToSave);
          newFile.concat("\n");
        } else {
          if (c == '\n') {
            continue;
          }
        }

        if (!config.available()) {
          break;
        }

        do {
          c = char(config.read());
          
          if (current != ConfigToSearch) {
            newFile.concat(c);
          }

          if (c == '\n') {
            break;
          }

        } while (config.available());
    }

    if (!currentFound) {
      Serial.println("SAVING NEW");
      Serial.println(ValueToSave);
      newFile.concat(ConfigToSearch);
      newFile.concat(":");
      newFile.concat(ValueToSave);
      newFile.concat("\n");
    }

    config.seek(0);
    config.print(newFile);
    config.truncate(newFile.length());

    config.close();
    Serial.println(newFile);
    Serial.println("fechando config.txt");
  } else {
    Serial.println("nenhuma config file");
  }

  return 0;
}

uint8_t getUser(String RFID) {
  String dummy = "";
  return getUser(RFID, &dummy, &dummy, &dummy);
}

uint8_t getUser(String RFID, String *Name, String *Role, String *Session) {
  uint8_t accesslvl = 0;

  String path = "lista.csv";
  File list = LittleFS.open(path, "r");

  if (list) { // caso for possivel abrir o arquivo
    Serial.println("lendo lista.csv");

    String current = "";
    String buffer = "";
    int userCount = 0;
    int column = 0;

    while (list.available()) {
      char c = char(list.read()); // current character being read

      if (c == '\r') {
        continue;
      }

      if ((c == ',') || (c == '\n')) {
        switch (column) {
          case 0:
            current.concat(buffer);
            Serial.printf("current: %s\n", current);
            break;
          case 1:
            Serial.println(buffer.charAt(0));
            if (current == RFID) {
              accesslvl = buffer.charAt(0) - '0';
            }
            break;
          case 2:
            if (current == RFID) {
              Name->concat(buffer);
            }
            break;
          case 3:
            if (current == RFID) {
              Role->concat(buffer);
            }
            break;
          case 4:
            if (current == RFID) {
              Session->concat(buffer);
            }
            userCount++;
            break;
        }

        buffer = "";
        column++;

        if (c == '\n') {
          current = "";
          column = 0;
        }
      } else {
        buffer.concat(c);
      }
    }

    list.close();
    Serial.println("fechando lista.csv");

    if ((userCount == 0) && (configReset == 0)) {
      clearRfidReadCallback();

      rfidReadContext.accesslvl = 3;
      rfidReadContext.name = "Admin";
      rfidReadContext.role = "admin";
      rfidReadContext.ms = millis();
      rfidReadCallback = createRfidUser;
    }
  } else {
    File create = LittleFS.open(path, "w");
    create.close();
    Serial.println("nao abrimos absolutamente nada");
  }

  return accesslvl;
}

bool saveUserSession(String RFID, String Session) {
  String name = "";
  String role = "";
  String dummy = "";

  uint8_t accesslvl = getUser(RFID, &name, &role, &dummy);
  return saveUser(RFID, name, role, Session, accesslvl);
}

bool saveUser(String RFID, String Name, String Role, String Session, short newAccesslvl) {
  String path = "lista.csv";
  String old_path = "lista_old.csv";

  int success = LittleFS.rename(path, old_path);
  if (!success) {
    Serial.println("não possivel renomear lista.csv");
    return 1;
  }

  File list = LittleFS.open(path, "w");
  File old_list = LittleFS.open(old_path, "r");

  if (list && old_list) { // caso for possivel abrir o arquivo
    Serial.println("gravando lista.csv");

    String current = "";
    String line = "";

    bool isRfid = 1;
    bool found = 0;

    while (old_list.available()) {
      char c = char(old_list.read()); // current character being read

      if (c == '\n') {
        list.print(line);
        list.print("\n");
        isRfid = 1;
        current = "";
        line = "";
        continue;
      }
      
      if (c == ',') {
        isRfid = 0;
      }

      if (RFID.equals(current)) {
        if (found == 0) {
          found = 1;
          line.concat(",");
          line.concat(char(newAccesslvl + '0'));
          line.concat(",");
          line.concat(Name);
          line.concat(",");
          line.concat(Role);
          line.concat(",");
          line.concat(Session);
        }
      } else {
        line.concat(c);
      }

      if (isRfid == 1) {
        current.concat(c);
      }
    }

    if (found == 0) {
      line = "";
      line.concat(RFID);
      line.concat(",");
      line.concat(char(newAccesslvl + '0'));
      line.concat(",");
      line.concat(Name);
      line.concat(",");
      line.concat(Role);
      line.concat(",");
      line.concat(Session);
      line.concat("\n");
      list.print(line);
    }

    old_list.close();
    list.close();
    Serial.println("fechando lista.csv e old_lista.csv");

    int success = LittleFS.remove(old_path);
    if (!success) {
      Serial.println("não possivel deletar old_lista.csv");
      return 1;
    } else {
      Serial.println("deletar old_lista.csv");
    }
    
  } else if (!list) {
    Serial.println("nao abrimos a lista.csv");
    return 1;
  } else if (!old_list) {
    Serial.println("nao abrimos a old_lista.csv");
    return 1;
  } else {
    Serial.println("nao gravamos absolutamente nada");
    return 1;
  }

  return 0;
}

void saveLog(String RFID, short action, String admRFID) {
  // action
  // 0 - acesso negado
  // 1 - acesso permitido
  // 10 - usuário deletado
  // 11 - usuário registrado
  // 12 - moderador registrado
  // 13 - admin registrado
  String path = "log.csv";
  File accesslog = LittleFS.open(path, "a");
    
  if (accesslog) {
    Serial.println("escrevendo log.csv");

    unsigned long time = timeClient.getEpochTime();
    char timestamp[50];
    sprintf(timestamp, "%lu", time);
    
    accesslog.print(timestamp);
    accesslog.print(",");

    bool actionBy = 0;
    
    switch (action) {
      case 0:
        accesslog.print("0");
        break;
      case 1:
        accesslog.print("1");
        break;
      case 10:
        actionBy = 1;
        accesslog.print("10");
        break;
      case 11:
        actionBy = 1;
        accesslog.print("11");
        break;
      case 12:
        actionBy = 1;
        accesslog.print("12");
        break;
      case 13:
        actionBy = 1;
        accesslog.print("13");
        break;
      default:
        accesslog.print("99");
        break;
    }

    String name = "";
    String role = "";
    String dummy = "";
    getUser(RFID, &name, &role, &dummy);

    accesslog.print(",");
    accesslog.print(RFID);
    accesslog.print(",");
    accesslog.print(name);
    accesslog.print(",");
    accesslog.print(role);
    accesslog.print(",");

    if (actionBy) {
      String admName = "";
      getUser(RFID, &admName, &dummy, &dummy);

      accesslog.print(admRFID);
      accesslog.print(",");
      accesslog.print(admName);
    } else {
      accesslog.print(",");
    }
    
    accesslog.print('\n');
    accesslog.close();
    Serial.println("fechando log.csv");
  } else {
    File create = LittleFS.open(path, "w");
    create.close();
    Serial.println("nao foi possivel abrir log.csv");
  }
}