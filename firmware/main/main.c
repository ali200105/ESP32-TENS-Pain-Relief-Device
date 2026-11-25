#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "i2c_lcd.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "driver/mcpwm_prelude.h"

static const char *TAG = "mcpwm_ui_merged"; 

// ==========================================
// ---------- إعدادات Encoder والواجهة --------
// ==========================================
#define ENC_A_GPIO 16
#define ENC_B_GPIO 17
#define ENC_SW_GPIO 5
#define BUZZER_GPIO 25 

static const int _pauseLength = 25000; 
static const int _fastIncrement = 10;   
static uint64_t _lastIncReadTime = 0;
static uint64_t _lastDecReadTime = 0;

typedef enum {
    MENU_FREQUENCY = 0,
    MENU_PULSE_WIDTH,
    MENU_TIMER,
    MENU_COUNT
} menu_selection_t;

static menu_selection_t current_selection = MENU_FREQUENCY;
static bool is_operating = true; 
static int blink_counter = 0;    

static int val_freq = 2;   
static int val_pw = 40;    
static int total_seconds = 15 * 60; 

static volatile int rotary_diff = 0; 
static portMUX_TYPE spinlock = portMUX_INITIALIZER_UNLOCKED;


// ==========================================
// ---------- مقابض العتاد العامة MCPWM --------
// ==========================================
static mcpwm_timer_handle_t timer = NULL;
static mcpwm_cmpr_handle_t cmpa[2];
static mcpwm_cmpr_handle_t cmpb[2];
static mcpwm_cmpr_handle_t cmpc;
static const uint32_t DEAD_TIME_TICKS = 3;

// مقابض المولدات (سنحتاجها لـ force level)
static mcpwm_gen_handle_t gena = NULL;
static mcpwm_gen_handle_t genb = NULL;
static mcpwm_gen_handle_t genc = NULL;


// ==========================================
// ---------------- الدوال ------------------
// ==========================================
static void timer_end_alert(void) {
    // تنبيه متقطع 5 نبضات قصيرة
    for (int i = 0; i < 5; i++) {
        gpio_set_level(BUZZER_GPIO, 1); // تشغيل الصفارة
        vTaskDelay(pdMS_TO_TICKS(150)); // انتظار 150 مللي ثانية
        gpio_set_level(BUZZER_GPIO, 0); // إطفاء الصفارة
        vTaskDelay(pdMS_TO_TICKS(100)); // انتظار 100 مللي ثانية
    }
}
void ui_draw(void) {
    lcd_clear_fast();

    lcd_put_cursor(0, 0);
    lcd_send_string("**Mode: ");
    if (is_operating) {
        lcd_send_string("Operating");
        if (blink_counter % 2 == 0) lcd_send_string("..."); 
        else lcd_send_string("   ");
    } else {
        lcd_send_string("OFF        ");
    }

    lcd_put_cursor(1, 0);
    if (current_selection == MENU_FREQUENCY) lcd_send_string("> ");
    else lcd_send_string("  ");
    lcd_send_string("Freq: ");
    lcd_send_number(val_freq);
    lcd_send_string(" Hz ");

    lcd_put_cursor(2, 0);
    if (current_selection == MENU_PULSE_WIDTH) lcd_send_string("> ");
    else lcd_send_string("  ");
    lcd_send_string("P.W : ");
    lcd_send_number(val_pw);
    lcd_send_string(" us ");

    lcd_put_cursor(3, 0);
    if (current_selection == MENU_TIMER) lcd_send_string("> ");
    else lcd_send_string("  ");
    lcd_send_string("Time: ");
    int mins = total_seconds / 60;
    int secs = total_seconds % 60;
    if (mins < 10) lcd_send_string("0");
    lcd_send_number(mins);
    lcd_send_string(":");
    if (secs < 10) lcd_send_string("0");
    lcd_send_number(secs);
}

static void IRAM_ATTR encoder_isr_handler(void *arg) {
    static uint8_t old_AB = 3;
    static int8_t encval = 0;
    
    int a = gpio_get_level(ENC_A_GPIO);
    int b = gpio_get_level(ENC_B_GPIO);
    old_AB <<= 2;
    if (a) old_AB |= 0x02;
    if (b) old_AB |= 0x01;
    
    static const int8_t enc_states[] = {0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0};
    encval += enc_states[(old_AB & 0x0f)];

    uint64_t now = esp_timer_get_time();
    int increment = 1;

    if (encval > 3) { 
        if ((now - _lastIncReadTime) < _pauseLength) increment = _fastIncrement; 
        _lastIncReadTime = now;
        
        portENTER_CRITICAL_ISR(&spinlock);
        rotary_diff += increment;
        portEXIT_CRITICAL_ISR(&spinlock);
        encval = 0;
    } 
    else if (encval < -3) { 
        if ((now - _lastDecReadTime) < _pauseLength) increment = _fastIncrement;
        _lastDecReadTime = now;

        portENTER_CRITICAL_ISR(&spinlock);
        rotary_diff -= increment;
        portEXIT_CRITICAL_ISR(&spinlock);
        encval = 0;
    }
}

