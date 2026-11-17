#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define WIFI_SSID   "REDMI 17 May"
#define WIFI_PASS   "Mayanne12"
#define MQTT_URI    "mqtt://192.168.198.37:1883"   // IP do PC/servidor com o Mosquitto
#define MQTT_USER   NULL                           
#define MQTT_PASS   NULL                           
#define MQTT_CLIENT_ID "esp32_sensor_porta"        

// Topicos (mesmo prefixo da ESP do leitor RFID)
#define TOPIC_ACESSO_TENTATIVA  "porta/principal/acesso/tentativa"   // assina: recebe leituras do RFID
#define TOPIC_STATUS_SENSOR     "porta/principal/sensor/status"      // online/offline desta ESP
#define TOPIC_SENSOR_PORTA      "porta/principal/sensor_porta"       // "aberta" / "fechada"
#define TOPIC_ALARME            "porta/principal/alarme"             // JSON com o estado do alarme

// Pinagem
#define PIN_BUZZER        GPIO_NUM_27
#define PIN_SENSOR_PORTA  GPIO_NUM_32   // OUT do modulo infravermelho
#define PIN_BOTAO_ALARME  GPIO_NUM_33   // botao entre o GPIO33 e o GND

// Nivel lido no sensor quando a porta esta ABERTA (1 ou 0).
#define SENSOR_NIVEL_PORTA_ABERTA  1

// Tempos do alarme
#define JANELA_LIBERACAO_MS   10000   // tempo para abrir a porta apos cartao autorizado
#define ALARME_DURACAO_MS     30000   // o alarme toca ate 30 s se ninguem silenciar
#define ALARME_PERIODO_MS       300   // bipe: 300 ms ligado / 300 ms desligado

// Leitura do sensor
#define POLL_PORTA_MS              20
#define SENSOR_AMOSTRAS_ESTAVEIS    5   // 5 x 20 ms = 100 ms estavel (anti-ruido)
// =========================

static const char *TAG = "SENSOR_PORTA";
static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_conectado = false;

// Estado da porta e do alarme (usados por mais de uma task)
static volatile bool s_porta_aberta = false;
static volatile bool s_alarme_ativo = false;
static volatile bool s_liberacao_ativa = false;     // cartao autorizado valendo
static volatile TickType_t s_liberacao_ate = 0;     // ate quando vale
static TickType_t s_alarme_inicio = 0;

/* ---------- MQTT: publicacao segura ---------- */
static void mqtt_pub(const char *topic, const char *msg, bool retain)
{
    if (s_mqtt_client && s_mqtt_conectado) {
        esp_mqtt_client_publish(s_mqtt_client, topic, msg, 0, 1, retain);
    }
}

/* ---------- Wi-Fi ---------- */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi caiu, reconectando...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP obtido: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // espera conectar antes de seguir
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
}

/* ---------- Alarme ---------- */
static void alarme_disparar(void)
{
    if (s_alarme_ativo) {
        return;
    }
    s_alarme_inicio = xTaskGetTickCount();
    s_alarme_ativo = true;
    ESP_LOGW(TAG, "ALARME! Porta aberta sem autorizacao.");
    mqtt_pub(TOPIC_ALARME, "{\"estado\":\"disparado\"}", true);
}

// Quem apaga o buzzer de fato e a tarefa_porta (evita conflito entre tasks).
static void alarme_silenciar(const char *motivo)
{
    if (!s_alarme_ativo) {
        return;
    }
    s_alarme_ativo = false;
    ESP_LOGW(TAG, "Alarme silenciado (%s).", motivo);

    char payload[96];
    snprintf(payload, sizeof(payload),
             "{\"estado\":\"silenciado\",\"motivo\":\"%s\"}", motivo);
    mqtt_pub(TOPIC_ALARME, payload, true);
}

// Chamada quando chega, pelo MQTT, uma leitura de cartao AUTORIZADO
// (publicada pela ESP do leitor RFID em TOPIC_ACESSO_TENTATIVA).
static void cartao_autorizado_recebido(void)
{
    // Abre a janela em que a porta pode ser aberta sem disparar o alarme
    s_liberacao_ate = xTaskGetTickCount() + pdMS_TO_TICKS(JANELA_LIBERACAO_MS);
    s_liberacao_ativa = true;
    ESP_LOGI(TAG, "Cartao autorizado recebido: janela de %d ms aberta.", JANELA_LIBERACAO_MS);

    // Cartao autorizado tambem silencia um alarme que esteja tocando
    alarme_silenciar("cartao autorizado");
}

/* ---------- Sensor da porta e botao ---------- */
static void porta_init(void)
{
    gpio_config_t buzzer = {
        .pin_bit_mask = 1ULL << PIN_BUZZER,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&buzzer);
    gpio_set_level(PIN_BUZZER, 0);

    gpio_config_t sensor = {
        .pin_bit_mask = 1ULL << PIN_SENSOR_PORTA,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&sensor);

    gpio_config_t botao = {
        .pin_bit_mask = 1ULL << PIN_BOTAO_ALARME,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   // botao liga o pino ao GND
    };
    gpio_config(&botao);
}

static bool sensor_porta_aberta_bruto(void)
{
    return gpio_get_level(PIN_SENSOR_PORTA) == SENSOR_NIVEL_PORTA_ABERTA;
}

