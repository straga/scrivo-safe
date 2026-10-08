#include "hw_facts.h"

#include "esp_chip_info.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

// The names people know the chips by; a model missing here is shown as the target the image is built for.
static const char *chip_name(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32: return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C3: return "ESP32-C3";
    case CHIP_ESP32C6: return "ESP32-C6";
    default: return CONFIG_IDF_TARGET;
    }
}

// PSRAM inside the chip package, read from efuse: the image does not bring PSRAM up (stage 3.19), so a separate PSRAM
// chip next to the one on the module is not seen this way.
static int64_t psram_in_package(const esp_chip_info_t *info)
{
#if CONFIG_IDF_TARGET_ESP32S3
    // PSRAM_CAP, components/efuse/esp32s3/esp_efuse_table.csv: {0: None; 1: 8M; 2: 2M; 3: 16M; 4: 4M}
    static const int64_t megabytes[] = {0, 8, 2, 16, 4};
    uint8_t cap = 0;
    if (esp_efuse_read_field_blob(ESP_EFUSE_PSRAM_CAP, &cap, esp_efuse_get_field_size(ESP_EFUSE_PSRAM_CAP)) != ESP_OK ||
        cap >= sizeof(megabytes) / sizeof(megabytes[0])) {
        return -1;
    }
    return megabytes[cap] * 1024 * 1024;
#elif CONFIG_IDF_TARGET_ESP32
    // The package says whether PSRAM is inside (CHIP_FEATURE_EMB_PSRAM), not how much: present but unknown is -1.
    return (info->features & CHIP_FEATURE_EMB_PSRAM) ? -1 : 0;
#else
    (void)info;
    return -1;
#endif
}

void hw_facts_read(hw_facts_t *facts)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint32_t flash = 0;
    *facts = (hw_facts_t){
        .chip = chip_name(info.model),
        .revision = info.revision,
        .cores = info.cores,
        .flash_bytes = esp_flash_get_size(NULL, &flash) == ESP_OK ? (int64_t)flash : -1,
        .psram_bytes = psram_in_package(&info),
        // Both counted over the same heap regions, the ones malloc hands out: without PSRAM brought up, the chip's
        // own RAM that the image did not take for itself.
        .heap_free_bytes = heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
        .heap_total_bytes = heap_caps_get_total_size(MALLOC_CAP_DEFAULT),
    };
}
