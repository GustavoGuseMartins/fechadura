#include <Arduino.h>
#include <Keypad.h>
#include <Adafruit_Fingerprint.h>
#include <Preferences.h>
#include "mbedtls/md.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ==========================================
// CONFIGURAÇÕES DE HARDWARE
// ==========================================
#define FINGER_RX_PIN 25
#define FINGER_TX_PIN 26
#define TOUCH_PIN     27 // Bypass
#define MOTOR_PIN     32
#define SENSOR_PIN    14 // Fim de curso (Lingueta)
#define LED_PIN       15 // LED de Status / Feedback
#define MAG_PIN       22 // Sensor Magnético (Porta Aberta/Fechada)

const int velocidade   = 145; 
const int TEMPO_ABERTO = 300;
const int TIMEOUT_INIT = 10000;

// ==========================================
// CONFIGURAÇÕES DO TECLADO MATRICIAL
// ==========================================
const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'A','3','2','1'},
  {'B','6','5','4'},
  {'C','9','8','7'},
  {'D','#','0','*'}
};
byte rowPins[ROWS] = {19, 18, 5, 17}; 
byte colPins[COLS] = {2, 0, 4, 16}; 
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ==========================================
// VARIÁVEIS DE ESTADO E SISTEMA
// ==========================================
String senhaDigitada = "";
bool aguardandoSenhaMestre = false; 
bool aguardandoNovaSenha   = false;
const int TOTAL_USER_SLOTS = 5;

HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
Preferences preferences;

// ==========================================
// PROTÓTIPOS DAS FUNÇÕES
// ==========================================
void abrirFechadura();
int lerDigital();
void cadastrarNovaDigital();
uint8_t getFingerprintEnroll(int id);
void inicializarFechadura();
String gerarHashSHA256(String senhaLimpa);
void validarAcessoOuCadastro();
void salvarSenhaUsuario(String novoHash);
bool verificarTodasAsSenhas(String hashTentativa);
void piscarLED(int vezes, int tempo);

// ==========================================
// SETUP
// ==========================================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); 
  Serial.begin(115200);
  delay(1000);
  
  // Configuração dos Pinos
  pinMode(MOTOR_PIN, OUTPUT);
  analogWrite(MOTOR_PIN, 0); 
  
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW); // LED começa apagado

  pinMode(SENSOR_PIN, INPUT_PULLUP); 
  pinMode(MAG_PIN, INPUT_PULLUP); // LOW = Porta fechada, HIGH = Porta aberta
  pinMode(TOUCH_PIN, INPUT_PULLDOWN);

  // Inicialização Biometria
  mySerial.begin(57600, SERIAL_8N1, FINGER_RX_PIN, FINGER_TX_PIN);
  if (finger.verifyPassword()) {
    Serial.println("\n[ Sistema ] Sensor Biometrico OK!");
  } else {
    Serial.println("\n[ ERRO ] Sensor nao encontrado!");
    piscarLED(10, 100); // Pisca rápido se der erro grave no hardware
  }

  // Se a porta já estiver aberta quando ligar o ESP32, avisa no console
  if (digitalRead(MAG_PIN) == HIGH) {
    Serial.println("\n[ AVISO ] Porta detectada como ABERTA. Feche para trancar.");
    digitalWrite(LED_PIN, HIGH);
    while(digitalRead(MAG_PIN) == HIGH) delay(100); // Trava aqui até fecharem a porta
    digitalWrite(LED_PIN, LOW);
  }

  // Calibrar fechadura (Só roda se a porta estiver encostada!)
  Serial.println("\n[ INIT ] Inicializando posicao da fechadura...");
  inicializarFechadura();

  // Inicialização NVS
  preferences.begin("fechadura", false);
  String hashMestreSalvo = preferences.getString("senha_mestre", "");
  
  if (hashMestreSalvo == "") {
    Serial.println("[ INIT ] Gravando Hash mestre inicial... (Senha: 123456)");
    preferences.putString("senha_mestre", "8d969eef6ecad3c29a3a629280e686cf0c3f5d5a86aff3ca12020c923adc6c92");
  } else {
    Serial.println("[ INIT ] Senhas carregadas da memoria segura.");
  }
}

