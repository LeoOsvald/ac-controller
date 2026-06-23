// ============================================================
//  CONTROLE AUTOMÁTICO DE VENTILAÇÃO
//  Sensores: RFID RC522 + DHT11
//  Saídas:   LED Verde, LCD 16x2
//  Comunicação: Serial USB → Raspberry Pi
// ============================================================

// Importa a biblioteca de comunicação SPI (necessária para o RFID)
#include <SPI.h>

// Importa a biblioteca do módulo RFID RC522
#include <MFRC522.h>

// Importa a biblioteca do sensor de temperatura e umidade DHT
#include <DHT.h>

// Importa a biblioteca do display LCD no modo paralelo
#include <LiquidCrystal.h>

// ── Define os pinos do RFID ──────────────────────────────────
#define SS_PIN   10   // Pino SDA do RC522 (Chip Select do SPI)
#define RST_PIN  9    // Pino de reset do RC522

// ── Define os pinos das saídas ───────────────────────────────
#define DHT_PIN    2  // Pino de dados do sensor DHT11
#define LED_VERDE  A1 // Pino do LED verde (indica sistema ativo)

// ── Define o tipo do sensor de temperatura ───────────────────
#define DHT_TYPE DHT11  // Modelo do sensor (DHT11 no hardware real)

// ── Cria os objetos dos componentes ─────────────────────────
MFRC522 rfid(SS_PIN, RST_PIN);   // Objeto do módulo RFID
DHT     dht(DHT_PIN, DHT_TYPE);  // Objeto do sensor de temperatura

// LCD no modo paralelo 4 bits: RS=3, EN=4, D4=5, D5=6, D6=7, D7=8
LiquidCrystal lcd(3, 4, 5, 6, 7, 8);

// ── Variáveis de controle ────────────────────────────────────

// Guarda quantos alunos estão presentes na sala
int alunosPresentes = 0;

// Guarda o UID do último cartão lido (para anti-bounce)
String ultimoCartao = "";

// Guarda o momento da última leitura de cartão (em milissegundos)
unsigned long ultimaLeitura = 0;

// Guarda o momento em que o LCD foi "bloqueado" para exibir mensagem temporária
unsigned long tempoBloqueioLCD = 0;

// Flag que indica se o LCD está exibindo uma mensagem temporária
bool lcdBloqueado = false;

// Temperatura limite para ligar o sistema (pode ser alterada via serial)
float TEMP_LIMITE = 28.0;

// ── Cartões RFID cadastrados ─────────────────────────────────
// Cada cartão tem 4 bytes de UID no formato hexadecimal
// Para descobrir o UID, use o sketch de teste do RFID
const byte CARTOES[][4] = {
  {0xA3, 0x39, 0x82, 0x11},  // Cartão do aluno 1
  {0x33, 0x15, 0x78, 0x09},  // Cartão do aluno 2
};

// Calcula automaticamente quantos cartões existem na tabela
const int TOTAL_CARTOES = sizeof(CARTOES) / sizeof(CARTOES[0]);

// Array que guarda se cada cartão está dentro ou fora da sala
// true = aluno está dentro | false = aluno está fora
bool cartaoPresente[10] = {false};

// Buffer para montar os comandos recebidos via serial caractere a caractere
String bufferSerial = "";

