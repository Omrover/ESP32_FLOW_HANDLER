//Librerías para websocket y servidor wifi
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <WiFi.h>
#include <WebSocketsServer.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>

//Credenciales
const char* ssid = "robot123";//Colocar id de la red wifi
const char* password = "11111111";//Colocar la pass de la red
const int serverPort = 80;//Definimos el puerto del servidor web
const int socketPort = 81;//Definimos el puerto del web socket

//Definición de funciones
void IRAM_ATTR contarPulsos();
void IRAM_ATTR pulseReceived();
float obtenerCaudal();
void sendData();
void handleWebSocketMessage(uint8_t num, WStype_t type, uint8_t *payload, size_t length);
void notFound(AsyncWebServerRequest *request);

//Variables de burst
const int Em_pulse = 13;

//Variable y función para lectura de sensor no invasivo
volatile unsigned long interruptTime = 0;  // volatile para variables compartidas con ISR
volatile bool newMeasurement = false;//Bandera para no saturar interrupción
unsigned long burstStartTime = 0;//Tiempo de disparo
unsigned long timeDifference = 0;//Tiempo de recepción

void IRAM_ATTR pulseReceived() {
  if (!newMeasurement) {  // Evitar múltiples capturas
    interruptTime = micros();  // Obtiene el tiempo actual de recepción en tiempo del micro
    newMeasurement = true;//Bandera alto
  }
}


//Servidores
AsyncWebServer server(serverPort);
WebSocketsServer webSocket(socketPort);

//Variables y cttes para cálculo de caudal
const unsigned factorConversion = 1;
int tiempoM=0;
float caudal = 0;
int frecuencia = 0;
volatile int pulsosInvasivo;

//Configuración de burst
const int PWM_Burst  = 25;
const uint32_t PWM_FREQ = 1000000; //1 MHz
const uint8_t  PWM_RES  = 6; //valores de 0 -> 64
const int duty = 32;

const int pinIvasivo = 12;//Pin de interrupción para leer flancos de subida

void IRAM_ATTR contarPulsos(){//ISR que se ramifica a la ram para ser ejeutada de forma rápida
  pulsosInvasivo++;//aumenta los pulsos
}

float obtenerCaudal(){//Función que retorna el caudal
  pulsosInvasivo=0;//Contador de pulsos reiniciado
  interrupts();//Activamos los ISR's
  delay(1000);//Esperamos un segundo
  noInterrupts();//Desactivamos
  frecuencia=pulsosInvasivo;//Pulsos en un segundo
  float caudal=(frecuencia*factorConversion);//Obteción del caudal L/min
  return caudal;
}



