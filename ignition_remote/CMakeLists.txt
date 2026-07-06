# Ignition Remote
#
# Standalone ESP-IDF project: a dumb ignition-powered ESP32 that broadcasts
# an ESP-NOW heartbeat while powered. The main firmware (in ../main/)
# receives these heartbeats and treats presence == ignition on.
#
# The remote does NOT connect to WiFi, does NOT run MQTT, does NOT have OTA.
# It only brings up the WiFi radio (unassociated) so ESP-NOW can operate.
#
# Build & flash:
#   idf.py -C ignition_remote set-target esp32s3
#   idf.py -C ignition_remote menuconfig   # set ESPNOW_HEARTBEAT_INTERVAL_MS, ESPNOW_SECRET
#   idf.py -C ignition_remote -p /dev/cu.usbserial-XXXX flash monitor
#
# The ESPNOW_SECRET MUST match the main firmware's CONFIG_ESPNOW_SECRET.
# The shared protocol header (../main/ignition_espnow_protocol.h) is included
# directly so both sides agree on the wire format.

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ignition_remote)
