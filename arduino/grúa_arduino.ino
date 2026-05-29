#include <SoftwareSerial.h>

// Pines para motores DC (TB6612FNG 1)
#define AIN1 2   // Motor A (Carro) - Dirección 1
#define AIN2 4   // Motor A (Carro) - Dirección 2
#define PWMA 3   // Motor A (Carro) - PWM
#define BIN1 7   // Motor B (Elevación) - Dirección 1
#define BIN2 8   // Motor B (Elevación) - Dirección 2
#define PWMB 5   // Motor B (Elevación) - PWM

// Pines para motor DC (TB6612FNG 2)
#define CIN1 11  // Motor C (Giro) - Dirección 1
#define CIN2 12  // Motor C (Giro) - Dirección 2
#define PWMC 9   // Motor C (Giro) - PWM

// Pines para Joysticks
#define JOY_CARRO A0     // Joystick 1 Eje X -> Carro
#define JOY_GIRO A2      // Joystick 1 Eje Y -> Giro
#define JOY_ELEVACION A1  // Joystick 2 Eje X -> Elevación
#define BTN_MODE 6       // Pulsador Joystick 2 -> Conmutador de Modo

// Puerto Serie por Software para depuración hacia el ESP32
// D10 = RX (no usado para transmitir, pero requerido por SoftwareSerial)
// D13 = TX (conecta al RX GPIO 16 del ESP32)
SoftwareSerial debugSerial(10, 13);

// Velocidades máximas configurables para cada motor (rango PWM: 0 a 255)
const int MAX_SPEED_CARRO = 255;
const int MAX_SPEED_ELEVACION = 255;
const int MAX_SPEED_GIRO = 255;

// Configuración de Joystick
const int DEADZONE = 80; // Zona muerta analógica incrementada a 80 para filtrar drift/ruido y evitar movimiento en reposo

// Estado del sistema
bool manualMode = false; // false = Modo Web (por defecto), true = Modo Manual (Joysticks)
int currentGiroSpeed = 0; // Velocidad de giro actual para estimación de ángulo
float estimatedAngle = 0.0; // Ángulo estimado en punto flotante
long lastSentAngle = 0; // Último ángulo reportado por Serial

// Telemetría y Seguridad
unsigned long lastCommandTime = 0;
const unsigned long TIMEOUT = 2000; // 2 segundos de inactividad para frenado automático en modo Web
const unsigned long DEBOUNCE_DELAY = 50; // Retardo de anti-rebote (ms)

// Variables para debouncing del botón físico
int currentButtonState = HIGH;
int lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;

void setup() {
  // Configurar pines de dirección y PWM de motores como salidas
  pinMode(AIN1, OUTPUT);
  pinMode(AIN2, OUTPUT);
  pinMode(PWMA, OUTPUT);
  pinMode(BIN1, OUTPUT);
  pinMode(BIN2, OUTPUT);
  pinMode(PWMB, OUTPUT);
  pinMode(CIN1, OUTPUT);
  pinMode(CIN2, OUTPUT);
  pinMode(PWMC, OUTPUT);

  // Configurar pin del botón con resistencia pull-up interna
  pinMode(BTN_MODE, INPUT_PULLUP);

  // Iniciar comunicación serial principal (USB) a 9600 bps
  Serial.begin(9600);

  // Iniciar puerto de depuración por software (hacia el ESP32) a 9600 bps
  debugSerial.begin(9600);
  debugSerial.println("[SYSTEM] Iniciando sistema de grúa torre...");

  // Inicializar motores detenidos
  stopAllMotors();
  
  // Enviar estado inicial del modo
  sendModeUpdate();
}

void loop() {
  unsigned long now = millis();
  
  // 1. Debounce y lectura del botón físico (D6)
  int reading = digitalRead(BTN_MODE);
  if (reading != lastButtonState) {
    lastDebounceTime = now;
  }
  
  if ((now - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (reading != currentButtonState) {
      currentButtonState = reading;
      // Transición a presionado (el pin pasa a LOW cuando se pulsa)
      if (currentButtonState == LOW) {
        manualMode = !manualMode;
        sendModeUpdate();
        stopAllMotors();
      }
    }
  }
  lastButtonState = reading;

  // 2. Procesar comandos seriales desde USB
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    processCommand(command);
    lastCommandTime = now;
  }

  // 3. Control de motores según el modo activo
  if (manualMode) {
    // Lectura de los Joysticks físicos
    int devCarro = analogRead(JOY_CARRO) - 512;
    int devElev = analogRead(JOY_ELEVACION) - 512;
    int devGiro = analogRead(JOY_GIRO) - 512;

    int speedCarro = mapJoystick(devCarro, MAX_SPEED_CARRO);
    int speedElev = mapJoystick(devElev, MAX_SPEED_ELEVACION);
    int speedGiro = mapJoystick(devGiro, MAX_SPEED_GIRO);

    setMotorA(speedCarro);
    setMotorB(speedElev);
    setMotorC(speedGiro);
  } else {
    // Frenado automático por timeout en modo Web
    if (now - lastCommandTime > TIMEOUT) {
      stopAllMotors();
    }
  }

  // 4. Estimación del ángulo de rotación de la pluma (Giro)
  static unsigned long lastAngleTime = 0;
  if (lastAngleTime == 0) lastAngleTime = now;
  unsigned long dt = now - lastAngleTime;
  lastAngleTime = now;

  if (currentGiroSpeed != 0) {
    // A velocidad máxima (PWM 255), gira a 30 RPM (180 grados por segundo)
    float speedFactor = (float)currentGiroSpeed / 255.0;
    estimatedAngle += speedFactor * 180.0 * ((float)dt / 1000.0);

    // Normalizar ángulo entre [0, 360)
    if (estimatedAngle >= 360.0) estimatedAngle -= 360.0;
    if (estimatedAngle < 0.0) estimatedAngle += 360.0;
  }

  long angleInt = (long)estimatedAngle;
  if (angleInt != lastSentAngle) {
    lastSentAngle = angleInt;
    sendTelemetry();
  }
}

