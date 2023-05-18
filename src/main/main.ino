#include <string.h>
#include <SD.h>
#include <Hash.h>

#include <ESP8266WiFi.h>
//#include <WiFiManager.h>
//#include <ESP8266WebServer.h>
#include <DNSServer.h>

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
int currentDisplayLine = 0;
int configReset = 0;
String configWiFiName = "DOOR_ACCESS";
String configWiFiPassword = "12345678";

void setup() {
  Serial.begin(9600);

  SPI.begin();

 // Inicia display
  displayInit();

  // Inicia rfid
  rfid.PCD_Init();

  printHeader();

  int sdIsInitialized = SD.begin(SD_CARD_CS);

  getConfig();
  
  mensagensIniciais(sdIsInitialized);

  // declaração do objeto wifiManager
  // WiFiManager wifiManager;

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("dooraccess")) {
      request->send(200, "text/plain", "ESP8266 DOOR ACCESS API");
    }

    request->send(400);
  });

  if (configReset == 1) {
    IPAddress local_ip(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 9);
    IPAddress subnet(255, 255, 255, 0);

    WiFi.disconnect();
    WiFi.softAP(configWiFiName, configWiFiPassword);
    WiFi.softAPConfig(local_ip, gateway, subnet);

    server.on("/", HTTP_POST, [](AsyncWebServerRequest *request) {
      AsyncWebParameter* ssid;
      AsyncWebParameter* pass;

      if (request->hasParam("ssid", true)) {
        ssid = request->getParam("ssid", true);
      }

      if (request->hasParam("pass", true)) {
        pass = request->getParam("pass", true);
      }

      if (ssid && pass) {
        request->send(200);
        WiFi.begin(ssid->value(), pass->value());
        int8_t res = WiFi.waitForConnectResult();
        if (res == WL_CONNECTED) {
          saveConfig("wifi_reset", "0");
        } else {
          WiFi.disconnect();
        }

        ESP.restart();
      } else {
        request->send(400);
      }
    });
    //wifiManager.resetSettings();
  } else {
    WiFi.begin();
    Serial.println(WiFi.localIP());

    server.on("/users", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send(SPIFFS, "/lista.csv");
    });

    server.on("/users", HTTP_POST, [](AsyncWebServerRequest *request) {
      AsyncWebParameter* rfid;
      short accesslvl = 1;

      if (request->hasParam("rfid", true)) {
        rfid = request->getParam("rfid", true);
      }

      if (request->hasParam("admin", true)) {
        accesslvl = 2;
      }

      if (rfid) {
        registerId(rfid->value(), accesslvl);
        request->send(200);
      } else {
        request->send(400);
      }
    });

    server.on("/users", HTTP_DELETE, [](AsyncWebServerRequest *request) {
      AsyncWebParameter* rfid;

      if (request->hasParam("rfid")) {
        rfid = request->getParam("rfid");
      }

      if (rfid) {
        registerId(rfid->value(), 0);
        request->send(200);
      } else {
        request->send(400);
      }
    });
  }

  //callback para quando entra em modo de configuração AP
  //wifiManager.setAPCallback(configModeCallback);
    
  //callback para quando se conecta em uma rede, ou seja, quando passa a trabalhar em modo estação
  //wifiManager.setSaveConfigCallback(saveConfigCallback); 

  //cria uma rede de nome ESP_AP com senha 12345678
  //wifiManager.autoConnect(configWiFiName.c_str(), configWiFiPassword.c_str());
}

void loop() {
  if (isCardPresent()) {
    String tag = getCardId();

    uint8_t accessType = getAccessType(tag); // tipo de acesso

    if (isAccessAllowed(accessType)) {
      Serial.println("Identificado");
      
      char *cardId = &tag[0];
      displayPrint(cardId, ST77XX_CYAN);         
    } 
    else {
      Serial.println("Bloqueado");
      
      char *cardId = &tag[0];
      displayPrint(cardId, ST77XX_RED);         
    }
  }
}

/*
// callback que indica que o ESP entrou no modo AP
void configModeCallback(WiFiManager *myWiFiManager) {  
  // Serial.println("Entered config mode");
  Serial.println("Entrou no modo de configuração");
  Serial.println(WiFi.softAPIP()); // imprime o IP do AP
  Serial.println(myWiFiManager->getConfigPortalSSID()); // imprime o SSID criado da rede
}
*/

