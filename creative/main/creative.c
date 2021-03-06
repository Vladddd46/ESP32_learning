#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include <pthread.h>
#include "get_dht11_data.h"
#include "general.h"
#include "wrappers.h"
#include "sh1106.h"
#include "make_beep.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_system.h"
#include "driver/spi_master.h"
#include "driver/dac.h"
#include "accelerometer.h"

#define GPIO_NUM_5    5
#define GPIO_NUM_25   25
#define GPIO_EN_ACCEL 23

#define GPIO_BUTTON1 39
#define GPIO_BUTTON2 18
#define GPIO_INPUT_PIN_SEL   ((1ULL<<GPIO_BUTTON1) | (1ULL<<GPIO_BUTTON2))
#define ESP_INTR_FLAG_DEFAULT 0

#define OLED_ENABLE 32

static xQueueHandle gpio_evt_queue = NULL;
static pthread_mutex_t mutex;
static int screen_num  = 0;
static int must_beep   = 0;
static int temperature = 0;
static int humidity    = 0;
static int reverse     = 0;
static int refresh     = 0;



// Beep task, which causes when user press on switch button.
static void beep() {
    must_beep = 1;
    make_beep();
    must_beep = 0;
    vTaskDelete(NULL);
}



/*
 * ISR, which handles button press.
 * Sets global variable "screen_num" to
 * the corresponding screen.
 */
static void IRAM_ATTR gpio_isr_handler(void *arg) {
    uint32_t gpio_num = (uint32_t)arg;

    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void switch_button_handler(void* arg) {
    uint32_t io_num;

    for(;;) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            gpio_intr_disable(GPIO_BUTTON1);
            gpio_intr_disable(GPIO_BUTTON2);
            if (io_num == GPIO_BUTTON1) {
                if (screen_num == 0)
                    screen_num += 1;
                if (must_beep == 0)
                    xTaskCreate(beep, "beep", 2048u, NULL, 5, 0);
            }
            else if (io_num == GPIO_BUTTON2) {
                if (screen_num == 1)
                    screen_num -= 1;
                if (must_beep == 0) 
                    xTaskCreate(beep, "beep", 2048u, NULL, 5, 0);
            }
            gpio_intr_enable(GPIO_BUTTON1);
            gpio_intr_enable(GPIO_BUTTON2);
        }
    }
}



/*
 * Gets data from dht11 depending on 
 * the current screen state.
 * Places result in 
 * global vars. temperature/humidity. 
 */
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



/*
 * Initialize temperature and humidity values.
 */
static void dht11_data_init() {
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



// reverses display 
static void screen_reverser(sh1106_t *display) {
    if (reverse == 1 && refresh) {
        sh1106_reverse(display);
        refresh = 0;
    }
    else if (reverse == 0 && refresh) {
        sh1106_dereverse(display);
        refresh = 0;
    }
}



// Forms msg to be drawn depending on screen_num.
static char *msg_former() {
    char res[50];
    char *msg;
    bzero(res, 50);

    if (screen_num == 0)
        sprintf(res, "    humidity: %d %%", humidity);
    else if (screen_num == 1)
        sprintf(res, "   temperature: %d C", temperature);
    msg = string_copy(res);
    return msg;
}



/*
 * Draws screen page depending on screen_num.
 */
static void drawer(void) {
    vTaskDelay(100);
    sh1106_t display;
    sh1106_init(&display);
    sh1106_clear(&display);
    sh1106_t *display1 = &display;

    char *msg;
    while(1) {
        pthread_mutex_lock(&mutex);
        sh1106_clear(&display);
        screen_reverser(&display);
        msg = msg_former();
        print_str_in_line(&display1, msg, 3);
        sh1106_update(&display);
        pthread_mutex_unlock(&mutex);
        free(msg);
        vTaskDelay(10);
    }
}



/*
 * Darws waiting page, while dht11 needs
 * to process data.
 */
static void wait_page_draw() {
    sh1106_t display;
    sh1106_init(&display);
    sh1106_clear(&display);

    sh1106_t *display1 = &display;
    print_str_in_line(&display1, "        Wait...", 3);
    sh1106_update(&display);
}



// defines acceleration.
static void accelerator(void* pvParameters) {
    int16_t accs[3];
    int y;

    spi_device_handle_t spi = (spi_device_handle_t)pvParameters;
    while (1) {
        adxl345_read_acceleration(spi, accs);
        y = (int)accs[1];

        if (y >= 200) {
            if (reverse == 0)
                refresh = 1;
            reverse = 1;
        }
        else {
            if (reverse == 1)
                refresh = 1;
            reverse = 0;
        }
        vTaskDelay(100);
    }
}



void app_main(void) {
    gpio_set(OLED_ENABLE,   GPIO_MODE_OUTPUT, 1);  // Oled enable.
    gpio_set(GPIO_NUM_5,    GPIO_MODE_INPUT,  1);  // Amp enable.
    gpio_set(GPIO_EN_ACCEL, GPIO_MODE_OUTPUT, 1);  // accelerometer enable.
    dac_output_enable_wrapper(DAC_CHANNEL_1);

    // Accel. configuration
    spi_device_handle_t spi;
    spi_bus_config_t bus_config                 = bus_config_init();
    spi_device_interface_config_t device_config = device_config_init();
    ESP_ERROR_CHECK(spi_bus_initialize(VSPI_HOST, &bus_config, 0)); // initializes the SPI bus
    ESP_ERROR_CHECK(spi_bus_add_device(VSPI_HOST, &device_config, &spi)); // attaches the ADXL to the SPI bus
    adxl345_start(spi);

    init_i2c_driver();
    wait_page_draw();
    dht11_data_init();

    // Interrupts configuration.
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    // io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    gpio_config(&io_conf);
    io_conf.intr_type = GPIO_PIN_INTR_POSEDGE;
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    io_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&io_conf);
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));

    xTaskCreate(switch_button_handler, "switch_button_handler", 4048, NULL, 10, NULL);
    xTaskCreate(drawer, "drawer", 4048, NULL, 10, NULL);
    xTaskCreate(dht11_data_checker, "dht11_data_checker", 4048, NULL, 10, NULL);
    xTaskCreate(accelerator, "acceleration", 2048u, (void*)spi, 5, 0);

    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT));
    ESP_ERROR_CHECK(gpio_isr_handler_add(GPIO_BUTTON1, gpio_isr_handler, (void*) GPIO_BUTTON1));
    ESP_ERROR_CHECK(gpio_isr_handler_add(GPIO_BUTTON2, gpio_isr_handler, (void*) GPIO_BUTTON2));
}