// Chamada quando o estado da porta muda (ja sem ruido)
static void porta_mudou(bool aberta)
{
    s_porta_aberta = aberta;
    mqtt_pub(TOPIC_SENSOR_PORTA, aberta ? "aberta" : "fechada", true);

    if (!aberta) {
        ESP_LOGI(TAG, "Porta fechada.");
        return;
    }

    // Porta abriu: existe um cartao autorizado ainda valendo?
    bool liberada = s_liberacao_ativa &&
                    ((int32_t)(s_liberacao_ate - xTaskGetTickCount()) > 0);
    s_liberacao_ativa = false;   // a autorizacao vale para uma abertura so

    if (liberada) {
        ESP_LOGI(TAG, "Porta aberta COM autorizacao.");
    } else {
        ESP_LOGW(TAG, "Porta aberta SEM autorizacao.");
        alarme_disparar();
    }
}

static void tarefa_porta(void *arg)
{
    bool aberta = sensor_porta_aberta_bruto();
    bool candidato = aberta;
    int estavel = 0;
    int botao_baixo = 0;
    bool botao_tratado = false;
    bool tocando = false;

    s_porta_aberta = aberta;
    ESP_LOGI(TAG, "Sensor da porta: nivel=%d -> porta %s",
             gpio_get_level(PIN_SENSOR_PORTA), aberta ? "ABERTA" : "FECHADA");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_PORTA_MS));

        // --- Sensor com anti-ruido: so muda se ficar estavel por ~100 ms ---
        bool leitura = sensor_porta_aberta_bruto();
        if (leitura == candidato) {
            if (estavel < SENSOR_AMOSTRAS_ESTAVEIS) {
                estavel++;
            }
        } else {
            candidato = leitura;
            estavel = 1;
        }
        if (estavel >= SENSOR_AMOSTRAS_ESTAVEIS && candidato != aberta) {
            aberta = candidato;
            porta_mudou(aberta);
        }

        // --- Botao de silenciar (ativo em nivel baixo, com anti-ruido) ---
        if (gpio_get_level(PIN_BOTAO_ALARME) == 0) {
            if (botao_baixo < 3) {
                botao_baixo++;
            }
            if (botao_baixo >= 3 && !botao_tratado) {
                botao_tratado = true;
                alarme_silenciar("botao");
            }
        } else {
            botao_baixo = 0;
            botao_tratado = false;
        }

        // --- Buzzer do alarme ---
        if (s_alarme_ativo) {
            TickType_t decorrido = xTaskGetTickCount() - s_alarme_inicio;
            if (decorrido >= pdMS_TO_TICKS(ALARME_DURACAO_MS)) {
                alarme_silenciar("tempo esgotado");
            } else {
                bool ligado = ((decorrido / pdMS_TO_TICKS(ALARME_PERIODO_MS)) % 2) == 0;
                gpio_set_level(PIN_BUZZER, ligado ? 1 : 0);
                tocando = true;
            }
        } else if (tocando) {
            gpio_set_level(PIN_BUZZER, 0);
            tocando = false;
        }
    }
}

/* ---------- MQTT ---------- */
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_conectado = true;
        ESP_LOGI(TAG, "Conectado ao broker");

        // Assina as leituras do RFID para saber quando um cartao autorizado passou
        esp_mqtt_client_subscribe(client, TOPIC_ACESSO_TENTATIVA, 1);

        // Status desta ESP (com retain, para quem conectar depois)
        esp_mqtt_client_publish(client, TOPIC_STATUS_SENSOR, "online", 0, 1, true);

        // Estado atual do sensor da porta e do alarme
        mqtt_pub(TOPIC_SENSOR_PORTA, s_porta_aberta ? "aberta" : "fechada", true);
        mqtt_pub(TOPIC_ALARME,
                 s_alarme_ativo ? "{\"estado\":\"disparado\"}" : "{\"estado\":\"normal\"}",
                 true);
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_conectado = false;
        ESP_LOGW(TAG, "Desconectado do broker (reconexao automatica)");
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Recebido -> topico: %.*s | msg: %.*s",
                 event->topic_len, event->topic,
                 event->data_len, event->data);

        if (event->topic_len == (int)strlen(TOPIC_ACESSO_TENTATIVA) &&
            strncmp(event->topic, TOPIC_ACESSO_TENTATIVA, event->topic_len) == 0) {

            // Copia o payload para um buffer terminado em '\0' antes de procurar nele
            char buf[160];
            int n = event->data_len < (int)sizeof(buf) - 1 ? event->data_len : (int)sizeof(buf) - 1;
            memcpy(buf, event->data, n);
            buf[n] = '\0';

            if (strstr(buf, "\"autorizado\":true") != NULL) {
                cartao_autorizado_recebido();
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "Erro MQTT");
        break;

    default:
        break;
    }
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_URI,
        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASS,
        .credentials.client_id = MQTT_CLIENT_ID,

        // Last Will and Testament: se esta ESP cair (energia ou Wi-Fi), o
        // PROPRIO broker publica "offline" nesse topico, com retain, avisando
        // que a porta ficou sem monitoramento do sensor/alarme.
        .session.last_will.topic = TOPIC_STATUS_SENSOR,
        .session.last_will.msg = "offline",
        .session.last_will.msg_len = 0,
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);
}

/* ---------- main ---------- */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Sensor da porta + botao + buzzer do alarme (task propria)
    porta_init();
    xTaskCreate(tarefa_porta, "porta", 4096, NULL, 5, NULL);

    wifi_init_sta();
    mqtt_app_start();
}