# store_scan_bonds against NVS on native_sim's simulated flash: the one read of the bond store
# that ownership rests on (see src/store.c).
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(store_test)

set(APP ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_sources(app PRIVATE src/main.c ${APP}/store.c)
target_include_directories(app PRIVATE ${APP})
