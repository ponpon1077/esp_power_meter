cmake_minimum_required(VERSION 3.16)

# Déclare la table de partition personnalisée
set(PARTITION_TABLE_CSV "${CMAKE_CURRENT_LIST_DIR}/partitions.csv")

# Inclusion des outils CMake d'ESP-IDF
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32_power_meter)