// ============================================================
//  SETUP — Executado uma única vez ao ligar o Arduino
// ============================================================
void setup() {

  // Inicia a comunicação serial com o Raspberry Pi a 9600 baud
  Serial.begin(9600);

  // Configura o pino do LED como saída
  pinMode(LED_VERDE, OUTPUT);

  // Garante que o LED começa apagado
  digitalWrite(LED_VERDE, LOW);

  // Pisca o LED 3 vezes para confirmar que o sistema iniciou
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_VERDE, HIGH); // Acende o LED
    delay(200);                    // Aguarda 200ms
    digitalWrite(LED_VERDE, LOW);  // Apaga o LED
    delay(200);                    // Aguarda 200ms
  }

  // Inicia o barramento SPI (necessário para o RFID)
  SPI.begin();

  // Inicializa o módulo RFID RC522
  rfid.PCD_Init();

  // Define o ganho máximo da antena do RFID para melhor alcance
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);

  // Inicia o sensor de temperatura DHT
  dht.begin();

  // Inicia o LCD com 16 colunas e 2 linhas
  lcd.begin(16, 2);

  // Exibe mensagem de boas-vindas na linha 1
  lcd.setCursor(0, 0);
  lcd.print("  Ventilacao    ");

  // Exibe mensagem de boas-vindas na linha 2
  lcd.setCursor(0, 1);
  lcd.print("  Automatica    ");

  // Aguarda 2 segundos para o usuário ver a mensagem
  delay(2000);

  // Limpa o LCD para começar a exibir os dados reais
  lcd.clear();

  // Informa ao Raspberry Pi que o sistema iniciou
  Serial.println("Sistema iniciado!");

  // Envia o limite de temperatura atual para o Raspberry Pi
  Serial.print("TEMP_LIMITE:");
  Serial.println(TEMP_LIMITE, 1);

  // Atualiza o LCD com a contagem inicial de alunos (zero)
  atualizarDisplay();
}

// ============================================================
//  LOOP — Executado repetidamente enquanto o Arduino estiver ligado
// ============================================================
void loop() {

  // Verifica se chegou algum comando do Raspberry Pi via serial
  verificarComandoSerial();

  // Verifica se algum cartão RFID foi aproximado
  verificarCartao();

  // Verifica a temperatura e decide se liga ou desliga o sistema
  verificarTemperatura();

  // Verifica se o LCD pode voltar a exibir os dados normais
  liberarLCD();

  // Sem delay aqui — o loop roda o mais rápido possível
  // para não perder leituras do RFID
}

// ============================================================
//  Lê comandos enviados pelo Raspberry Pi via serial
//  Usa leitura não-bloqueante (caractere por caractere)
//  para não travar o loop e perder leituras do RFID
// ============================================================
void verificarComandoSerial() {

  // Enquanto houver bytes disponíveis na serial, lê um por vez
  while (Serial.available()) {

    // Lê o próximo caractere disponível
    char c = Serial.read();

    // Se encontrou fim de linha, o comando está completo
    if (c == '\n') {

      // Remove espaços e quebras de linha extras
      bufferSerial.trim();

      // Verifica se o comando é para alterar o limite de temperatura
      if (bufferSerial.startsWith("SETTEMP:")) {

        // Extrai o valor numérico após "SETTEMP:"
        float novo = bufferSerial.substring(8).toFloat();

        // Valida se o valor está dentro do intervalo permitido (10 a 40°C)
        if (novo >= 10.0 && novo <= 40.0) {

          // Atualiza o limite de temperatura na memória
          TEMP_LIMITE = novo;

          // Confirma ao Raspberry Pi que o limite foi atualizado
          Serial.print("TEMP_LIMITE_ATUALIZADO:");
          Serial.println(TEMP_LIMITE, 1);

          // Exibe o novo limite brevemente no LCD
          lcd.setCursor(0, 1);
          lcd.print("Limite:");
          lcd.print(TEMP_LIMITE, 1);
          lcd.print((char)223); // Caractere especial do símbolo °
          lcd.print("C     ");

          // Bloqueia o LCD por 1.5s para mostrar a mensagem
          lcdBloqueado     = true;
          tempoBloqueioLCD = millis();

        } else {
          // Valor fora do intervalo — informa o erro
          Serial.println("ERRO: fora do range (10-40)");
        }
      }

      // Limpa o buffer para receber o próximo comando
      bufferSerial = "";

    } else {
      // Ainda não chegou o fim do comando — acumula o caractere
      bufferSerial += c;
    }
  }
}

