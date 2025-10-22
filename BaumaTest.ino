#include <Adafruit_GFX.h>
#include <Adafruit_ST7796S.h>
#include <Wire.h>
#include <SparkFun_MicroPressure.h>
#include <BluetoothSerial.h>

// ------------------ TFT (ST7796S SPI) ------------------
#define TFT_CS    15
#define TFT_DC     2
#define TFT_RST    4
#define TFT_BL     5
Adafruit_ST7796S tft = Adafruit_ST7796S(TFT_CS, TFT_DC, TFT_RST);

// ------------------ Pines del sistema ------------------
#define BOTON_PIN    13
#define IN1          19
#define IN2          27
#define VALVULA_PIN  12   // HIGH = válvula cerrada (inflar), LOW = abierta (deflar)

// ------------------ Parámetros ------------------
#define PRESION_MAX 140.0

SparkFun_MicroPressure mpr;
BluetoothSerial SerialBT;

// ------------------ Estados ------------------
bool motorEncendido    = false;
bool capturando        = false;
bool ultimoEstadoBoton = HIGH;
unsigned long ultimaPulsacion = 0;
const int tiempoDebounce = 50;

// ------------------ Muestras ------------------
const int maxMuestras = 10000;
float presionMuestras[maxMuestras];

int muestraIndex = 0;
unsigned long tiempoInicioCaptura = 0;
unsigned long ultimaLecturaMicros = 0;

String datosCSV = "";

// ------------------ Resultados ------------------
bool  resultadosListos = false;
float sistolica = 0.0f;
float diastolica = 0.0f;

// ------------------ Búsqueda de picos ------------------
#define MAX_PICOS 20

int encontrarPicosLocales(int* indicesPicos, float* valoresPicos) {
  int contador = 0;
  for (int i = 1; i < muestraIndex - 1 && contador < MAX_PICOS; i++) {
    if (presionMuestras[i] > presionMuestras[i - 1] && presionMuestras[i] > presionMuestras[i + 1]) {
      indicesPicos[contador] = i;
      valoresPicos[contador] = presionMuestras[i];
      contador++;
    }
  }
  return contador;
}

void ordenarPicos(int* indicesPicos, float* valoresPicos, int n) {
  for (int i = 0; i < n - 1; i++) {
    for (int j = 0; j < n - i - 1; j++) {
      if (valoresPicos[j] < valoresPicos[j + 1]) {
        float tempVal = valoresPicos[j];
        valoresPicos[j] = valoresPicos[j + 1];
        valoresPicos[j + 1] = tempVal;
        int tempInd = indicesPicos[j];
        indicesPicos[j] = indicesPicos[j + 1];
        indicesPicos[j + 1] = tempInd;
      }
    }
  }
}

int obtenerCantidadPicos() {
  int indicesPicos[MAX_PICOS];
  float valoresPicos[MAX_PICOS];
  return encontrarPicosLocales(indicesPicos, valoresPicos);
}

int* obtenerPicosOrdenados(int &cantidad) {
  static int indicesPicos[MAX_PICOS];
  static float valoresPicos[MAX_PICOS];
  cantidad = encontrarPicosLocales(indicesPicos, valoresPicos);
  ordenarPicos(indicesPicos, valoresPicos, cantidad);
  return indicesPicos;
}

// ------------------ UI: corazón y textos ------------------
// Patrón de corazón 7×9
static const uint8_t heartPattern[7][9] = {
  {0,0,1,1,0,1,1,0,0},
  {0,1,1,1,1,1,1,1,0},
  {1,1,1,1,1,1,1,1,1},
  {1,1,1,1,1,1,1,1,1},
  {0,1,1,1,1,1,1,1,0},
  {0,0,1,1,1,1,1,0,0},
  {0,0,0,1,1,1,0,0,0}
};
bool heartBig = false;
unsigned long lastHeartToggle = 0;
const unsigned long heartInterval = 500;  // ms

void drawHeart(int16_t x0, int16_t y0, int16_t bs) {
  const int rows = 7, cols = 9;
  int16_t px = x0 - (cols * bs) / 2;
  int16_t py = y0 - (rows * bs) / 2;
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      if (heartPattern[r][c]) {
        tft.fillRect(px + c * bs, py + r * bs, bs, bs, ST77XX_YELLOW);
      }
    }
  }
}