// ==========================================
// LOOP PRINCIPAL
// ==========================================
void loop() {
  char key = keypad.getKey();
  
  if (key) {
    if (key == '*') {
      senhaDigitada = "";
      aguardandoSenhaMestre = false;
      aguardandoNovaSenha = false;
      digitalWrite(LED_PIN, LOW); // Apaga LED se cancelar
      Serial.println("\n[ Cancelado ] Operacao resetada.");
    } 
    else if (key == '#') {
      if (senhaDigitada == "ABCD") {
        aguardandoSenhaMestre = true;
        digitalWrite(LED_PIN, HIGH); // Acende LED fixo indicando "Aguardando"
        Serial.println("\n[ MODO BIOMETRIA ] Digite a Senha Mestre para autorizar:");
        senhaDigitada = "";
      } 
      else if (senhaDigitada == "BCDA") {
        aguardandoSenhaMestre = true;
        aguardandoNovaSenha = true;
        digitalWrite(LED_PIN, HIGH); // Acende LED fixo
        Serial.println("\n[ MODO NOVA SENHA ] Digite a Senha Mestre para autorizar:");
        senhaDigitada = "";
      }
      else if (senhaDigitada == "9999") { 
        Serial.println("\n[ ATENCAO ] Apagando TODAS as digitais da memoria...");
        finger.emptyDatabase();
        Serial.println("[ OK ] Banco de dados limpo!");
        senhaDigitada = "";
        piscarLED(3, 500);
      }
      else {
        validarAcessoOuCadastro();
      }
    } 
    else {
      senhaDigitada += key;
      Serial.print("*");
    }
  }

  // Verificar Biometria continuamente
  int id = lerDigital();
  if (id > 0) {
    Serial.printf("\n[ OK ] Digital ID %d reconhecida!\n", id);
    abrirFechadura();
  }
} 

// ==========================================
// LÓGICA DE VALIDAÇÃO (SENHAS E HASHES)
// ==========================================
void validarAcessoOuCadastro() {
  String hashTentativa = gerarHashSHA256(senhaDigitada);
  String hashMestre = preferences.getString("senha_mestre", "");

  if (aguardandoSenhaMestre) {
    if (hashTentativa == hashMestre) {
      Serial.println("\n[ OK ] Mestre Autorizado!");
      piscarLED(2, 200); // Pisca indicando sucesso parcial
      
      if (aguardandoNovaSenha) {
        Serial.println("-> Agora digite a NOVA senha desejada e aperte #:");
        digitalWrite(LED_PIN, HIGH); // Mantém o LED aceso esperando a nova senha
        aguardandoSenhaMestre = false; 
      } else {
        cadastrarNovaDigital();
        aguardandoSenhaMestre = false;
        digitalWrite(LED_PIN, LOW); // Apaga LED ao fim do processo
      }
    } else {
      Serial.println("\n[ ERRO ] Senha Mestre Incorreta!");
      piscarLED(4, 100); // Pisca rápido indicando erro
      aguardandoSenhaMestre = false;
      aguardandoNovaSenha = false;
    }
  } 
  else if (aguardandoNovaSenha) {
    salvarSenhaUsuario(hashTentativa);
    piscarLED(3, 300); // Sucesso final
    aguardandoNovaSenha = false;
    digitalWrite(LED_PIN, LOW);
  }
  else {
    if (verificarTodasAsSenhas(hashTentativa)) {
      Serial.println("\n[ OK ] Acesso Liberado!");
      abrirFechadura();
    } else {
      Serial.println("\n[ ERRO ] Senha Invalida!");
      piscarLED(4, 100); // Erro de acesso
    }
  }
  
  senhaDigitada = ""; 
}

void salvarSenhaUsuario(String novoHash) {
  int slot = preferences.getInt("proximo_slot", 0);
  String chave = "user_" + String(slot);
  preferences.putString(chave.c_str(), novoHash);
  int proximo = (slot + 1) % TOTAL_USER_SLOTS;
  preferences.putInt("proximo_slot", proximo);
  Serial.printf("\n[ SUCESSO ] Nova senha salva na posicao %d de %d!\n", slot + 1, TOTAL_USER_SLOTS);
}

bool verificarTodasAsSenhas(String hashTentativa) {
  if (hashTentativa == preferences.getString("senha_mestre", "")) {
    Serial.println("-> Reconhecido: Administrador");
    return true;
  }
  for (int i = 0; i < TOTAL_USER_SLOTS; i++) {
    String chave = "user_" + String(i);
    if (hashTentativa == preferences.getString(chave.c_str(), "")) {
      Serial.printf("-> Reconhecido: Usuario %d\n", i + 1);
      return true;
    }
  }
  return false; 
}

String gerarHashSHA256(String senhaLimpa) {
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
  const size_t payloadLength = senhaLimpa.length();
  byte shaResult[32];

  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char *) senhaLimpa.c_str(), payloadLength);
  mbedtls_md_finish(&ctx, shaResult);
  mbedtls_md_free(&ctx);

  String hashStr = "";
  for (int i = 0; i < 32; i++) {
    char str[3];
    sprintf(str, "%02x", (int)shaResult[i]);
    hashStr += str;
  }
  return hashStr; 
}

