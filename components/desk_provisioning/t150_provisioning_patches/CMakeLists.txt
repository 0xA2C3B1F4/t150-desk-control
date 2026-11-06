idf_component_register()
# IDF reads component definitions in script mode to discover dependencies.
if(CMAKE_SCRIPT_MODE_FILE)
  return()
endif()
set(desk_wifi_target "${PROJECT_DIR}/src/esphome/components/wifi/wifi_component.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${PROJECT_DIR}/desk_apply_wifi_patch.py"
             "${PROJECT_DIR}/desk_upstream_wifi.cpp"
             "${desk_wifi_target}")
execute_process(COMMAND "${PYTHON}" "${PROJECT_DIR}/desk_apply_wifi_patch.py" "${PROJECT_DIR}"
                RESULT_VARIABLE desk_wifi_result)
if(NOT desk_wifi_result EQUAL 0)
  message(FATAL_ERROR "T150 provisioning Wi-Fi patch failed")
endif()
