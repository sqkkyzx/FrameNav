# Source retained from AlphaLoc and extended by FrameNav.
# SPDX-License-Identifier: Apache-2.0

# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

set(FRAMENAV_CERT_DIR ${CMAKE_SOURCE_DIR}/certs)
set(FRAMENAV_CERT_FILE ${FRAMENAV_CERT_DIR}/server_cert.pem)
set(FRAMENAV_KEY_FILE ${FRAMENAV_CERT_DIR}/server_key.pem)

if(EXISTS ${FRAMENAV_CERT_FILE} AND EXISTS ${FRAMENAV_KEY_FILE})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${FRAMENAV_CERT_FILE} ${FRAMENAV_KEY_FILE}
    ${CMAKE_SOURCE_DIR}/scripts/embed_tls_cert.py)
  set(FRAMENAV_EMBEDDED_TLS 1)
  message(STATUS "FrameNav: embedding local HTTPS certificate and key")
  set(FRAMENAV_TLS_SOURCE ${FRAMENAV_CERT_DIR}/framenav_tls_embedded.c)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/embed_tls_cert.py
            --cert ${FRAMENAV_CERT_FILE}
            --key ${FRAMENAV_KEY_FILE}
            --output ${FRAMENAV_TLS_SOURCE}
    RESULT_VARIABLE FRAMENAV_TLS_GENERATE_RESULT
  )
  if(NOT FRAMENAV_TLS_GENERATE_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate certs/framenav_tls_embedded.c")
  endif()
  list(APPEND app_sources ${FRAMENAV_TLS_SOURCE})
  idf_component_register(
    SRCS ${app_sources}
    PRIV_REQUIRES esp_https_server esp_http_client)
else()
  set(FRAMENAV_EMBEDDED_TLS 0)
  message(WARNING "FrameNav: certs/server_cert.pem and certs/server_key.pem are missing; HTTPS server disabled")
  idf_component_register(
    SRCS ${app_sources}
    PRIV_REQUIRES esp_https_server esp_http_client)
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE FRAMENAV_EMBEDDED_TLS=${FRAMENAV_EMBEDDED_TLS})
