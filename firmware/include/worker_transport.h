#pragma once
#include <stdint.h>
#if defined(CONFIG_IDF_TARGET_ESP32P4)
extern const uint8_t worker_ca_start[] asm("_binary_certs_x509_crt_bundle_start");
extern const uint8_t worker_ca_end[] asm("_binary_certs_x509_crt_bundle_end");
#endif
