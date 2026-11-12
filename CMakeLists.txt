cmake_minimum_required(VERSION 3.22)

# Env daq_mtp only (platformio.ini: board_build.cmake_extra_args = -DDAQ_MTP_USB=ON). Docs/MTP/Merge_Report.md MTP-M-2.
# The env daq (and all other envs) skip this block: no esp_tinyusb, the same sdkconfig and the same dependencies.lock as before.
if(DAQ_MTP_USB)
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/mtp_usb")
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/mtp_usb/daq_mtp.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(DAQ_MTP_USB)
    idf_build_set_property(DAQ_MTP_USB ON)     # a build property: the early requirement expansion of src/CMakeLists.txt reads it too
    idf_build_set_property(DEPENDENCIES_LOCK "dependencies.lock.daq_mtp")    # its own lock file: the lock of the env daq stays as it is
endif()
project(irec_srad_daq)
