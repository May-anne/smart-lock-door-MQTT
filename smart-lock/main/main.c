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
#include "driver/ledc.h"
#include "rc522.h"
#include "driver/rc522_spi.h"
#include "rc522_picc.h"

#define WIFI_SSID   "REDMI 17 May"
#define WIFI_PASS   "Mayanne12"
#define MQTT_URI    "mqtt://192.168.198.37:1883"   // IP do PC/servidor com o Mosquitto
#define MQTT_USER   NULL                           
#define MQTT_PASS   NULL                           
#define TOPIC_SUB   "esp32/cmd"
#define TOPIC_PUB   "esp32/status"

// Topicos da fechadura (prefixo "porta/principal")
#define TOPIC_ACESSO_TENTATIVA  "porta/principal/acesso/tentativa"
#define TOPIC_ESTADO            "porta/principal/estado"
#define TOPIC_STATUS            "porta/principal/status"

// Pinagem do leitor RC522 (SPI)
#define RC522_SPI_BUS_GPIO_MISO    19
#define RC522_SPI_BUS_GPIO_MOSI    23
#define RC522_SPI_BUS_GPIO_SCLK    18
#define RC522_SPI_SCANNER_GPIO_SDA 5
#define RC522_SCANNER_GPIO_RST     22

// Pinagem do feedback (LEDs / buzzer)
#define PIN_LED_VERDE     GPIO_NUM_26
#define PIN_LED_VERMELHO  GPIO_NUM_25
#define PIN_BUZZER        GPIO_NUM_27

// Micro servo (SG90)
#define PIN_SERVO               GPIO_NUM_13
#define SERVO_CANAL             LEDC_CHANNEL_0
#define SERVO_ANGULO_FECHADO     90     // graus (porta fechada)
#define SERVO_PULSO_MIN_US     1000
#define SERVO_PULSO_MAX_US     2000
#define SERVO_ANGULO_ABERTO     0

#define PERIOD_ABERTA 3000   // tempo (ms) que a fechadura/servo fica aberto após cartao autorizado

static const char *TAGS_AUTORIZADAS[] = {
    "71 ED F6 04",
};
#define NUM_TAGS_AUTORIZADAS (sizeof(TAGS_AUTORIZADAS) / sizeof(TAGS_AUTORIZADAS[0]))
// =========================

static const char *TAG = "MQTT_APP";
static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_conectado = false;

static rc522_driver_handle_t s_rc522_driver;
static rc522_handle_t s_rc522_scanner;

static char s_ultimo_uid[32] = {0};
static TickType_t s_ultimo_tick = 0;

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

/* ---------- Feedback (LED / buzzer) ---------- */
static void feedback_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_LED_VERDE) | (1ULL << PIN_LED_VERMELHO) | (1ULL << PIN_BUZZER),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_LED_VERDE, 0);
    gpio_set_level(PIN_LED_VERMELHO, 0);
    gpio_set_level(PIN_BUZZER, 0);
}

