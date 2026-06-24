#pragma once

#include <stddef.h>

#define EVENT_LOG_MAX_ENTRIES 30
#define EVENT_LOG_MESSAGE_MAX_LEN 96

typedef struct {
    unsigned long uptime_s;
    char message[EVENT_LOG_MESSAGE_MAX_LEN];
} event_log_entry_t;

void event_log_add(const char *fmt, ...);
size_t event_log_get_entries(event_log_entry_t *entries, size_t max_entries);
void event_log_clear(void);
