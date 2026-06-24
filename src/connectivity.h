#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define CONNECTIVITY_RESULT_MAX_LEN 192

typedef struct {
    bool sta_connected;
    bool ping_ok;
    bool dns_ok;
    bool http_ok;
    bool internet_ok;
    char message[CONNECTIVITY_RESULT_MAX_LEN];
} connectivity_test_result_t;

void connectivity_start(void);
bool connectivity_is_internet_ok(void);
void connectivity_set_internet_ok(bool ok);
esp_err_t connectivity_run_manual_test(connectivity_test_result_t *result);
