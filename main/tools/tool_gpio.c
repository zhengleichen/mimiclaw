#include "tools/tool_gpio.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "soc/soc_caps.h"

static const char *TAG = "tool_gpio";

static bool gpio_pin_valid_int(int pin)
{
#ifdef SOC_GPIO_PIN_COUNT
    return pin >= 0 && pin < SOC_GPIO_PIN_COUNT;
#else
    return pin >= 0 && pin < 49;
#endif
}

static esp_err_t write_json_ok(cJSON *obj, char *output, size_t output_size)
{
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"no_mem\"}");
        return ESP_ERR_NO_MEM;
    }
    snprintf(output, output_size, "%s", s);
    free(s);
    return ESP_OK;
}

static esp_err_t write_json_error(const char *msg, char *output, size_t output_size)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        snprintf(output, output_size, "Error: %s", msg ? msg : "unknown");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(obj, "ok", false);
    cJSON_AddStringToObject(obj, "error", msg ? msg : "unknown");
    esp_err_t err = write_json_ok(obj, output, output_size);
    cJSON_Delete(obj);
    return err == ESP_OK ? ESP_ERR_INVALID_ARG : err;
}

static const char *get_str(cJSON *root, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (!v || !cJSON_IsString(v)) return NULL;
    return v->valuestring;
}

static bool get_int(cJSON *root, const char *key, int *out)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (!v || !cJSON_IsNumber(v)) return false;
    *out = v->valueint;
    return true;
}

esp_err_t tool_gpio_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        return write_json_error("invalid_json", output, output_size);
    }

    const char *action = get_str(root, "action");
    int pin = -1;
    if (!action || !get_int(root, "pin", &pin) || !gpio_pin_valid_int(pin)) {
        cJSON_Delete(root);
        return write_json_error("invalid_action_or_pin", output, output_size);
    }

    gpio_num_t gpio = (gpio_num_t)pin;
    esp_err_t err = ESP_OK;

    if (strcmp(action, "config_output") == 0) {
        int initial_level = 0;
        cJSON *lvl = cJSON_GetObjectItem(root, "initial_level");
        if (lvl && cJSON_IsNumber(lvl)) initial_level = lvl->valueint ? 1 : 0;

        err = gpio_reset_pin(gpio);
        if (err == ESP_OK) err = gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
        if (err == ESP_OK) err = gpio_set_level(gpio, initial_level);

        if (err == ESP_OK) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddBoolToObject(obj, "ok", true);
            cJSON_AddStringToObject(obj, "action", action);
            cJSON_AddNumberToObject(obj, "pin", pin);
            cJSON_AddNumberToObject(obj, "level", initial_level);
            write_json_ok(obj, output, output_size);
            cJSON_Delete(obj);
        }
    } else if (strcmp(action, "config_input") == 0) {
        const char *pull = get_str(root, "pull");

        err = gpio_reset_pin(gpio);
        if (err == ESP_OK) err = gpio_set_direction(gpio, GPIO_MODE_INPUT);
        if (err == ESP_OK) err = gpio_set_pull_mode(gpio, GPIO_FLOATING);

        if (err == ESP_OK && pull) {
            if (strcmp(pull, "up") == 0) {
                err = gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
            } else if (strcmp(pull, "down") == 0) {
                err = gpio_set_pull_mode(gpio, GPIO_PULLDOWN_ONLY);
            } else if (strcmp(pull, "none") == 0) {
                err = gpio_set_pull_mode(gpio, GPIO_FLOATING);
            } else {
                err = ESP_ERR_INVALID_ARG;
            }
        }

        if (err == ESP_OK) {
            int level = gpio_get_level(gpio);
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddBoolToObject(obj, "ok", true);
            cJSON_AddStringToObject(obj, "action", action);
            cJSON_AddNumberToObject(obj, "pin", pin);
            if (pull) cJSON_AddStringToObject(obj, "pull", pull);
            cJSON_AddNumberToObject(obj, "level", level);
            write_json_ok(obj, output, output_size);
            cJSON_Delete(obj);
        }
    } else if (strcmp(action, "set_level") == 0) {
        int level = 0;
        if (!get_int(root, "level", &level)) {
            cJSON_Delete(root);
            return write_json_error("missing_level", output, output_size);
        }
        level = level ? 1 : 0;
        err = gpio_set_level(gpio, level);
        if (err == ESP_OK) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddBoolToObject(obj, "ok", true);
            cJSON_AddStringToObject(obj, "action", action);
            cJSON_AddNumberToObject(obj, "pin", pin);
            cJSON_AddNumberToObject(obj, "level", level);
            write_json_ok(obj, output, output_size);
            cJSON_Delete(obj);
        }
    } else if (strcmp(action, "get_level") == 0) {
        int level = gpio_get_level(gpio);
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(obj, "ok", true);
        cJSON_AddStringToObject(obj, "action", action);
        cJSON_AddNumberToObject(obj, "pin", pin);
        cJSON_AddNumberToObject(obj, "level", level);
        write_json_ok(obj, output, output_size);
        cJSON_Delete(obj);
        err = ESP_OK;
    } else if (strcmp(action, "toggle") == 0) {
        int level = gpio_get_level(gpio);
        int new_level = level ? 0 : 1;
        err = gpio_set_level(gpio, new_level);
        if (err == ESP_OK) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddBoolToObject(obj, "ok", true);
            cJSON_AddStringToObject(obj, "action", action);
            cJSON_AddNumberToObject(obj, "pin", pin);
            cJSON_AddNumberToObject(obj, "level", new_level);
            write_json_ok(obj, output, output_size);
            cJSON_Delete(obj);
        }
    } else if (strcmp(action, "pulse") == 0) {
        int level = 0;
        int duration_ms = 0;
        if (!get_int(root, "level", &level) || !get_int(root, "duration_ms", &duration_ms)) {
            cJSON_Delete(root);
            return write_json_error("missing_level_or_duration_ms", output, output_size);
        }
        if (duration_ms < 0 || duration_ms > 60000) {
            cJSON_Delete(root);
            return write_json_error("invalid_duration_ms", output, output_size);
        }

        int prev = gpio_get_level(gpio);
        level = level ? 1 : 0;

        err = gpio_set_level(gpio, level);
        if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(duration_ms));
        if (err == ESP_OK) err = gpio_set_level(gpio, prev);

        if (err == ESP_OK) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddBoolToObject(obj, "ok", true);
            cJSON_AddStringToObject(obj, "action", action);
            cJSON_AddNumberToObject(obj, "pin", pin);
            cJSON_AddNumberToObject(obj, "level", level);
            cJSON_AddNumberToObject(obj, "duration_ms", duration_ms);
            cJSON_AddNumberToObject(obj, "restored_level", prev);
            write_json_ok(obj, output, output_size);
            cJSON_Delete(obj);
        }
    } else {
        cJSON_Delete(root);
        return write_json_error("unknown_action", output, output_size);
    }

    cJSON_Delete(root);

    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        ESP_LOGE(TAG, "gpio tool failed: action=%s pin=%d err=%s", action, pin, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "gpio tool ok: action=%s pin=%d", action, pin);
    }
    return err;
}