void splash() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(4);
  tft.setCursor(25, 155); tft.print("Presione el");
  tft.setCursor(25, 200); tft.print("boton para");
  tft.setCursor(25, 245); tft.print("medir la");
  tft.setCursor(25, 290); tft.print("presion <3");
}

void drawLive(float pmmHg, const char* estado) {
  // Encabezado
  tft.fillRect(0, 0, tft.width(), 40, ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setTextSize(3);
  tft.setCursor(10, 10);
  tft.print(estado);

  // Presión instantánea
  tft.fillRect(0, tft.height() - 100, tft.width(), 100, ST77XX_BLACK);
  tft.setCursor(20, tft.height() - 90);
  tft.printf("Pres: %.1f mmHg", pmmHg);

  // SYS/DIA (si hay; si no, --/--)
  tft.setCursor(20, tft.height() - 50);
  if (resultadosListos) {
    tft.printf("SYS/DIA: %.0f/%.0f", sistolica, diastolica);
  } else {
    tft.print("SYS/DIA: --/--");
  }
}

void animaInflando(float pmmHg) {
  if (millis() - lastHeartToggle > heartInterval) {
    lastHeartToggle = millis();
    heartBig = !heartBig;
    tft.fillScreen(ST77XX_BLACK);
    drawHeart(tft.width()/2, tft.height()/2, heartBig ? 22 : 17);
  }
  drawLive(pmmHg, "Inflando...");
}

void dibujaMedicion(float pmmHg) {
  static bool once = false;
  if (!once) {
    tft.fillScreen(ST77XX_BLACK);
    drawHeart(tft.width()/2, tft.height()/2, 20); // estático
    once = true;
  }
  drawLive(pmmHg, "Midiendo...");
}

// ------------------ Control motor/válvula ------------------
void encenderMotores() {
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(VALVULA_PIN, HIGH); // válvula cerrada al inflar
}

void detenerMotores() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
}

// ------------------ Reiniciar proceso ------------------
void reiniciarProceso() {
  detenerMotores();
  digitalWrite(VALVULA_PIN, HIGH); // cierra válvula
  motorEncendido = false;
  capturando = false;
  muestraIndex = 0;
  datosCSV = "";
  ultimaLecturaMicros = micros();
  tiempoInicioCaptura = 0;
  resultadosListos = false;
  sistolica = diastolica = 0.0f;
  splash();
  Serial.println("Proceso reiniciado. Presiona botón para iniciar.");
}

// ------------------ Setup ------------------
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  // TFT
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  tft.init();
  delay(100);
  tft.setRotation(0); // VERTICAL
  splash();

  // IO
  pinMode(BOTON_PIN, INPUT_PULLUP);
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(VALVULA_PIN, OUTPUT);

  digitalWrite(VALVULA_PIN, HIGH); // válvula cerrada (NO) al inicio
  detenerMotores();

  // Sensor
  if (!mpr.begin()) {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(3);
    tft.setCursor(40, 160);
    tft.print("Error sensor");
    Serial.println("Sensor no encontrado.");
    while (1);
  }

  // Bluetooth
  SerialBT.begin("Baumanometro");

  Serial.println("Presione el botón para iniciar");
}

