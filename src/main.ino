#include <string.h>
#include <LittleFS.h>

#include <ESP8266WiFi.h>

#include <ESPAsyncWebSrv.h>
#include <ESPAsyncTCP.h>

#include <MFRC522.h> // biblioteca responsável pela comunicação com o módulo RFID-RC522
#include <MFRC522Extended.h> // biblioteca responsável pela comunicação com o módulo RFID-RC522
#include <SPI.h> // biblioteca para comunicação do barramento SPI
#include <Adafruit_GFX.h> // Core graphics library
#include <Adafruit_ST7735.h> // Hardware-specific library for ST7735

#define MASTERCARD "0935D9AC"

AsyncWebServer server(80);

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
  AsyncWebServerRequest *request;
  unsigned long ms;
  String rfid;
  String name;
  String role;
  short accesslvl;
} rfidReadContext;

void setup() {
  Serial.begin(9600);

  SPI.begin();

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

  server.on("/dooraccess", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("ENDPOINT 1");
    request->send(200, "text/plain", "NodeMCU Door Access API");
  });

  if (configReset == 1) {
    WiFi.disconnect();
    WiFi.softAP(configWiFiName2, configWiFiPassword2);

    server.on("/ssid", HTTP_GET, [](AsyncWebServerRequest *request) {
      Serial.println("ENDPOINT 2");
      AsyncWebParameter* ssid;
      AsyncWebParameter* pass;

      if (request->hasParam("ssid")) {
        ssid = request->getParam("ssid");
      }

      if (request->hasParam("pass")) {
        pass = request->getParam("pass");
      }

      if (ssid && pass) {
        Serial.println("ENDPOINT 2A");
        saveConfig("wifi_reset", "2");
        saveConfig("wifi_nome", ssid->value());
        saveConfig("wifi_senha", pass->value());
        request->send(200);

        Serial.println("Salvo com sucesso");
        ESP.restart();
      } else {
        Serial.println("ENDPOINT 2B");
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
      }
    } else {
      saveConfig("wifi_reset", "1");
      ESP.restart();
    }

    server.on("/logs", HTTP_GET, [](AsyncWebServerRequest *request) {
      Serial.println("ENDPOINT 3");
      request->send(LittleFS, "log.csv");
    });

    server.on("/login", HTTP_GET, [](AsyncWebServerRequest *request) {
      Serial.println("ENDPOINT 4");
      clearRfidReadCallback(true);

      rfidReadContext.request = request;
      rfidReadContext.ms = millis();
      rfidReadCallback = doLogin;
    });

    server.on("/users", HTTP_GET, [](AsyncWebServerRequest *request) {
      Serial.println("ENDPOINT 5");
      request->send(LittleFS, "lista.csv");
    });

    server.on("/users", HTTP_POST, [](AsyncWebServerRequest *request) {
      Serial.println("ENDPOINT 6");
      AsyncWebParameter* rfid;
      AsyncWebParameter* name;
      AsyncWebParameter* role;
      short accesslvl = 1;
      bool create = false;

      if (request->hasParam("rfid", true)) {
        rfid = request->getParam("rfid", true);
      } else {
        create = true;
      }
      
      if (request->hasParam("name", true)) {
        name = request->getParam("name", true);
      }

      if (request->hasParam("role", true)) {
        role = request->getParam("role", true);
      }

      if (request->hasParam("mod", true)) {
        accesslvl = 2;
      }

      if (request->hasParam("admin", true)) {
        accesslvl = 3;
      }

      if (!name || !role) {
        request->send(400);
        return;
      }
      
      if (create) {
        clearRfidReadCallback(true);

        rfidReadContext.request = request;
        rfidReadContext.name = name->value();
        rfidReadContext.role = role->value();
        rfidReadContext.accesslvl = accesslvl;
        rfidReadContext.ms = millis();
        rfidReadCallback = createRfidUser;
      } else {
        saveUser(rfid->value(), name->value(), role->value(), accesslvl);
        request->send(200);
      }
    });

    server.on("/users", HTTP_DELETE, [](AsyncWebServerRequest *request) {
      Serial.println("ENDPOINT 7");
      AsyncWebParameter* rfid;

      if (request->hasParam("rfid")) {
        rfid = request->getParam("rfid");
      }

      if (rfid) {
        saveUser(rfid->value(), 0);
        request->send(200);
      } else {
        request->send(400);
      }
    });
  }

  server.begin();
}

