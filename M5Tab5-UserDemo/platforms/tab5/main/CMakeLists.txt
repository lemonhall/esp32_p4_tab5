file(GLOB_RECURSE APP_LAYER_SRCS
    ../../../app/*.c
    ../../../app/*.cc
    ../../../app/*.cpp
)

set(APP_LAYER_INCS
    ../../../app
)

file(GLOB_RECURSE MY_HAL_SRCS
    ./hal/*.c
    ./hal/*.cc
    ./hal/*.cpp
)

set(_wifi_secrets_h "${CMAKE_BINARY_DIR}/generated/wifi_secrets.h")
get_filename_component(_tab5_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_wifi_secrets_script "${_tab5_root}/cmake/gen_wifi_secrets.cmake")
get_filename_component(_repo_root "${_tab5_root}/../../.." ABSOLUTE)
set(_wifi_env_candidates
    "${_tab5_root}/.env"
    "${_repo_root}/.env"
)
set(_wifi_env_file "")
foreach(_candidate IN LISTS _wifi_env_candidates)
    if(EXISTS "${_candidate}")
        set(_wifi_env_file "${_candidate}")
        break()
    endif()
endforeach()

if(NOT _wifi_env_file STREQUAL "")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_wifi_env_file}")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -DINPUT=${_wifi_env_file} -DOUTPUT=${_wifi_secrets_h} -P ${_wifi_secrets_script}
    RESULT_VARIABLE _wifi_gen_ret
)
if(NOT _wifi_gen_ret EQUAL 0)
    message(FATAL_ERROR "Failed to generate wifi_secrets.h from .env (ret=${_wifi_gen_ret})")
endif()

idf_component_register(SRCS "app_main.cpp" ${APP_LAYER_SRCS} ${MY_HAL_SRCS}
                    INCLUDE_DIRS "." ${APP_LAYER_INCS} "${CMAKE_BINARY_DIR}/generated"
                    EMBED_TXTFILES "../audio/canon_in_d.mp3" "../audio/startup_sfx.mp3" "../audio/shutdown_sfx.mp3")