// ============================================================
//  Libera o LCD após 1.5s de mensagem temporária
//  Usa millis() para não bloquear o loop com delay()
// ============================================================
void liberarLCD() {

  // Verifica se o LCD está bloqueado e se já passou 1.5 segundos
  if (lcdBloqueado && millis() - tempoBloqueioLCD >= 1500) {

    // Libera o LCD
    lcdBloqueado = false;

    // Atualiza o display com os dados normais
    atualizarDisplay();
  }
}

// ============================================================
//  Verifica se algum cartão RFID foi aproximado
//  Alterna o estado do aluno (entrada/saída) e atualiza a contagem
// ============================================================
void verificarCartao() {

  // Variáveis estáticas — mantêm o valor entre chamadas do loop
  static unsigned long ultimoReset    = 0; // Momento do último reset do RFID
  static unsigned long ultimaLeituraOk = 0; // Momento da última leitura bem-sucedida

  // Watchdog: se ficou 10s sem ler nenhum cartão, reinicia o módulo RFID
  // Isso resolve travamentos silenciosos que acontecem com o RC522
  if (millis() - ultimaLeituraOk > 10000 && millis() - ultimoReset > 10000) {
    rfid.PCD_Init();                        // Reinicia o módulo
    rfid.PCD_SetAntennaGain(rfid.RxGain_max); // Restaura o ganho máximo
    ultimoReset = millis();                 // Registra o momento do reset
  }

  // Se não há nenhum cartão novo no campo do leitor, encerra a função
  if (!rfid.PICC_IsNewCardPresent()) return;

  // Se não conseguiu ler o serial do cartão, encerra a função
  if (!rfid.PICC_ReadCardSerial()) return;

  // Registra o momento da última leitura bem-sucedida (para o watchdog)
  ultimaLeituraOk = millis();

  // Monta a string do UID lido em formato hexadecimal
  String uidStr = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uidStr += "0"; // Adiciona zero à esquerda se necessário
    uidStr += String(rfid.uid.uidByte[i], HEX);     // Converte byte para hex
  }

  // Anti-bounce: ignora se o mesmo cartão foi lido há menos de 2 segundos
  // Evita que uma única passagem seja contada duas vezes
  if (uidStr == ultimoCartao && millis() - ultimaLeitura < 2000) {
    rfid.PICC_HaltA();      // Para a comunicação com o cartão
    rfid.PCD_StopCrypto1(); // Encerra a criptografia
    return;
  }

  // Atualiza o controle anti-bounce com o cartão atual
  ultimoCartao  = uidStr;
  ultimaLeitura = millis();

  // Envia o UID lido para o Raspberry Pi
  Serial.print("Cartao lido: ");
  Serial.println(uidStr);

  // Busca o cartão lido na tabela de cartões cadastrados
  int indice = buscarCartao(rfid.uid.uidByte, rfid.uid.size);

  // Verifica se o cartão está cadastrado
  if (indice >= 0) {

    // Se o cartão já estava dentro → é uma saída
    if (cartaoPresente[indice]) {
      cartaoPresente[indice] = false;                    // Marca como fora
      alunosPresentes = max(0, alunosPresentes - 1);    // Decrementa (mínimo 0)
      Serial.print("SAIDA:");                            // Informa ao Raspberry Pi

    } else {
      // Se o cartão estava fora → é uma entrada
      cartaoPresente[indice] = true; // Marca como dentro
      alunosPresentes++;             // Incrementa a contagem
      Serial.print("ENTRADA:");     // Informa ao Raspberry Pi
    }

    // Envia o UID e a nova contagem ao Raspberry Pi
    Serial.println(uidStr);
    Serial.print("ALUNOS:");
    Serial.println(alunosPresentes);

    // Atualiza o LCD com a nova contagem
    atualizarDisplay();

  } else {
    // Cartão não cadastrado — informa ao Raspberry Pi
    Serial.print("CARTAO_INVALIDO:");
    Serial.println(uidStr);
  }

  // Encerra a comunicação com o cartão atual
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