void clearRfidReadCallback(bool cancel) {
  if (cancel && rfidReadContext.request) {
    rfidReadContext.request->send(409);
  }

  rfidReadContext.request = NULL;
  rfidReadContext.name = NULL;
  rfidReadContext.role = NULL;
  rfidReadContext.accesslvl = 0;
  rfidReadCallback = NULL;
}

void createRfidUser(String rfid) {
  saveUser(rfid, rfidReadContext.name, rfidReadContext.role, rfidReadContext.accesslvl);
  rfidReadContext.request->send(200);
}

void doLogin(String rfid) {
  uint32_t session = ESP.random();
  char sess[20];
  sprintf(sess, "%x", session);

  rfidReadContext.request->send(200, "text/plain", sess);
}

void clearMessageInFiveSeconds(bool reset) {
  willRestart = reset;
  previousMillis = millis();
}

void loop() {
  if (isCardPresent()) {
    String tag = getCardId();
    String name;
    String role;

    uint8_t accessType = getUser(tag, name, role); // tipo de acesso

    if (rfidReadCallback != NULL) {
      rfidReadCallback(tag);
      clearRfidReadCallback(false);
    } else {
      if (tag.equals("6375457A")) {
        saveConfig("wifi_reset", "1");
      }

      printMessageAccess(tag, name, accessType);
      clearMessageInFiveSeconds(false);
      delay(1000);
    }
  }

  if (previousMillis) {
    unsigned long currentMillis = millis();

    if ((currentMillis - previousMillis) >= 5000) {
      previousMillis = 0;
      printMessageDefault();
    }
  }

  if (rfidReadContext.ms) {
    unsigned long currentMillis = millis();

    if ((currentMillis - rfidReadContext.ms) >= 15000) {
      clearRfidReadCallback(true);
      printMessageDefault();
    }
  }
}

