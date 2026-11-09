# cip link library — EtherNet/IP (CIP over TCP), client and server
set(LINK_NAME cip)
set(LINK_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cip.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/wire.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server.cpp
)

add_library(${LINK_NAME} SHARED ${LINK_SRC})
target_include_directories(${LINK_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/lib/alxlib/include/alxbase
    ${PROJECT_SOURCE_DIR}/lib/alxlib/include/alxcore
    ${PROJECT_SOURCE_DIR}/lib/alxlib/include/alxcomm
    ${PROJECT_SOURCE_DIR}/lib/alxlib/include/alxscpt
)
target_link_libraries(${LINK_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/bin/alxlib/libalxbase.so
    ${CMAKE_SOURCE_DIR}/bin/alxlib/libalxcore.so
    ${CMAKE_SOURCE_DIR}/bin/alxlib/libalxcomm.so
)
set_target_properties(${LINK_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME ${LINK_NAME}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/links
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/../alxlib"
)
# completion docs live next to the .so; LINK_DEPENDS makes a doc edit relink
set_property(TARGET ${LINK_NAME} APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cip.json)
add_custom_command(TARGET ${LINK_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/cip.json
        ${CMAKE_SOURCE_DIR}/bin/links/cip.json
)