static void feedback_acesso_liberado(void)
{
    gpio_set_level(PIN_LED_VERMELHO, 0);
    gpio_set_level(PIN_LED_VERDE, 1);
    gpio_set_level(PIN_BUZZER, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    gpio_set_level(PIN_BUZZER, 0);
    vTaskDelay(pdMS_TO_TICKS(PERIOD_ABERTA));
    gpio_set_level(PIN_LED_VERDE, 0);
}

static void feedback_acesso_negado(void)
{
    gpio_set_level(PIN_LED_VERDE, 0);
    for (int i = 0; i < 3; i++) {
        gpio_set_level(PIN_LED_VERMELHO, 1);
        gpio_set_level(PIN_BUZZER, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
        gpio_set_level(PIN_LED_VERMELHO, 0);
        gpio_set_level(PIN_BUZZER, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
}

/* ---------- Servo (simboliza a porta abrindo/fechando) ---------- */
static void servo_mover(int graus)
{
    if (graus < 0)   graus = 0;
    if (graus > 180) graus = 180;

    uint32_t pulso_us = SERVO_PULSO_MIN_US +
                        (uint32_t)(SERVO_PULSO_MAX_US - SERVO_PULSO_MIN_US) * graus / 180;
    // PWM de 50 Hz (periodo de 20 ms) com 14 bits de resolucao
    uint32_t duty = (pulso_us * (1U << 14)) / 20000U;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, SERVO_CANAL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, SERVO_CANAL);
}

static void servo_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 50,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t canal = {
        .gpio_num   = PIN_SERVO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = SERVO_CANAL,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&canal);

    servo_mover(SERVO_ANGULO_FECHADO);   // a porta começa fechada
}

/* ---------- Autorizacão local ---------- */
static bool tag_autorizada(const char *uid_str)
{
    for (size_t i = 0; i < NUM_TAGS_AUTORIZADAS; i++) {
        if (strcasecmp(uid_str, TAGS_AUTORIZADAS[i]) == 0) {
            return true;
        }
    }
    return false;
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
        esp_mqtt_client_subscribe(client, TOPIC_SUB, 1);
        esp_mqtt_client_publish(client, TOPIC_PUB, "online", 0, 1, 0);

        // Publica status/estado da fechadura com retain, para quem conectar
        // depois já ver o estado atual sem esperar o próximo evento.
        esp_mqtt_client_publish(client, TOPIC_STATUS, "online", 0, 1, true);
        esp_mqtt_client_publish(client, TOPIC_ESTADO, "travada", 0, 1, true);
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_conectado = false;
        ESP_LOGW(TAG, "Desconectado do broker (reconexao automatica)");
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Recebido -> topico: %.*s | msg: %.*s",
                 event->topic_len, event->topic,
                 event->data_len, event->data);
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
        .session.keepalive = 15,
        .session.last_will.topic = TOPIC_STATUS,
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

/* ---------- RFID (RC522) ---------- */

// Processa uma leitura de cartão: publica a tentativa, checa autorização,
// Dá o feedback (LED verde/vermelho) e atualiza o tópico de estado.
static void processar_cartao(rc522_picc_t *picc)
{
    char uid_str[RC522_PICC_UID_STR_BUFFER_SIZE_MAX];
    esp_err_t err = rc522_picc_uid_to_str(&picc->uid, uid_str, sizeof(uid_str));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel converter o UID para string.");
        return;
    }

    // Evita reprocessar a mesma tag em sequencia
    TickType_t agora = xTaskGetTickCount();
    bool repetido = (strcmp(uid_str, s_ultimo_uid) == 0) &&
                     (agora - s_ultimo_tick) < pdMS_TO_TICKS(2500);
    if (repetido) {
        return;
    }
    strncpy(s_ultimo_uid, uid_str, sizeof(s_ultimo_uid) - 1);
    s_ultimo_tick = agora;

    bool autorizado = tag_autorizada(uid_str);
    ESP_LOGI(TAG, "Cartao lido: %s -> %s", uid_str, autorizado ? "AUTORIZADO" : "NEGADO");

    char payload[128];
    snprintf(payload, sizeof(payload), "{\"uid\":\"%s\",\"autorizado\":%s}",
             uid_str, autorizado ? "true" : "false");

    if (s_mqtt_conectado) {
        esp_mqtt_client_publish(s_mqtt_client, TOPIC_ACESSO_TENTATIVA, payload, 0, 1, false);
    } else {
        ESP_LOGW(TAG, "MQTT desconectado - tentativa nao publicada (ficara so no log local).");
    }

    if (autorizado) {
        if (s_mqtt_conectado) {
            esp_mqtt_client_publish(s_mqtt_client, TOPIC_ESTADO, "aberta", 0, 1, true);
        }
        servo_mover(SERVO_ANGULO_ABERTO);
        feedback_acesso_liberado();   // LED verde + espera PERIOD_ABERTA
        servo_mover(SERVO_ANGULO_FECHADO);
        if (s_mqtt_conectado) {
            esp_mqtt_client_publish(s_mqtt_client, TOPIC_ESTADO, "travada", 0, 1, true);
        }
    } else {
        feedback_acesso_negado();
    }
}

// Dispara este evento sempre que o estado de um cartão muda
static void on_picc_state_changed(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    rc522_picc_state_changed_event_t *event = (rc522_picc_state_changed_event_t *)data;
    rc522_picc_t *picc = event->picc;

    if (picc->state == RC522_PICC_STATE_ACTIVE) {
        processar_cartao(picc);
    } else if (picc->state == RC522_PICC_STATE_IDLE && event->old_state >= RC522_PICC_STATE_ACTIVE) {
        ESP_LOGI(TAG, "Cartao removido do leitor.");
    }
}

static esp_err_t rc522_setup(void)
{
    rc522_spi_config_t driver_config = {
        .host_id = SPI3_HOST,
        .bus_config = &(spi_bus_config_t){
            .miso_io_num = RC522_SPI_BUS_GPIO_MISO,
            .mosi_io_num = RC522_SPI_BUS_GPIO_MOSI,
            .sclk_io_num = RC522_SPI_BUS_GPIO_SCLK,
        },
        .dev_config = {
            .spics_io_num = RC522_SPI_SCANNER_GPIO_SDA,
        },
        .rst_io_num = RC522_SCANNER_GPIO_RST,
    };

    esp_err_t err = rc522_spi_create(&driver_config, &s_rc522_driver);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao criar driver SPI do RC522: %s", esp_err_to_name(err));
        return err;
    }

    err = rc522_driver_install(s_rc522_driver);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao instalar driver do RC522: %s", esp_err_to_name(err));
        return err;
    }

    rc522_config_t scanner_config = {
        .driver = s_rc522_driver,
    };

    err = rc522_create(&scanner_config, &s_rc522_scanner);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao criar scanner RC522: %s", esp_err_to_name(err));
        return err;
    }

    rc522_register_events(s_rc522_scanner, RC522_EVENT_PICC_STATE_CHANGED, on_picc_state_changed, NULL);

    err = rc522_start(s_rc522_scanner);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar scanner RC522: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
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

    feedback_init();
    servo_init();

    if (rc522_setup() != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar o RC522. Verifique a conexão SPI antes de continuar.");
    }

    wifi_init_sta();
    mqtt_app_start();
}