#include "data_transmitter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "wifi.h"
#include <stdio.h>
#include <string.h>
#include "config.h"
#include <stdint.h>
#include <stddef.h>
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "mqtt_client.h"

//MQTT
static const char *TAG = "MQTT_EXAMPLE";

// Set your local broker URI
#define BROKER_URI projectConfig.brockerUri

#define MOSQUITO_USER_NAME              projectConfig.mosquittoUserName
#define MOSQUITO_USER_PASSWORD          projectConfig.mosquittoUserPassword

#define MAX_SENSORS 30
#define TX_BUFF_SIZE (1024*16) //16k just in case

static esp_mqtt_client_handle_t global_client;

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
//is not used by now
/*static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}*/

static void mqtt_app_send_metric(char * metric)
{
    int msg_id;
    ESP_LOGI(TAG, "Publish");
    msg_id = esp_mqtt_client_publish(global_client, "/metrics/123", metric, 0, 1, 0);
    ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
}

static void mqtt_app_start()
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = BROKER_URI,
        .username = MOSQUITO_USER_NAME,
        .password = MOSQUITO_USER_PASSWORD,
    };
    #if CONFIG_BROKER_URL_FROM_STDIN
        char line[128];

        if (strcmp(mqtt_cfg.uri, "FROM_STDIN") == 0) {
            int count = 0;
            printf("Please enter url of mqtt broker\n");
            while (count < 128) {
                int c = fgetc(stdin);
                if (c == '\n') {
                    line[count] = '\0';
                    break;
                } else if (c > 0 && c < 127) {
                    line[count] = c;
                    ++count;
                }
                vTaskDelay(10 / portTICK_PERIOD_MS);
            }
            mqtt_cfg.uri = line;
            printf("Broker url: %s\n", line);
        } else {
            ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
            abort();
        }
    #endif /* CONFIG_BROKER_URL_FROM_STDIN */

    global_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(global_client);
}

struct
{
	char name[30];
	void * pSensorTda;
	void (*sensorRead)(void *,char *, int);

}registeredSensors[MAX_SENSORS];

void dataTransmitterInit(void)
{
	for(int i=0;i<MAX_SENSORS;i++) registeredSensors[i].sensorRead = NULL; //to crete free slots
}

int dataTransmitterRegisterSensor(char * name, void * pSensorTda, void (*sensorRead)(void *,char *, int))
{
	for(int i=0;i<MAX_SENSORS;i++)
	{
		if(registeredSensors[i].sensorRead == NULL)
		{
			strcpy(registeredSensors[i].name, name);
			registeredSensors[i].pSensorTda = pSensorTda;
			registeredSensors[i].sensorRead = sensorRead;
			return 1;
		}
	}
	return 0;
}

void appendToTxBuffer(char * txBuffer, char *name, char *value)
{
	int len = strlen(txBuffer);
	sprintf(&txBuffer[len],"\"%s\":%s,",name,value);
}

void dataTransmitterPrintTask(void * parameter)
{
	char * txBuffer = malloc(TX_BUFF_SIZE);
	while(1)
	{
		vTaskDelay(15000 / portTICK_PERIOD_MS);
        wifi_wait_connected();
        strcpy(txBuffer,"{");
		for(int i=0;i<MAX_SENSORS;i++)
		{
			if(registeredSensors[i].sensorRead != NULL)
			{
				char value[100];
				registeredSensors[i].sensorRead( registeredSensors[i].pSensorTda,value,100);
				appendToTxBuffer(txBuffer, registeredSensors[i].name, value);
			}
		}
		txBuffer[strlen(txBuffer) -1] = '}'; //cambiamos la , por }
		printf("---Temperature is:");
		printf(txBuffer);
        mqtt_app_send_metric(txBuffer);
	}
}

void dataTransmitterPrintStart(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("MQTT_EXAMPLE", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_BASE", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    //ESP_ERROR_CHECK(esp_event_loop_create_default());

    mqtt_app_start();
	xTaskCreate(dataTransmitterPrintTask,"dataTxTask", 10000, NULL, 1, NULL);
}