void setup() {
  //Iniciar puerto serial
  Serial.begin(115200);

  pinMode(Em_pulse, INPUT_PULLUP);//Configurar el pin como entrada con resistencia pullup
  attachInterrupt(digitalPinToInterrupt(Em_pulse), pulseReceived, RISING);//Configura la interrupción del pin para detectar flancos de subida
  
  pinMode(pinIvasivo,INPUT_PULLUP);//Configurar el pin como entrada con resistencia pullup
  attachInterrupt(digitalPinToInterrupt(pinIvasivo), contarPulsos, RISING);//Configura la interrupción del pin para detectar flancos de subida

  //Iniciar la conexión wifi con los datos de red
  WiFi.begin(ssid, password);

  Serial.println("Conectandose...");//log
  while(WiFi.status() != WL_CONNECTED) //Espera por la conexión entre cliente y red
  { 
    delay(500);
    Serial.print("Conectandose...");//Imprime cada que quiere conectarse
  }
  Serial.println("");
  Serial.print("Conexión exitosa, Su dirección IP es: ");
  Serial.println(WiFi.localIP());//ip de la tarjeta en el servidor

if(!SPIFFS.begin(true)) {//Inicia el sistema de archivos SPIFFS de la esp32
    Serial.println("Falló el montaje de SPIFFS");//log cuando no se inicia el sistema
    // Intentar formatear
    Serial.println("Intentando formatear SPIFFS...");
    if(SPIFFS.format()) {
      Serial.println("SPIFFS formateado exitosamente");
      if(SPIFFS.begin(true)) {
        Serial.println("SPIFFS montado después de formatear");
      } else {
        Serial.println("No se pudo montar SPIFFS después de formatear");
        return;
      }
    } else {
      Serial.println("No se pudo formatear SPIFFS");
      return;
    }
  } else {
    Serial.println("SPIFFS montado correctamente");
  }

  //Imprime la lista de archivos en el directorio "esp32_matlab"
  File root = SPIFFS.open("/");//Abre el directorio data donde se encuentran los archivos
  File file = root.openNextFile();//Abre los archivos para flashear
  while(file){//Si el archivo no es nulo
    Serial.println("Archivo: ");
    Serial.print(file.name());//Nombre del archivo
    file = root.openNextFile();
  }
//}
  root.close();//Cierra el directorio

  //Por medio del protocolo get enviamos el archivo index.html del SPIFFS cuando el cliente hace la petición al directorio raíz
  server.on("/",HTTP_GET,[](AsyncWebServerRequest *request) 
  {request->send(SPIFFS,"/index.html","text/html ");
  });
  server.onNotFound(notFound);//Manejo de error de petición
  server.begin();//Inicia el servidor
  webSocket.begin();//Iniciamos una conexión websocket para comunicar en tiempo real el cliente y el servidor 
  webSocket.onEvent([](uint8_t num, WStype_t type, uint8_t *payload, size_t lenght)//Maneja los eventos sel servidor (conexión, desconexión, envío de datos)
  {handleWebSocketMessage(num, type, payload, lenght);});

  ledcAttach(PWM_Burst, PWM_FREQ, PWM_RES);//Configuramos la función PWM (1MHz, 6bits de resolución)


}


void loop() {
  // put your main code here, to run repeatedly:
  webSocket.loop();//Hacemos loop al servidos para que mantega la comunicación ctte
  while((millis() - tiempoM) < (300)){}//Delay de 300ms no bloqueantes
      tiempoM = millis();
  caudal=obtenerCaudal();//LLama a la función para obtener el caudal invasivo
  sendData();//Envía el payload al websocket
 // 1. Enviar pulso y marcar tiempo de inicio
  burstStartTime = micros();
  ledcWrite(PWM_Burst, duty);
  
  // 2. Pequeña pausa para el pulso (50μs en lugar de 0.05ms)
  delayMicroseconds(20);
  
  // 3. Apagar el pulso
  ledcWrite(PWM_Burst, 0);
  
  // 4. Esperar y verificar si llegó una medición
  unsigned long timeout = millis() + 100; // Timeout de 100ms
  
  while (!newMeasurement && millis() < timeout) {
    // Esperar activamente por nueva medición
    delay(1);
  }
  
  // 5. Procesar medición si está disponible
  if (newMeasurement) {
    timeDifference = interruptTime - burstStartTime;
    
    Serial.print("Tiempo de vuelo: ");
    Serial.print(timeDifference);
    Serial.println(" μs");
    
    // Reset después de procesar
    newMeasurement = false;
  } 
  
  // 6. Pausa entre mediciones completas
  delay(10);
}

void sendData(){
  String data = "{\"caudal\":" + String(caudal)+"}";//Damos formato al payload en JSON para envíar la onformación de los sensores
  webSocket.broadcastTXT(data);//Envíamos la estructura por broadcast para que el servidor la escuche

}

void handleWebSocketMessage(uint8_t num, WStype_t type, uint8_t *payload, size_t lenght){//Callback de handler de websocket
  switch (type)
  {
    case WStype_DISCONNECTED://Caso para hay desconexión
      Serial.printf("[%u] Desconectado!\n",num);
      break;
    case WStype_CONNECTED://Caso para hay desconexión al websocket
      {
      IPAddress ip = webSocket.remoteIP(num);//Recuperar la ip del cliente
      Serial.printf("[%u] Conectado desde %d.%d.%d.%d\n",num,ip[0],ip[1],ip[2],ip[3]);//Imprime la IP
      break;
      }
  }
}

void notFound (AsyncWebServerRequest *request){//Callback de handler cuando no funciona
  request->send(404,"text/plain","Página no encontrada!");
}
