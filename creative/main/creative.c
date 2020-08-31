#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include <pthread.h>
#include "get_dht11_data.h"
#include "general.h"
#include "wrappers.h"
#include "sh1106.h"
// #include "font6x8.h"

#define GPIO_LED1 27
#define GPIO_LED2 26

#define GPIO_BUTTON1 39
#define GPIO_BUTTON2 18
#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<GPIO_LED1) | (1ULL<<GPIO_LED2))
#define GPIO_INPUT_PIN_SEL  ((1ULL<<GPIO_BUTTON1) | (1ULL<<GPIO_BUTTON2))
#define ESP_INTR_FLAG_DEFAULT 0

#define OLED_ENABLE 32

static xQueueHandle gpio_evt_queue = NULL;
pthread_mutex_t mutex;

static void IRAM_ATTR gpio_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t) arg;

    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}



static int screen_num = 0;

static void switch_handler(void* arg) {
    uint32_t io_num;

    for(;;) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            if (io_num == GPIO_BUTTON1) {
                if (screen_num == 0)
                    screen_num += 1;
            }
            else if (io_num == GPIO_BUTTON2) {
                if (screen_num == 1)
                    screen_num -= 1;
            }
        }
    }
}

static int temperature = 0;
static int humidity    = 0;

static void dht11_data_checker() {
    int res;

    while(1) {
        if (screen_num == 0) {
            res = get_dht11_data(2, 4, 1);
            if (res != -1) humidity    = res;
        }
        else if (screen_num == 1) {
            res = get_dht11_data(2, 4, 0);
            if (res != -1) temperature = res;
        }
        vTaskDelay(100);
    }
}



// Initialize temperature and humidity value;
static void dht11_init() {
    int counter = 0;
    int res;

    while(counter < 5) {
        res = get_dht11_data(2, 4, 0);
        if (res == -1) {
            counter++;
            continue;
        }
        temperature = res;

        res = get_dht11_data(2, 4, 1);
        if (res == -1) {
            counter++;
            continue;
        }
        humidity = res;
        return;
    }
    printf("Error while dht11_init\n");
}


static void checker(void *s) {
    sh1106_t display;
    sh1106_init(&display);
    sh1106_clear(&display);

    sh1106_t *display1 = &display;

    while(1) {
        pthread_mutex_lock(&mutex);
        if (screen_num == 0) {
            sh1106_clear(&display);

            char res[50];
            bzero(res, 50);
            sprintf(res, "    humidity: %d %%", humidity);

            print_str_in_line(&display1, res, 3);
        }
        else if (screen_num == 1) {
            sh1106_clear(&display);

            char res[50];
            bzero(res, 50);
            sprintf(res, "   temperature: %d C", temperature);

            print_str_in_line(&display1, res, 3);
        }
        sh1106_update(&display);
        pthread_mutex_unlock(&mutex);
        vTaskDelay(100);
    }
}


void app_main(void) {

    // Oled enable
    gpio_set_direction_wrapper(OLED_ENABLE, GPIO_MODE_OUTPUT);
    gpio_set_level_wrapper(OLED_ENABLE, 1);

    init_i2c_driver();

    sh1106_t display;
    sh1106_init(&display);
    sh1106_clear(&display);

    sh1106_t *display1 = &display;
    print_str_in_line(&display1, "        Wait...", 3);
    sh1106_update(&display);


    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    gpio_config(&io_conf);

    io_conf.intr_type = GPIO_PIN_INTR_POSEDGE;
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    io_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&io_conf);

    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));

    dht11_init();
    xTaskCreate(switch_handler, "switch_handler", 4048, NULL, 10, NULL);
    xTaskCreate(checker, "checker", 4048, NULL, 10, NULL);
    xTaskCreate(dht11_data_checker, "dht11_data_checker", 4048, NULL, 10, NULL);

    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(GPIO_BUTTON1, gpio_isr_handler, (void*) GPIO_BUTTON1);
    gpio_isr_handler_add(GPIO_BUTTON2, gpio_isr_handler, (void*) GPIO_BUTTON2);
}