void displayInit() {
  display.initR(INITR_BLACKTAB); // Init ST7735S chip, black tab
  display.setTextColor(ST7735_WHITE);
  display.setRotation(3);
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
    String wifiIp = "http://";
    
    wifiName.concat(configWiFiName2);
    wifiPassword.concat(configWiFiPassword2);
    wifiIp.concat(WiFi.softAPIP().toString());

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
    displayPrint("Entrada recusada", ST77XX_ORANGE);
    Serial.println("Acesso negado:");
    Serial.println(RFID);
  } else {
    String userName = "Nome: ";
    String spaces = "      ";

    userName.concat(Name.substring(0, 20));
    spaces.concat(Name.substring(20, 40));

    displayPrint("Entrada liberada", ST77XX_CYAN);
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
  // File config = SD.open("config.txt")

  // caso for possivel abrir o arquivo
  if (config) {
    Serial.println("lendo config.txt");
    
    while (config.available()) {
      current = "";
      currentvalue = "";
      char c = '\0';

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
  return getUser(RFID, "", "");
}

uint8_t getUser(String RFID, String Name, String Role) {
  String current = "";
  uint8_t accesslvl = 0;
  int userCount = 0;

  String path = "lista.csv";
  File list = LittleFS.open(path, "r");

  // caso for possivel abrir o arquivo
  if (list) {
    Serial.println("lendo lista.csv");
    
    while (list.available()) {
      char c = '\0';
      
      do {
        c = char(list.read());

        if ((c == ',') || (c == '\n')) {
          break;
        } else {
          current.concat(c);
        }

      } while (list.available());

      Serial.printf("current: %s\n", current);

      userCount++;
      if (current == RFID) {
        accesslvl = char(list.read()) - '0'; // ler o numero depois da virgula que indica se eh ou n eh administrador
      
        if (!list.available() || (c == '\n')) {
          current = "";
          accesslvl = 0;
          break;
        }

        c = char(list.read()); // descartar vírgula

        if (!list.available() || (c == '\n')) {
          current = "";
          accesslvl = 0;
          break;
        }

        do {
          c = char(list.read());

          if ((c == ',') || (c == '\n')) {
            break;
          } else {
            Role.concat(c);
          }

        } while (list.available());

        do {
          c = char(list.read());

          if ((c == '\n')) {
            break;
          } else {
            Name.concat(c);
          }

        } while (list.available());
      } else {
        current = "";

        do {
          c = char(list.read());

          if (c == '\n') {
            break;
          }

        } while (list.available());
      }

      Serial.println(accesslvl);
    }
    list.close();
    Serial.println("fechando lista.csv");

    if (userCount == 0) {
      clearMessage();
      displayPrint("Registrar cartao admin...", ST77XX_WHITE);
    }
  } else {
    File create = LittleFS.open(path, "w");
    create.close();
    Serial.println("nao abrimos absolutamente nada");
  }

  return accesslvl;
}

uint8_t saveUser(String RFID, short newAccesslvl) {
  return saveUser(RFID, "", "", newAccesslvl);
}

uint8_t saveUser(String RFID, String Name, String Role, short newAccesslvl) {
    short n = 0; // linha
    String current = ""; // string do RFID
    short accesslvl = -1; // 0 = n encontrou   1 = encontrou e eh usuario comum   2 = encontrou e eh adm
    short state = 0;

    String path = "lista.csv";
    File list = LittleFS.open(path, "r+");
      
    if (list) {
      Serial.println("lendo lista.csv");
      
      while (list.available()) {  // le linha por linha do arquivo ate acabar, ou ate encontrar uma correspondencia com o RFID lido, e entao da um break
          for (short i = 0; i < 8; i++) {
              current.concat(char(list.read()));
          }
          list.read(); // leia e ignore a virgula
          accesslvl = char(list.read()) - '0';
          list.read(); // leia e ignore o \r
          list.read(); // leia e ignore o \n
          n++;
          if (current == RFID) {
              break;
          }
          current = "";
          accesslvl = -1;
      }

      Serial.println("escrevendo lista.csv");
    
      if (accesslvl == -1) {   // criar nova linha com o cadastro do novo usuario
          state = 1;
          list.print(RFID);
          list.print(',');
          list.print(newAccesslvl);
          list.print('\r');
          list.print('\n');
          Serial.println("CADASTROU");
      } else { 
          state = 1;
          if (accesslvl != 0) {newAccesslvl = 0; state = 2;}

          list.seek((n - 1) * 12);
          list.print(RFID);
          list.print(',');
          list.print(newAccesslvl);
          Serial.println("REMOVEU");
      }
      list.close();
      Serial.println("fechando lista.csv");
    }

    return state;
}

void saveLog(String RFID, short action, String admRFID) {
  // action
  // 0 - bloqueado
  // 1 - identificado
  // 2 - cadastrado
  // 3 - removido/desativado
  String path = "log.csv";
  File accesslog = LittleFS.open(path, "a");
    
  if (accesslog) {
    Serial.println("escrevendo log.csv");
    
    accesslog.print("hora,");
    
    if (RFID == MASTERCARD) {
      accesslog.print("MSTRCARD");
    } else {
      accesslog.print(RFID);
    }
    
    switch (action) {
      case 0:
        accesslog.print(",BLOQUEADO");
        break;
      case 1:
        accesslog.print(",IDENTIFICADO");
        break;
      case 2:
        accesslog.print(",ADICIONADO POR,");
        if (RFID == MASTERCARD) {
          accesslog.print(admRFID);
        } else {
          accesslog.print(",ADICIONADO POR MASTERCARD");
        }
        break;
      case 3:
        accesslog.print(",REMOVIDO POR,");
        if (RFID == MASTERCARD) {
          accesslog.print(admRFID);
        } else {
          accesslog.print(",REMOVIDO POR MASTERCARD");
        }
        break;
      default:
        accesslog.print(",");
        break;
    }
    
    accesslog.print('\r');
    accesslog.print('\n');
    accesslog.close();
    Serial.println("fechando log.csv");
  }
}