// ------------------ Loop ------------------
void loop() {
  float pressurePa   = mpr.readPressure(PA);
  float pressureMmHg = (pressurePa * 0.00750062f) - 751.6f;

  // Botón con debounce
  bool estadoBoton = digitalRead(BOTON_PIN);
  if (estadoBoton == LOW && ultimoEstadoBoton == HIGH) {
    if (millis() - ultimaPulsacion > (unsigned long)tiempoDebounce) {
      reiniciarProceso();
      encenderMotores();
      motorEncendido = true;
      lastHeartToggle = millis();
      heartBig = false;
      Serial.println("Bomba encendida");
      ultimaPulsacion = millis();
    }
  }
  ultimoEstadoBoton = estadoBoton;

  // UI en vivo
  if (motorEncendido && !capturando) {
    animaInflando(pressureMmHg);
  } else if (capturando) {
    dibujaMedicion(pressureMmHg);
  }

  // Inicia captura al llegar a PRESION_MAX
  if (motorEncendido && !capturando) {
    if (pressureMmHg >= PRESION_MAX) {
      detenerMotores();
      motorEncendido = false;

      delay(1000);
      digitalWrite(VALVULA_PIN, LOW); // abre válvula para desinflar

      muestraIndex = 0;
      tiempoInicioCaptura = micros();
      ultimaLecturaMicros = micros();
      capturando = true;

      Serial.println("Captura iniciada durante 10s (cada 0.2 ms)");
    }
  }

  // Captura 10s @ 200us (tu lógica original)
  if (capturando) {
    unsigned long tiempoInicioMs = tiempoInicioCaptura / 1000;
    if (millis() - tiempoInicioMs <= 10000 && muestraIndex < maxMuestras) {
      if (micros() - ultimaLecturaMicros >= 200) {
        presionMuestras[muestraIndex++] = pressureMmHg;
        ultimaLecturaMicros = micros();
      }
    } else if (capturando) {
      // Fin medición
      capturando = false;
      digitalWrite(VALVULA_PIN, HIGH); // cierra válvula primero para concluir
      delay(150);                      // pequeño respiro
      digitalWrite(VALVULA_PIN, LOW);  // y la dejas en LOW como pediste (NO desenergizada/abierta)

      int cantidadPicos = 0;
      int* picosOrdenados = obtenerPicosOrdenados(cantidadPicos);

      Serial.println("Picos seleccionados (por amplitud):");
      for (int i = 0; i < cantidadPicos; i++) {
        int idx = picosOrdenados[i];
        Serial.print("Pico "); Serial.print(i + 1);
        Serial.print(" en muestra "); Serial.print(idx);
        Serial.print(": "); Serial.print(presionMuestras[idx], 2);
        Serial.println(" mmHg");
      }

      // Sistólica = índice 2 (3er pico)
      // Diastólica = promedio de índices 3..7 (4º a 8º), usando los que existan
      float sys = 0.0f, dia = 0.0f;
      bool ok = false;

      if (cantidadPicos > 0) {
        if (cantidadPicos >= 3) {
          sys = presionMuestras[picosOrdenados[2]];
        } else {
          // respaldo si no hay 3 picos: usa el mayor disponible
          sys = presionMuestras[picosOrdenados[cantidadPicos - 1]];
        }

        double sumDia = 0.0;
        int cntDia = 0;
        for (int r = 3; r <= 7; r++) {      // índices 3..7
          if (r < cantidadPicos) {
            int idx = picosOrdenados[r];
            sumDia += presionMuestras[idx];
            cntDia++;
          }
        }
        if (cntDia > 0) {
          dia = (float)(sumDia / cntDia);
        } else {
          // respaldo si no hay picos 3..7: usa el 2º mejor si existe, si no el 1º
          int rankFallback = (cantidadPicos >= 2) ? 1 : 0;
          dia = presionMuestras[picosOrdenados[rankFallback]];
        }
        ok = true;
      }

      // Mostrar/Enviar resultados
      tft.fillScreen(ST77XX_BLACK);
      tft.setTextColor(ST77XX_WHITE);
      tft.setTextSize(4);

      if (ok) {
        sistolica = sys;
        diastolica = dia;
        resultadosListos = true;

      // Títulos grandes
      tft.setTextSize(4);
      tft.setCursor(20, 100); 
      tft.print("RESULTADOS");

      // SYS y DIA más pequeños
      tft.setTextSize(3);
      tft.setCursor(20, 180); 
      tft.print("SYS: "); 
      tft.print(sistolica, 1); 
      tft.print(" mmHg");

      tft.setCursor(20, 230); 
      tft.print("DIA: "); 
      tft.print(diastolica, 1); 
      tft.print(" mmHg");

        // Bluetooth: "SYS,DIA\n" sin decimales
        SerialBT.printf("%.0f,%.0f\n", sistolica, diastolica);

        Serial.printf("RESULTADOS -> SYS=%.1f  DIA=%.1f  (picos=%d, muestras=%d)\n",
                      sistolica, diastolica, cantidadPicos, muestraIndex);
      } else {
        resultadosListos = false;
        tft.setCursor(20, 200); tft.print("Lectura invalida");
        Serial.println("No hay suficientes picos para calcular SYS/DIA.");
      }

      // CSV (opcional)
      datosCSV = "Presion(mmHg)\n";
      for (int i = 0; i < muestraIndex; i++) {
        datosCSV += String(presionMuestras[i], 2) + "\n";
      }
      Serial.println("----- DATOS CSV -----");
      Serial.println(datosCSV);
      Serial.println("----- FIN DATOS -----");

      delay(6000);
      splash();
    }
  }

  delay(2);
}