// ============================================================
//  Lê a temperatura e decide se liga ou desliga o sistema
//  Executa a cada 3 segundos usando millis() (sem delay)
// ============================================================
void verificarTemperatura() {

  // Variável estática para controlar o intervalo de leitura
  static unsigned long ultimaLeituraTemp = 0;

  // Se ainda não passaram 3 segundos desde a última leitura, encerra
  if (millis() - ultimaLeituraTemp < 3000) return;

  // Registra o momento desta leitura
  ultimaLeituraTemp = millis();

  // Lê a temperatura em graus Celsius
  float temp = dht.readTemperature();

  // Lê a umidade relativa em percentual
  float umid = dht.readHumidity();

  // Verifica se as leituras são válidas (NaN = sensor não respondeu)
  if (isnan(temp) || isnan(umid)) {
    Serial.println("ERRO: DHT nao responde!");
    return;
  }

  // Envia temperatura e umidade ao Raspberry Pi no formato padrão
  Serial.print("TEMP:");
  Serial.print(temp, 1);   // Uma casa decimal
  Serial.print(";UMID:");
  Serial.println(umid, 1); // Uma casa decimal

  // Lógica de decisão:
  // Liga SE há alunos presentes E a temperatura está acima do limite
  bool ligar = (alunosPresentes > 0) && (temp >= TEMP_LIMITE);

  if (ligar) {
    // Condições atendidas — acende o LED e informa ao Raspberry Pi
    digitalWrite(LED_VERDE, HIGH);
    Serial.println("VENTILADOR:LIGADO");
  } else {
    // Condições não atendidas — apaga o LED e informa ao Raspberry Pi
    digitalWrite(LED_VERDE, LOW);
    Serial.println("VENTILADOR:DESLIGADO");
  }

  // Atualiza a linha 2 do LCD com temperatura e estado
  // Só atualiza se o LCD não estiver exibindo mensagem temporária
  if (!lcdBloqueado) {
    lcd.setCursor(0, 1);
    lcd.print("Temp:");
    lcd.print(temp, 1);
    lcd.print((char)223); // Símbolo °
    lcd.print("C ");
    if (ligar) {
      lcd.print("[ON] "); // Sistema ativo
    } else {
      lcd.print("[OFF]"); // Sistema inativo
    }
  }
}

// ============================================================
//  Atualiza a linha 1 do LCD com a contagem de alunos
// ============================================================
void atualizarDisplay() {

  // Só atualiza se o LCD não estiver exibindo mensagem temporária
  if (lcdBloqueado) return;

  // Posiciona o cursor no início da linha 1
  lcd.setCursor(0, 0);

  // Exibe a contagem de alunos
  lcd.print("Alunos: ");
  lcd.print(alunosPresentes);

  // Preenche o restante com espaços para limpar caracteres antigos
  lcd.print("       ");
}

// ============================================================
//  Busca um UID lido na tabela de cartões cadastrados
//  Retorna o índice do cartão se encontrado, -1 se não cadastrado
// ============================================================
int buscarCartao(byte* uid, byte tamanho) {

  // O RC522 lê UIDs de 4 bytes — qualquer outro tamanho é inválido
  if (tamanho != 4) return -1;

  // Percorre todos os cartões cadastrados
  for (int i = 0; i < TOTAL_CARTOES; i++) {
    bool igual = true;

    // Compara os 4 bytes do UID lido com o cartão cadastrado
    for (int j = 0; j < 4; j++) {
      if (uid[j] != CARTOES[i][j]) {
        igual = false; // Encontrou diferença — não é esse cartão
        break;
      }
    }

    // Se todos os 4 bytes batem, encontrou o cartão
    if (igual) return i;
  }

  // Nenhum cartão da tabela corresponde ao UID lido
  return -1;
}