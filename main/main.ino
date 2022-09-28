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

#define DISPLAY_CS      13
#define DISPLAY_DC      2
#define DISPLAY_SDA     13
#define DISPLAY_SCK     14
#define DISPLAY_RST     3

#define SIZE_BUFFER     18
#define MAX_SIZE_BLOCK  16
#define PIN_AP          16

Adafruit_ST7735 tft = Adafruit_ST7735(DISPLAY_CS, DISPLAY_DC, DISPLAY_SDA, DISPLAY_SCK, DISPLAY_RST);

MFRC522 rfid(RFID_SDA_PIN, RFID_RST_PIN); // Instance of the class
MFRC522::MIFARE_Key key;

int LINHA_DO_DISPLAY = 0;

// callback que indica que o ESP entrou no modo AP
void configModeCallback (WiFiManager *myWiFiManager) {  
  // Serial.println("Entered config mode");
  Serial.println("Entrou no modo de configuração");
  Serial.println(WiFi.softAPIP()); // imprime o IP do AP
  Serial.println(myWiFiManager->getConfigPortalSSID()); // imprime o SSID criado da rede
}

//callback que indica que salvamos uma nova rede para se conectar (modo estação)
void saveConfigCallback () {
  Serial.println("Configuração salva");
}

void mensagensIniciais()
{
  // Mensagens iniciais no serial monitor
  Serial.println("Aproxime o seu cartao do leitor...");
  Serial.println();

  // Mensagens iniciais no display
  tft.fillScreen(ST77XX_BLACK); //Pinta a tela de preto
  LINHA_DO_DISPLAY = 1;
  displayPrint("Door Access", ST77XX_WHITE);
  LINHA_DO_DISPLAY = 3;
  displayPrint("Aproxime seu cartao", ST77XX_WHITE);
  displayPrint("do leitor...", ST77XX_WHITE);
  delay(1000);
}

//função para controlar linhas e automatizar impressão no display
void displayPrint(char *text, uint16_t color) {
  tft.setCursor(0, LINHA_DO_DISPLAY * 10);
  tft.setTextColor(color);
  tft.setTextWrap(true);
  tft.print(text);
  LINHA_DO_DISPLAY++;
  if (LINHA_DO_DISPLAY > 12)
  {
    tft.fillScreen(ST77XX_BLACK); //Pinta a tela de preto
    LINHA_DO_DISPLAY = 0;
  }
}

String getCardId() {
  String cardId = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    cardId.concat(String(rfid.uid.uidByte[i] < 0x10 ? "0" : "")); // Adiciona o zero se o id é menor que 16 ou seja 00, 01, 02, ..., 0E, 0F;
    cardId.concat(String(rfid.uid.uidByte[i], HEX));
  }
  cardId.toUpperCase();
  return cardId;
}

boolean isCardPresent() {
  rfid.PICC_ReadCardSerial(); //Always fails
  Serial.println(rfid.PICC_IsNewCardPresent());
  Serial.println(rfid.PICC_ReadCardSerial());
  if (rfid.PICC_IsNewCardPresent()) {
    if (rfid.PICC_ReadCardSerial()) {
      return true;
    }
  }
  return false;
}

void setup() {
  Serial.begin(9600);
  SPI.begin();

  // Use this initializer if using a 1.8" TFT screen:
  //tft.initR(INITR_BLACKTAB);      // Init ST7735S chip, black tab
  //tft.setRotation(1); //Ajusta a direção do display
  //Serial.println(F("Display iniciado."));

  // Inicia MFRC522
  rfid.PCD_Init();
  
  //mensagensIniciais();
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

String tag;

void loop() {
  if (!rfid.PICC_IsNewCardPresent())
    return;
  if (rfid.PICC_ReadCardSerial()) {
    for (byte i = 0; i < 4; i++) {
      tag += rfid.uid.uidByte[i];
    }
    Serial.println(tag);
    tag = "";
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();

    /*
    char * cardId;
    tag.toCharArray(cardId, 32);
    Serial.println(cardId);
    displayPrint(cardId, ST77XX_WHITE);
    */
  }
}