//callback que indica que salvamos uma nova rede para se conectar (modo estação)
void saveConfigCallback() {
  saveConfig("wifi_reset", "0");
  Serial.println("Configuração salva");
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

void mensagensIniciais(int sdIsInitialized) {
  String wifiName = "WiFi: ";
  String wifiPassword = "Senha: ";
  
  wifiName.concat(configWiFiName);
  wifiPassword.concat(configWiFiPassword);

  if (configReset == 1) {
    displayPrint("--------------------------", ST77XX_WHITE);
    displayPrint(wifiName, ST77XX_WHITE);
    displayPrint(wifiPassword, ST77XX_WHITE);
    displayPrint("", ST77XX_WHITE);
    displayPrint("http://192.168.4.1/", ST77XX_CYAN);
    return;
  }

  if (sdIsInitialized) {
    Serial.println("SDCard iniciado."); // F()
    displayPrint("--------------------------", ST77XX_GREEN);
  } else {
    Serial.println("Inicialização do SDCard falhou!");
    displayPrint("SDCard falhou!", ST77XX_RED);
  }

  // Mensagens iniciais no display
  
  displayPrint("", ST77XX_WHITE);
  displayPrint("Aproxime seu cartao", ST77XX_WHITE);
  displayPrint("do leitor...", ST77XX_WHITE);
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

  File config;

  // caso for possivel abrir o arquivo
  if (config = SD.open("config.txt")) {
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
    Serial.println("nenhuma config file");
  }

  return 0;
}

uint8_t saveConfig(String ConfigToSearch, String ValueToSave) {
  String newFile = "";
  String current = ""; // config label buffer

  File config;

  // caso for possivel abrir o arquivo
  if (config = SD.open("config.txt", FILE_WRITE)) {
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

uint8_t getAccessType(String RFID) { // 0: denied | 1: allowed (Commun User) | 2: allowed (Admin)
  String current = "";
  uint8_t accesslvl = 0;

  File list;

  // caso for possivel abrir o arquivo
  if (list = SD.open("lista.csv")) {
    Serial.println("lendo lista.csv");
    
    while (list.available()) {   // enquanto n chegar ao EOF, ler cada char dos 8 caracteres, colocar numa string e...
        for (uint8_t i = 0; i < 8; i++) {
          current.concat(char(list.read()));
        }
        
        Serial.printf("current: %s\n", current);
        
        list.read(); // leia e ignore a virgula
        accesslvl = char(list.read()) - '0'; // ler o numero depois da virgula que indica se eh ou n eh administrador
        
        Serial.println(accesslvl);
        
        list.read(); // leia e ignore o \r
        list.read(); // leia e ignore o \n
        if (current == RFID) { // ...checar se essa string bate com a string lida pelo RFID
            break;
        }
        current = "";
        accesslvl = 0;
    }
    list.close();
    Serial.println("fechando lista.csv");
  } else {
    Serial.println("nao abrimos absolutamente nada");
  }

  return accesslvl; // 0: denied | 1: allowed (Commun User) | 2: allowed (Admin)
}

boolean isAccessAllowed(uint8_t _accessType) {
  if (_accessType == 0) { // Verifica o nivel de acesso do cartão
    return false; // Cartão nao está na lista
  }
  return true; // // Cartao está na lista (Usuario Comum/Admin)
}

uint8_t registerId(String RFID, short newAccesslvl) {
    short n = 0; // linha
    String current = ""; // string do RFID
    short accesslvl = -1; // 0 = n encontrou   1 = encontrou e eh usuario comum   2 = encontrou e eh adm
    short state = 0;

    File list;
      if(list = SD.open("lista.csv", FILE_WRITE)){
      Serial.println("teste lista");  
      list.close();
      }
      
    if (list = SD.open("lista.csv", FILE_READ)) {
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
      list.close();
      Serial.println("fechando lista.csv");
    }

    if (list = SD.open("lista.csv", FILE_WRITE)) {   // abre o arquivo e prepara o carriage no final de tudo
        Serial.println("escrevendo lista.csv");
      
        if (accesslvl == -1) {   // criar nova linha com o cadastro do novo usuario
            state = 1;
            list.print(RFID);
            list.print(',');
            list.print(newAccesslvl);
            list.print('\r');
            list.print('\n');
            //DEBUG  Serial.println("CADASTROU");
        } else { 
            state = 1;
            if (accesslvl != 0) {newAccesslvl = 0; state = 2;}

            list.seek((n - 1) * 12);
            list.print(RFID);
            list.print(',');
            list.print(newAccesslvl);
            //DEBUG  Serial.println("REMOVEU");
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
  File accesslog;
    
  if (accesslog = SD.open("log.csv", FILE_WRITE)){
    Serial.println("escrevendo log.csv");
    
    accesslog.print("hora,");
    
    if(RFID == MASTERCARD) {
      accesslog.print("MSTRCARD");
    } 
    else {
      accesslog.print(RFID);
    }
    
    switch(action) {
      case 0:
        accesslog.print(",BLOQUEADO");
        break;
      case 1:
        accesslog.print(",IDENTIFICADO");
        break;
      case 2:
        accesslog.print(",ADICIONADO POR,");
        if(RFID == MASTERCARD) {
          accesslog.print(admRFID);
        }
        else {
          accesslog.print(",ADICIONADO POR MASTERCARD");
        }
        break;
      case 3:
        accesslog.print(",REMOVIDO POR,");
        if(RFID == MASTERCARD) {
          accesslog.print(admRFID);
        }
        else {
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