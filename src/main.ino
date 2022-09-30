#include <string.h>

#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>

#include <MFRC522.h> // biblioteca responsável pela comunicação com o módulo RFID-RC522
#include <MFRC522Extended.h> // biblioteca responsável pela comunicação com o módulo RFID-RC522
#include <SPI.h> // biblioteca para comunicação do barramento SPI
#include <Adafruit_GFX.h> // Core graphics library
#include <Adafruit_ST7735.h> // Hardware-specific library for ST7735

constexpr uint8_t RFID_RST_PIN = D3;     // Configurable, see typical pin layout above
constexpr uint8_t RFID_SDA_PIN = D4;     // Configurable, see typical pin layout above

constexpr uint8_t DISPLAY_CS = D8;
constexpr uint8_t DISPLAY_DC = D2; // A0/DC;
constexpr uint8_t DISPLAY_RST = D1;

constexpr uint8_t DISPLAY_SDA = D7; // sda/mosi;
constexpr uint8_t DISPLAY_SCK = D5;

Adafruit_ST7735 display = Adafruit_ST7735(DISPLAY_CS,  DISPLAY_DC, DISPLAY_RST); 

MFRC522 rfid(RFID_SDA_PIN, RFID_RST_PIN); // Instance of the class
MFRC522::MIFARE_Key key;

int currentDisplayLine = 0;

void setup() {
  Serial.begin(9600);
  
  SPI.begin();

  displayInit();

  // Inicia MFRC522
  rfid.PCD_Init();
  
  mensagensIniciais();
  
  /*
  // declaração do objeto wifiManager
  WiFiManager wifiManager;
  
  // wifiManager.resetSettings();

  //callback para quando entra em modo de configuração AP
  wifiManager.setAPCallback(configModeCallback); 
    
  //callback para quando se conecta em uma rede, ou seja, quando passa a trabalhar em modo estação
  wifiManager.setSaveConfigCallback(saveConfigCallback); 

  //cria uma rede de nome ESP_AP com senha 12345678
  wifiManager.autoConnect("ESP_AP", "12345678"); 
  */
}

void loop() {
  if (isCardPresent()) {
    String tag = getCardId();

    Serial.println(tag);
    
    char *cardId = &tag[0];
    
    displayPrint(cardId, ST77XX_CYAN);
  }
}

// callback que indica que o ESP entrou no modo AP
void configModeCallback(WiFiManager *myWiFiManager) {  
  // Serial.println("Entered config mode");
  Serial.println("Entrou no modo de configuração");
  Serial.println(WiFi.softAPIP()); // imprime o IP do AP
  Serial.println(myWiFiManager->getConfigPortalSSID()); // imprime o SSID criado da rede
}

//callback que indica que salvamos uma nova rede para se conectar (modo estação)
void saveConfigCallback() {
  Serial.println("Configuração salva");
}

void displayInit() {
  display.initR(INITR_BLACKTAB); // Init ST7735S chip, black tab
  display.setTextColor(ST7735_WHITE);
  display.setRotation(1);
  display.setTextWrap(true);

  Serial.println(F("Display iniciado."));
}

void mensagensIniciais() {
  // Mensagens iniciais no display
  display.fillScreen(ST77XX_BLACK); //Pinta a tela de preto
  currentDisplayLine = 1;
  displayPrint("Door Access", ST77XX_GREEN);
  currentDisplayLine = 3;
  displayPrint("Aproxime seu cartao", ST77XX_WHITE);
  displayPrint("do leitor...", ST77XX_WHITE);
}

//função para controlar linhas e automatizar impressão no display
void displayPrint(char *text, uint16_t color) {
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