// --- دالة تشغيل/إطفاء جميع المخارج ---
static void set_all_outputs(bool enable) {
    if (enable) {
        // إزالة force level عن جميع المولدات (العودة للتحكم الطبيعي)
        ESP_ERROR_CHECK(mcpwm_generator_set_force_level(gena, -1, true));
        ESP_ERROR_CHECK(mcpwm_generator_set_force_level(genb, -1, true));
        ESP_ERROR_CHECK(mcpwm_generator_set_force_level(genc, -1, true));
        ESP_ERROR_CHECK(mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP));
        ESP_LOGI(TAG, "All outputs ENABLED");
    } else {
        // إيقاف المؤقت أولاً
        ESP_ERROR_CHECK(mcpwm_timer_start_stop(timer, MCPWM_TIMER_STOP_FULL));
        // ثم فرض LOW على جميع المخارج
        ESP_ERROR_CHECK(mcpwm_generator_set_force_level(gena, 0, true));
        ESP_ERROR_CHECK(mcpwm_generator_set_force_level(genb, 0, true));
        ESP_ERROR_CHECK(mcpwm_generator_set_force_level(genc, 0, true));
        ESP_LOGI(TAG, "All outputs DISABLED (forced LOW)");
    }
}

static void update_pwm_laws(uint32_t period, uint32_t ton)
{
    uint32_t A0 = DEAD_TIME_TICKS;
    uint32_t A1 = A0 + ton;
    uint32_t B0 = A1 + DEAD_TIME_TICKS;
    uint32_t B1 = B0 + ton;
    uint32_t C  = B1 + DEAD_TIME_TICKS;

    mcpwm_timer_set_period(timer, period);
    mcpwm_comparator_set_compare_value(cmpa[0], A0);
    mcpwm_comparator_set_compare_value(cmpa[1], A1);
    mcpwm_comparator_set_compare_value(cmpb[0], B0);
    mcpwm_comparator_set_compare_value(cmpb[1], B1);
    mcpwm_comparator_set_compare_value(cmpc, C);
}


