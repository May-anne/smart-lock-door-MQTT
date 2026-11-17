# Smart Lock — Tranca Eletrônica com RFID, MQTT e ESP32

Sistema de tranca eletrônica baseado em dois ESP32 que se comunicam via MQTT. Um lê cartões RFID e aciona a fechadura; o outro monitora a porta e dispara um alarme se ela for aberta sem autorização. Todas as tentativas de acesso são registradas em um banco PostgreSQL.

## Visão geral

```
┌──────────────────┐        ┌───────────────┐        ┌────────────────────┐
│ ESP32 #1         │ publica│               │ assina │ ESP32 #2           │
│ Leitor RFID      ├───────►│  Mosquitto    ├───────►│ Sensor da porta    │
│ + servo + LEDs   │        │  (broker MQTT)│        │ + buzzer + botão   │
└──────────────────┘        └───────┬───────┘        └────────────────────┘
                                    │ assina
                                    ▼
                          ┌───────────────────┐
                          │ Ponte Python      │
                          │ MQTT → PostgreSQL │
                          └───────────────────┘
```

## Componentes

| Módulo | Descrição |
|---|---|
| **ESP32 – Leitor RFID** | Lê o cartão (RC522), valida o UID contra uma lista local, aciona o servo (simula a trava), LEDs e buzzer, e publica a tentativa e o estado da fechadura. |
| **ESP32 – Sensor da porta** | Lê um sensor infravermelho que detecta a porta aberta/fechada. Dispara o alarme (buzzer) se a porta abrir sem um cartão autorizado válido. Possui botão para silenciar o alarme. |
| **Ponte Python** | Assina as tentativas de acesso no MQTT e grava cada uma no PostgreSQL. |

## Funcionamento

1. O usuário aproxima o cartão do leitor RC522.
2. O ESP32 #1 compara o UID com a lista de tags autorizadas e publica o resultado em `porta/principal/acesso/tentativa`.
3. Se autorizado: LED verde, bipe, servo abre por 3 s e volta a fechar. Se negado: LED vermelho e 3 bipes.
4. O ESP32 #2 recebe a tentativa. Se for autorizada, abre uma **janela de 10 s** em que a porta pode ser aberta sem alarme (vale para uma única abertura).
5. Se a porta abrir **fora da janela**, o alarme toca por até 30 s. Ele para ao passar um cartão autorizado, ao apertar o botão ou por tempo esgotado.
6. A ponte Python grava cada tentativa (UID + autorizado/negado) no banco.

## Hardware

- 2× ESP32
- 1× leitor RFID RC522 (SPI)
- 1× micro servo SG90
- 1× módulo sensor infravermelho (detecção da porta)
- LEDs verde e vermelho, 2× buzzer, 1× botão (push button)
- Um PC/servidor para o broker Mosquitto e o PostgreSQL

### Pinagem – ESP32 #1 (leitor RFID)

| Função | GPIO |
|---|---|
| RC522 MISO | 19 |
| RC522 MOSI | 23 |
| RC522 SCLK | 18 |
| RC522 SDA (CS) | 5 |
| RC522 RST | 22 |
| LED verde | 26 |
| LED vermelho | 25 |
| Buzzer | 27 |
| Servo (PWM) | 13 |

### Pinagem – ESP32 #2 (sensor da porta)

| Função | GPIO |
|---|---|
| Sensor infravermelho (OUT) | 32 |
| Botão de silenciar (entre o pino e GND) | 33 |
| Buzzer | 27 |

## Tópicos MQTT

| Tópico | Publicado por | Conteúdo |
|---|---|---|
| `porta/principal/acesso/tentativa` | ESP32 #1 | `{"uid":"71 ED F6 04","autorizado":true}` |
| `porta/principal/estado` | ESP32 #1 | `aberta` / `travada` (retain) |
| `porta/principal/status` | ESP32 #1 | `online` / `offline` (retain, via Last Will) |
| `porta/principal/sensor/status` | ESP32 #2 | `online` / `offline` (retain, via Last Will) |
| `porta/principal/sensor_porta` | ESP32 #2 | `aberta` / `fechada` (retain) |
| `porta/principal/alarme` | ESP32 #2 | `{"estado":"disparado"}`, `{"estado":"silenciado","motivo":"..."}` ou `{"estado":"normal"}` (retain) |
| `esp32/cmd` / `esp32/status` | — / ESP32 #1 | Comandos recebidos / status geral |

## Requisitos

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) (firmware dos ESP32)
- Componente [esp-idf-rc522](https://github.com/abobija/esp-idf-rc522) para o leitor RC522
- [Mosquitto](https://mosquitto.org/) (broker MQTT)
- PostgreSQL
- Python 3.9+ com `paho-mqtt` (2.x) e `psycopg2`

## Como rodar

### 1. Broker MQTT

Instale e inicie o Mosquitto no PC/servidor. Anote o IP da máquina na rede local.

### 2. Banco de dados

Crie o banco e a tabela. Exemplo de esquema:

```sql
CREATE DATABASE "smart-lock";

CREATE TABLE acessos (
    id          SERIAL PRIMARY KEY,
    uid         TEXT      NOT NULL,
    autorizado  BOOLEAN   NOT NULL,
    criado_em   TIMESTAMP NOT NULL DEFAULT NOW()
);
```

### 3. Ponte MQTT → PostgreSQL

```bash
pip install paho-mqtt psycopg2-binary
python ponte.py
```

Ajuste `MQTT_HOST` e `PG_CONFIG` no início do script conforme o seu ambiente.

### 4. Firmware dos ESP32

Em cada projeto, edite as constantes no topo do código:

- `WIFI_SSID` e `WIFI_PASS`: credenciais da sua rede Wi-Fi
- `MQTT_URI`: `mqtt://<IP_DO_BROKER>:1883`
- ESP32 #1: `TAGS_AUTORIZADAS` com os UIDs dos cartões permitidos
- ESP32 #2: `SENSOR_NIVEL_PORTA_ABERTA` (1 ou 0, conforme o seu módulo) e os tempos do alarme, se desejar

Depois compile e grave:

```bash
idf.py build
idf.py -p <PORTA_SERIAL> flash monitor
```

> O ESP32 #2 usa o `client_id` `esp32_sensor_porta`. Cada dispositivo conectado ao broker precisa de um ID diferente.

## Configurações úteis

| Constante | Padrão | Descrição |
|---|---|---|
| `PERIOD_ABERTA` | 3000 ms | Tempo que o servo fica aberto após cartão autorizado |
| `JANELA_LIBERACAO_MS` | 10000 ms | Tempo para abrir a porta após o cartão autorizado sem disparar alarme |
| `ALARME_DURACAO_MS` | 30000 ms | Duração máxima do alarme |
| `ALARME_PERIODO_MS` | 300 ms | Intervalo do bipe do alarme |

## Segurança

Este é um projeto didático. Antes de usar em algo real, considere:

- Não versionar credenciais de Wi-Fi, MQTT ou banco; use variáveis de ambiente ou arquivos de configuração fora do repositório.
- Habilitar usuário/senha (e idealmente TLS) no Mosquitto.
- UIDs de cartões RFID podem ser clonados; não são uma autenticação forte.
- A lista de tags autorizadas está fixa no firmware.

## Estrutura sugerida do repositório

```
.
├── esp32-leitor-rfid/     # firmware do ESP32 #1
├── esp32-sensor-porta/    # firmware do ESP32 #2
├── ponte/                 # script Python MQTT → PostgreSQL
└── README.md
```