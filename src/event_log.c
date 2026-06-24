#include "event_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "event_log";

static event_log_entry_t s_entries[EVENT_LOG_MAX_ENTRIES];
static size_t s_next;
static size_t s_count;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void event_log_add(const char *fmt, ...)
{
    char message[EVENT_LOG_MESSAGE_MAX_LEN];
    va_list args;

    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    ESP_LOGI(TAG, "%s", message);

    portENTER_CRITICAL(&s_lock);
    s_entries[s_next].uptime_s = (unsigned long)(esp_timer_get_time() / 1000000ULL);
    strlcpy(s_entries[s_next].message, message, sizeof(s_entries[s_next].message));
    s_next = (s_next + 1) % EVENT_LOG_MAX_ENTRIES;
    if (s_count < EVENT_LOG_MAX_ENTRIES) {
        s_count++;
    }
    portEXIT_CRITICAL(&s_lock);
}

size_t event_log_get_entries(event_log_entry_t *entries, size_t max_entries)
{
    if (entries == NULL || max_entries == 0) {
        return 0;
    }

    portENTER_CRITICAL(&s_lock);
    size_t count = s_count < max_entries ? s_count : max_entries;
    size_t first = (s_next + EVENT_LOG_MAX_ENTRIES - s_count) % EVENT_LOG_MAX_ENTRIES;

    for (size_t i = 0; i < count; i++) {
        entries[i] = s_entries[(first + i) % EVENT_LOG_MAX_ENTRIES];
    }
    portEXIT_CRITICAL(&s_lock);

    return count;
}

void event_log_clear(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(s_entries, 0, sizeof(s_entries));
    s_next = 0;
    s_count = 0;
    portEXIT_CRITICAL(&s_lock);
}