// ==========================================
// -------------- الدالة الرئيسية -------------
// ==========================================
void app_main(void) {
    lcd_init();
    
    // تهيئة Encoder
    gpio_config_t btn_conf = {
        .intr_type = GPIO_INTR_ANYEDGE, 
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << ENC_SW_GPIO),
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn_conf);

    gpio_config_t enc_conf = {
        .intr_type = GPIO_INTR_ANYEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << ENC_A_GPIO) | (1ULL << ENC_B_GPIO),
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&enc_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(ENC_A_GPIO, encoder_isr_handler, NULL);
    gpio_isr_handler_add(ENC_B_GPIO, encoder_isr_handler, NULL);

    _lastIncReadTime = esp_timer_get_time();
    _lastDecReadTime = esp_timer_get_time();

    ui_draw();

    // تهيئة MCPWM
    ESP_LOGI(TAG, "Create timer and operator");
    mcpwm_timer_config_t timer_config = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = 100000,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = 50000,
        .flags.update_period_on_empty = true,
    };
    ESP_ERROR_CHECK(mcpwm_new_timer(&timer_config, &timer));

    ESP_LOGI(TAG, "Create operators");
    mcpwm_oper_handle_t operators[3];
    mcpwm_operator_config_t operator_config = {
        .group_id = 0,
    };
    for(int i = 0; i < 3; i++){
        ESP_ERROR_CHECK(mcpwm_new_operator(&operator_config, &operators[i]));
    }

    ESP_LOGI(TAG, "Connect operators to the same timer");
    for(int i = 0; i < 3; i++){
        ESP_ERROR_CHECK(mcpwm_operator_connect_timer(operators[i], timer));
    }

    mcpwm_comparator_config_t compare_config = {
        .flags.update_cmp_on_tez = true,
    };
    ESP_ERROR_CHECK(mcpwm_new_comparator(operators[0], &compare_config, &cmpa[0]));
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(cmpa[0], 3000));
    
    ESP_ERROR_CHECK(mcpwm_new_comparator(operators[0], &compare_config, &cmpa[1]));
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(cmpa[1], 13000));
    
    ESP_ERROR_CHECK(mcpwm_new_comparator(operators[1], &compare_config, &cmpb[0]));
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(cmpb[0], 16000));
   
    ESP_ERROR_CHECK(mcpwm_new_comparator(operators[1], &compare_config, &cmpb[1]));
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(cmpb[1], 26000));
    
    ESP_ERROR_CHECK(mcpwm_new_comparator(operators[2], &compare_config, &cmpc));
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(cmpc, 29000));

    // Generators (نحفظ المقابض في المتغيرات العامة)
    mcpwm_generator_config_t gen_config_op0 = {};
    gen_config_op0.gen_gpio_num = 2;
    ESP_ERROR_CHECK(mcpwm_new_generator(operators[0], &gen_config_op0, &gena));

    mcpwm_generator_config_t gen_config_op1 = {};
    gen_config_op1.gen_gpio_num = 4;
    ESP_ERROR_CHECK(mcpwm_new_generator(operators[1], &gen_config_op1, &genb));

    mcpwm_generator_config_t gen_config_op2 = {};
    gen_config_op2.gen_gpio_num = 15;
    ESP_ERROR_CHECK(mcpwm_new_generator(operators[2], &gen_config_op2, &genc));

    // Generator Actions
    ESP_LOGI(TAG, "Set generator actions");
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(gena,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_LOW)));
    ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_compare_event(gena,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmpa[0], MCPWM_GEN_ACTION_HIGH),
        MCPWM_GEN_COMPARE_EVENT_ACTION_END()));
    ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_compare_event(gena,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmpa[1], MCPWM_GEN_ACTION_LOW),
        MCPWM_GEN_COMPARE_EVENT_ACTION_END()));

    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(genb,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_LOW)));
    ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_compare_event(genb,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmpb[0], MCPWM_GEN_ACTION_HIGH),
        MCPWM_GEN_COMPARE_EVENT_ACTION_END()));
    ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_compare_event(genb,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmpb[1], MCPWM_GEN_ACTION_LOW),
        MCPWM_GEN_COMPARE_EVENT_ACTION_END()));

    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(genc,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_LOW)));
    ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_compare_event(genc,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmpc, MCPWM_GEN_ACTION_HIGH),
        MCPWM_GEN_COMPARE_EVENT_ACTION_END()));

    ESP_LOGI(TAG, "Enable and start timer");
    ESP_ERROR_CHECK(mcpwm_timer_enable(timer));
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP));
    // تهيئة منفذ الصفارة كمخرج
    gpio_set_direction(BUZZER_GPIO, GPIO_MODE_OUTPUT);
    // نبدأ بإطفائها
    gpio_set_level(BUZZER_GPIO, 0);
    // متغيرات الواجهة
    bool last_btn_state = true;
    uint32_t press_start_time = 0;
    uint32_t last_blink_time = 0;
    uint32_t last_timer_tick = 0;
    bool long_press_executed = false;

    // الحلقة الرئيسية
    while (1) {
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // تحديث الوميض
        if (current_time - last_blink_time > 500) {
            blink_counter++;
            ui_draw(); 
            last_blink_time = current_time;
        }

        // المؤقت الحقيقي
        if (is_operating && total_seconds > 0) {
            if (current_time - last_timer_tick >= 1000) {
                total_seconds--;
                if (total_seconds == 0) {
                    timer_end_alert(); 
                    is_operating = false;
                    set_all_outputs(false);  // إطفاء جميع المخارج
                    ESP_LOGI(TAG, "Timer ended - outputs OFF");
                }
                ui_draw();
                last_timer_tick = current_time;
            }
        } else if (!is_operating) {
            last_timer_tick = current_time;
        }

        // معالجة الزر
        bool current_btn_state = gpio_get_level(ENC_SW_GPIO);
        if (last_btn_state == true && current_btn_state == false) {
            press_start_time = current_time;
            long_press_executed = false;
        } 
        else if (last_btn_state == false && current_btn_state == false) {
            if (!long_press_executed && (current_time - press_start_time >= 2000)) {
                is_operating = !is_operating;
                long_press_executed = true;
                set_all_outputs(is_operating);  // تشغيل أو إطفاء جميع المخارج
                ui_draw();
            }
        }
        else if (last_btn_state == false && current_btn_state == true) {
            if (!long_press_executed) {
                current_selection++;
                if (current_selection >= MENU_COUNT) current_selection = MENU_FREQUENCY;
                ui_draw();
            }
        }
        last_btn_state = current_btn_state;

        // معالجة الروتاري
        int diff = 0;
        portENTER_CRITICAL(&spinlock);
        diff = rotary_diff;
        rotary_diff = 0; 
        portEXIT_CRITICAL(&spinlock);

        if (diff != 0) {
            switch (current_selection) {
                case MENU_FREQUENCY:
                    val_freq += diff;
                    if (val_freq < 2) val_freq = 2;
                    if (val_freq > 200) val_freq = 200;
                    break;
                case MENU_PULSE_WIDTH:
                    val_pw += diff*10; 
                    if (val_pw < 40) val_pw = 40;
                    if (val_pw > 500) val_pw = 500;
                    break;
                case MENU_TIMER:
                    total_seconds += (diff * 60);
                    if (total_seconds < 0) total_seconds = 0;
                    if (total_seconds > 3600) total_seconds = 3600;
                    break;
                default: break;
            }
            ui_draw();
        }

        // تحديث PWM فقط إذا كان شغّال
        if (is_operating) {
            uint32_t current_period = 100000 / val_freq;
            uint32_t current_ton = val_pw / 10;
            update_pwm_laws(current_period, current_ton);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}