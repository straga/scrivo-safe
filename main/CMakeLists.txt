# LittleFS v2 as MicroPython carries it (third_party/littlefs), built with MicroPython's flags (extmod/extmod.mk), so
# the recovery image reads and writes the vfs partition exactly as VfsLfs2 does (main/app_fs.h).
set(littlefs_dir ${CMAKE_CURRENT_LIST_DIR}/../third_party/littlefs)

idf_component_register(SRCS "main.c" "session.c" "form.c" "body.c" "app_fs.c" "app_fs_tree.c" "files_http.c" "files_zip.c"
                            "status_led.c" "ptable.c" "host_name.c" "hw_facts.c" "hw_facts_chip.c"
                            "${littlefs_dir}/lfs2.c" "${littlefs_dir}/lfs2_util.c"
                       INCLUDE_DIRS "." "${littlefs_dir}"
                       PRIV_REQUIRES app_update bootloader_support esp_app_format esp_partition esp_event
                                     esp_netif esp_wifi esp_http_server nvs_flash dns_server
                                     esp_driver_rmt esp_driver_gpio esp_timer spi_flash efuse)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LFS2_NO_MALLOC LFS2_NO_DEBUG LFS2_NO_WARN LFS2_NO_ERROR LFS2_NO_ASSERT)
# third-party source: its warnings are not ours to turn into errors
set_source_files_properties("${littlefs_dir}/lfs2.c" "${littlefs_dir}/lfs2_util.c"
    PROPERTIES COMPILE_OPTIONS "-Wno-error;-Wno-unused-function")
