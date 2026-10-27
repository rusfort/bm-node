cmake_minimum_required(VERSION 3.16)

# Роль платы задаётся при сборке:
#   idf.py -B build_core -D NODE_ROLE=CORE -p <порт CORE> flash monitor
#   idf.py -B build_tx   -D NODE_ROLE=TX   -p <порт TX>   flash monitor
if(NOT DEFINED NODE_ROLE)
    message(FATAL_ERROR "Укажи роль: -D NODE_ROLE=CORE или -D NODE_ROLE=TX (и отдельный каталог -B)")
endif()
if(NOT NODE_ROLE STREQUAL "CORE" AND NOT NODE_ROLE STREQUAL "TX")
    message(FATAL_ERROR "NODE_ROLE должен быть CORE или TX, получено: ${NODE_ROLE}")
endif()

# У каждой роли свой sdkconfig внутри своего каталога сборки.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(COMPILE_DEFINITIONS "NODE_ROLE_${NODE_ROLE}=1" APPEND)
project(box_node_dual)