// Mapea la lectura del joystick analógico a un rango de velocidad PWM respetando la velocidad máxima
int mapJoystick(int dev, int maxSpeed) {
  if (abs(dev) < DEADZONE) {
    return 0;
  }
  int sign = (dev > 0) ? 1 : -1;
  int absDev = abs(dev);
  int speed = map(absDev, DEADZONE, 512, 0, maxSpeed);
  if (speed > maxSpeed) {
    speed = maxSpeed;
  }
  return sign * speed;
}

// Envía la telemetría en formato JSON a través del puerto serie principal USB (Serial)
void sendTelemetry() {
  Serial.print("{\"angle\":");
  Serial.print(lastSentAngle);
  Serial.print(",\"mode\":\"");
  Serial.print(manualMode ? "manual" : "web");
  Serial.println("\"}");
}

// Envía el estado actual del modo de control al depurador
void sendModeUpdate() {
  sendTelemetry();
  if (manualMode) {
    debugSerial.println("[SYSTEM] Modo cambiado a MANUAL.");
  } else {
    debugSerial.println("[SYSTEM] Modo cambiado a WEB.");
  }
}

// Procesa los comandos serie recibidos de la laptop por USB
void processCommand(String cmd) {
  if (cmd == "MM") {
    manualMode = true;
    sendModeUpdate();
    stopAllMotors();
    debugSerial.println("[CMD] Modo cambiado a MANUAL.");
  } else if (cmd == "MW") {
    manualMode = false;
    sendModeUpdate();
    stopAllMotors();
    debugSerial.println("[CMD] Modo cambiado a WEB.");
  } else if (!manualMode) {
    // Comandos de movimiento sólo permitidos en modo Web
    if (cmd == "CL") {
      setMotorA(-MAX_SPEED_CARRO);
      debugSerial.println("[CMD] Carro a la IZQUIERDA.");
    } else if (cmd == "CR") {
      setMotorA(MAX_SPEED_CARRO);
      debugSerial.println("[CMD] Carro a la DERECHA.");
    } else if (cmd == "EU") {
      setMotorB(MAX_SPEED_ELEVACION);
      debugSerial.println("[CMD] Elevación ARRIBA.");
    } else if (cmd == "ED") {
      setMotorB(-MAX_SPEED_ELEVACION);
      debugSerial.println("[CMD] Elevación ABAJO.");
    } else if (cmd == "GL") {
      setMotorC(-MAX_SPEED_GIRO);
      debugSerial.println("[CMD] Giro a la IZQUIERDA.");
    } else if (cmd == "GR") {
      setMotorC(MAX_SPEED_GIRO);
      debugSerial.println("[CMD] Giro a la DERECHA.");
    } else if (cmd == "S") {
      stopAllMotors();
      debugSerial.println("[CMD] Motores DETENIDOS.");
    }
  }
}

// Controla el Motor A (Carro)
void setMotorA(int speed) {
  if (speed > 0) {
    digitalWrite(AIN1, HIGH);
    digitalWrite(AIN2, LOW);
    analogWrite(PWMA, speed);
  } else if (speed < 0) {
    digitalWrite(AIN1, LOW);
    digitalWrite(AIN2, HIGH);
    analogWrite(PWMA, -speed);
  } else {
    digitalWrite(AIN1, LOW);
    digitalWrite(AIN2, LOW);
    analogWrite(PWMA, 0);
  }
}

// Controla el Motor B (Elevación)
void setMotorB(int speed) {
  if (speed > 0) {
    digitalWrite(BIN1, HIGH);
    digitalWrite(BIN2, LOW);
    analogWrite(PWMB, speed);
  } else if (speed < 0) {
    digitalWrite(BIN1, LOW);
    digitalWrite(BIN2, HIGH);
    analogWrite(PWMB, -speed);
  } else {
    digitalWrite(BIN1, LOW);
    digitalWrite(BIN2, LOW);
    analogWrite(PWMB, 0);
  }
}

// Controla el Motor C (Giro)
void setMotorC(int speed) {
  if (speed > 0) {
    digitalWrite(CIN1, HIGH);
    digitalWrite(CIN2, LOW);
    analogWrite(PWMC, speed);
    currentGiroSpeed = speed;
  } else if (speed < 0) {
    digitalWrite(CIN1, LOW);
    digitalWrite(CIN2, HIGH);
    analogWrite(PWMC, -speed);
    currentGiroSpeed = speed;
  } else {
    digitalWrite(CIN1, LOW);
    digitalWrite(CIN2, LOW);
    analogWrite(PWMC, 0);
    currentGiroSpeed = 0;
  }
}

// Detiene todos los motores
void stopAllMotors() {
  setMotorA(0);
  setMotorB(0);
  setMotorC(0);
}