// ==========================================
// FUNÇÕES DE MECÂNICA E FEEDBACK
// ==========================================
void piscarLED(int vezes, int tempo) {
  for (int i = 0; i < vezes; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(tempo);
    digitalWrite(LED_PIN, LOW);
    delay(tempo);
  }
}

void inicializarFechadura() {
  Serial.println("-> Movendo motor para posicao FECHADA...");
  unsigned long tempoInicio = millis();
  
  analogWrite(MOTOR_PIN, velocidade);
  
  while (digitalRead(SENSOR_PIN) == HIGH) { 
    if (millis() - tempoInicio > TIMEOUT_INIT) {
      Serial.println("[ ERRO FATAL ] Timeout - sensor de fim de curso nao acionado!");
      analogWrite(MOTOR_PIN, 0);
      piscarLED(10, 100);
      return;
    }
    delay(10);
  }
  
  analogWrite(MOTOR_PIN, 0);
  Serial.println("[ OK ] Fechadura trancada e calibrada com seguranca.");
}

void abrirFechadura() {
  Serial.println("-> Destrancando motor...");
  digitalWrite(LED_PIN, HIGH); // Acende o LED para mostrar que está liberado
  analogWrite(MOTOR_PIN, velocidade); 
  delay(TEMPO_ABERTO); 
  analogWrite(MOTOR_PIN, 0);
  Serial.println("-> Porta destrancada!");

  // --- NOVA LÓGICA COM SENSOR MAGNÉTICO ---
  Serial.println("\n[ Auto-Lock ] Aguardando 10 segundos...");
  delay(10000); 
  
  // Se após os 10s a porta estiver aberta, ele espera aqui infinitamente até fechar
  if (digitalRead(MAG_PIN) == HIGH) {
    Serial.println("[ Auto-Lock ] A porta esta aberta. Aguardando fechar...");
    // Fica piscando o LED de leve para avisar que a porta ficou aberta
    while(digitalRead(MAG_PIN) == HIGH) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN)); // Inverte o estado
      delay(500); 
    }
  }
  
  digitalWrite(LED_PIN, LOW); // Apaga ao confirmar que fechou
  Serial.println("[ Auto-Lock ] Porta fechada detectada! Trancando automaticamente...");
  inicializarFechadura(); 
}

// ==========================================
// FUNÇÕES DA BIOMETRIA
// ==========================================
int lerDigital() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK) return -1;
  return finger.fingerID;
}

void cadastrarNovaDigital() {
  int id = 1;
  Serial.println("\n[ Cadastro ] Procurando slot livre na memoria...");

  while (id <= 127) {
    if (finger.loadModel(id) != FINGERPRINT_OK) break; 
    id++;
  }

  if (id > 127) {
    Serial.println("[ ERRO ] Memoria do sensor cheia (127 digitais)!");
    piscarLED(5, 100);
    return;
  }

  Serial.printf("[ Cadastro ] Slot #%d esta livre. Iniciando...\n", id);
  digitalWrite(LED_PIN, HIGH); // Acende o LED durante todo o cadastro físico
  
  if (getFingerprintEnroll(id)) {
    Serial.printf("\n[ SUCESSO ] Digital salva fisicamente no leitor (ID #%d)!\n", id);
    piscarLED(3, 300);
  } else {
    Serial.println("\n[ FALHA ] Tempo esgotado ou erro de leitura. Tente novamente.");
    piscarLED(4, 100);
  }
  digitalWrite(LED_PIN, LOW);
}

uint8_t getFingerprintEnroll(int id) {
  int p = -1;
  Serial.printf("\n[ Cadastro ] Passo 1: Encoste o dedo no sensor (ID #%d)\n", id);
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) {
      Serial.println("-> Imagem capturada!");
    } else if (p != FINGERPRINT_NOFINGER) {
      Serial.println("-> Erro na captura. Limpe o vidro e tente novamente.");
    }
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) return 0; 
  
  Serial.println("-> Remova o dedo...");
  delay(2000);
  p = 0;
  while (p != FINGERPRINT_NOFINGER) {
    p = finger.getImage();
  }

  Serial.println("-> Passo 2: Coloque o MESMO dedo novamente para confirmacao...");
  p = -1;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) return 0; 

  Serial.println("-> Processando as minúcias e comparando...");
  p = finger.createModel();
  if (p == FINGERPRINT_OK) {
    Serial.println("-> Sucesso! As digitais batem perfeitamente.");
  } else if (p == FINGERPRINT_ENROLLMISMATCH) {
    Serial.println("[ ERRO ] As digitais nao sao iguais! Tente ser mais preciso na borda.");
    return 0; 
  } else {
    return 0; 
  }

  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    return 1; 
  } else {
    Serial.println("[ ERRO ] Falha ao gravar no chip do sensor.");
    return 0; 
  }
}