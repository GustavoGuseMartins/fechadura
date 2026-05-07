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
#define TOUCH_PIN     27 // Mantido no setup, mas ignorado no loop (bypass)
#define MOTOR_PIN     32
#define SENSOR_PIN    14 // Sensor de posição fechada (Fim de curso)

const int velocidade   = 105; 
const int TEMPO_ABERTO = 500;
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
const int TOTAL_USER_SLOTS = 5; // Limite de 5 senhas de usuários comuns

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

// ==========================================
// SETUP
// ==========================================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Desliga detector de queda de tensão
  Serial.begin(115200);
  delay(1000);
  
  // Configuração dos Pinos
  pinMode(MOTOR_PIN, OUTPUT);
  analogWrite(MOTOR_PIN, 0); // Garante motor desligado
  pinMode(SENSOR_PIN, INPUT_PULLUP); // O sensor joga para GND (LOW) quando porta fecha
  pinMode(TOUCH_PIN, INPUT_PULLDOWN);

  // Inicialização Biometria
  mySerial.begin(57600, SERIAL_8N1, FINGER_RX_PIN, FINGER_TX_PIN);
  if (finger.verifyPassword()) {
    Serial.println("\n[ Sistema ] Sensor Biometrico OK!");
  } else {
    Serial.println("\n[ ERRO ] Sensor nao encontrado!");
  }

  // Calibrar fechadura
  Serial.println("\n[ INIT ] Inicializando posicao da fechadura...");
  inicializarFechadura();

  // Inicialização NVS (Preferências)
  preferences.begin("fechadura", false);
  String hashMestreSalvo = preferences.getString("senha_mestre", "");
  
  // Grava o Hash inicial apenas se a memória estiver vazia
  if (hashMestreSalvo == "") {
    Serial.println("[ INIT ] Gravando Hash mestre inicial... (Senha: 123456)");
    preferences.putString("senha_mestre", "8d969eef6ecad3c29a3a629280e686cf0c3f5d5a86aff3ca12020c923adc6c92");
  } else {
    Serial.println("[ INIT ] Senha mestre e usuarios carregados da memoria segura.");
  }
}

// ==========================================
// LOOP PRINCIPAL
// ==========================================
void loop() {
  char key = keypad.getKey();
  
  if (key) {
    // BOTÃO DE CANCELAR / LIMPAR
    if (key == '*') {
      senhaDigitada = "";
      aguardandoSenhaMestre = false;
      aguardandoNovaSenha = false;
      Serial.println("\n[ Cancelado ] Operacao resetada.");
    } 
    // BOTÃO DE CONFIRMAR
    else if (key == '#') {
      // COMANDO 1: Cadastrar nova Biometria
      if (senhaDigitada == "ABCD") {
        aguardandoSenhaMestre = true;
        Serial.println("\n[ MODO BIOMETRIA ] Digite a Senha Mestre para autorizar:");
        senhaDigitada = "";
      } 
      // COMANDO 2: Cadastrar nova Senha Numérica
      else if (senhaDigitada == "DCAB") {
        aguardandoSenhaMestre = true;
        aguardandoNovaSenha = true;
        Serial.println("\n[ MODO NOVA SENHA ] Digite a Senha Mestre para autorizar:");
        senhaDigitada = "";
      }
      // COMANDO 3: Apagar todas as biometrias
      else if (senhaDigitada == "9999") { 
        Serial.println("\n[ ATENCAO ] Apagando TODAS as digitais da memoria...");
        finger.emptyDatabase();
        Serial.println("[ OK ] Banco de dados limpo!");
        senhaDigitada = "";
      }
      // AVALIAR O QUE FOI DIGITADO
      else {
        validarAcessoOuCadastro();
      }
    } 
    // CONSTRUIR A SENHA
    else {
      senhaDigitada += key;
      Serial.print("*"); // Feedback no Serial sem vazar a senha
    }
  }

  // Verificar Biometria continuamente (gambiarra)
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

  // 1. ESTAMOS TENTANDO AUTORIZAR UM CADASTRO?
  if (aguardandoSenhaMestre) {
    if (hashTentativa == hashMestre) {
      Serial.println("\n[ OK ] Mestre Autorizado!");
      
      if (aguardandoNovaSenha) {
        Serial.println("-> Agora digite a NOVA senha desejada e aperte #:");
        aguardandoSenhaMestre = false; // Limpa para a próxima etapa ler como senha nova
      } else {
        cadastrarNovaDigital();
        aguardandoSenhaMestre = false;
      }
    } else {
      Serial.println("\n[ ERRO ] Senha Mestre Incorreta!");
      aguardandoSenhaMestre = false;
      aguardandoNovaSenha = false;
    }
  } 
  // 2. ESTAMOS NO PASSO FINAL DE GRAVAR UMA SENHA NOVA?
  else if (aguardandoNovaSenha) {
    salvarSenhaUsuario(hashTentativa);
    aguardandoNovaSenha = false;
  }
  // 3. É UMA TENTATIVA NORMAL DE ABRIR A PORTA
  else {
    if (verificarTodasAsSenhas(hashTentativa)) {
      Serial.println("\n[ OK ] Acesso Liberado!");
      abrirFechadura();
    } else {
      Serial.println("\n[ ERRO ] Senha Invalida!");
    }
  }
  
  senhaDigitada = ""; // Limpa o buffer de digitação em qualquer cenário
}

