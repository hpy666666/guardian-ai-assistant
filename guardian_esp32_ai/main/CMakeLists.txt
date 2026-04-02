set(SOURCES
    "main.cc"
    "application.cc"
    "settings.cc"
    "system_info.cc"
    "audio/audio_codec.cc"
    "audio/audio_service.cc"
    "audio/codecs/no_audio_codec.cc"
    "audio/processors/no_audio_processor.cc"
    "protocols/protocol.cc"
    "protocols/websocket_protocol.cc"
    # Sensor gateway: UART(F407) -> JSON -> MQTT
    "sensor_gateway/sensor_gateway.c"
)

set(INCLUDE_DIRS "." "audio" "protocols" "sensor_gateway")

# Board common files
file(GLOB BOARD_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/boards/common/*.cc)
list(APPEND SOURCES ${BOARD_COMMON_SOURCES})
list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards/common)

# Guardian S3 board files
set(BOARD_TYPE "guardian-s3")
set(BOARD_NAME "Guardian-ESP32-S3")
file(GLOB BOARD_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_TYPE}/*.cc)
list(APPEND SOURCES ${BOARD_SOURCES})
list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_TYPE})

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        spi_flash
        driver
        esp_driver_i2s
        json
        esp_hw_support
        espressif__esp_websocket_client
        78__esp-opus
        78__esp-opus-encoder
        mqtt
        esp_timer
        newlib
    PRIV_REQUIRES
        esp_app_format
    WHOLE_ARCHIVE
)

target_compile_definitions(${COMPONENT_LIB}
    PRIVATE
    BOARD_TYPE=\"${BOARD_TYPE}\"
    BOARD_NAME=\"${BOARD_NAME}\"
)