void salvarSenhaUsuario(String novoHash) {
  int slot = preferences.getInt("proximo_slot", 0);
  String chave = "user_" + String(slot);
  
  preferences.putString(chave.c_str(), novoHash);
  
  // Lógica circular: quando chegar em 4, o próximo volta a ser 0
  int proximo = (slot + 1) % TOTAL_USER_SLOTS;
  preferences.putInt("proximo_slot", proximo);
  
  Serial.printf("\n[ SUCESSO ] Nova senha salva na posicao %d de %d!\n", slot + 1, TOTAL_USER_SLOTS);
}

bool verificarTodasAsSenhas(String hashTentativa) {
  // Testa a senha Mestre primeiro
  if (hashTentativa == preferences.getString("senha_mestre", "")) {
    Serial.println("-> Reconhecido: Administrador");
    return true;
  }

  // Varre os slots de usuários comuns
  for (int i = 0; i < TOTAL_USER_SLOTS; i++) {
    String chave = "user_" + String(i);
    if (hashTentativa == preferences.getString(chave.c_str(), "")) {
      Serial.printf("-> Reconhecido: Usuario %d\n", i + 1);
      return true;
    }
  }
  
  return false; // Não bateu com nenhuma do sistema
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
// FUNÇÕES DE MECÂNICA (MOTOR E SENSOR)
// ==========================================
void inicializarFechadura() {
  Serial.println("-> Movendo motor para posicao FECHADA...");
  unsigned long tempoInicio = millis();
  
  analogWrite(MOTOR_PIN, velocidade);
  
  // Com o PULLUP, HIGH significa que o botão do sensor NÃO está sendo pressionado
  while (digitalRead(SENSOR_PIN) == HIGH) { 
    if (millis() - tempoInicio > TIMEOUT_INIT) {
      Serial.println("[ ERRO FATAL ] Timeout - sensor de fim de curso nao acionado!");
      analogWrite(MOTOR_PIN, 0);
      return;
    }
    delay(10);
  }
  
  analogWrite(MOTOR_PIN, 0);
  Serial.println("[ OK ] Fechadura trancada e calibrada com seguranca.");
}

void abrirFechadura() {
  Serial.println("-> Destrancando motor...");
  analogWrite(MOTOR_PIN, velocidade); 
  delay(TEMPO_ABERTO); // Força necessária para abrir a lingueta
  analogWrite(MOTOR_PIN, 0);
  Serial.println("-> Porta destrancada!");

  // Lógica Auto-Lock Integrada
  Serial.println("\n[ Auto-Lock ] Aguardando 10 segundos para trancar...");
  delay(10000); 
  
  Serial.println("[ Auto-Lock ] Trancando a porta automaticamente...");
  inicializarFechadura(); // Motor gira até bater no sensor físico
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
    if (finger.loadModel(id) != FINGERPRINT_OK) {
      break; 
    }
    id++;
  }

  if (id > 127) {
    Serial.println("[ ERRO ] Memoria do sensor cheia (127 digitais)!");
    return;
  }

  Serial.printf("[ Cadastro ] Slot #%d esta livre. Iniciando...\n", id);
  
  if (getFingerprintEnroll(id)) {
    Serial.printf("\n[ SUCESSO ] Digital salva fisicamente no leitor (ID #%d)!\n", id);
  } else {
    Serial.println("\n[ FALHA ] Tempo esgotado ou erro de leitura. Tente novamente.");
  }
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
  if (p != FINGERPRINT_OK) {
    Serial.println("[ ERRO ] Falha ao converter primeira imagem.");
    return 0; 
  }
  
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
  if (p != FINGERPRINT_OK) {
    Serial.println("[ ERRO ] Falha ao converter segunda imagem.");
    return 0; 
  }

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