# net link library — raw TCP/UDP delivering into the script's yield points
set(LINK_NAME net)
set(LINK_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/net.cpp)

add_library(${LINK_NAME} SHARED ${LINK_SRC})
target_include_directories(${LINK_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/lib/alxlib/include/alxbase
    ${PROJECT_SOURCE_DIR}/lib/alxlib/include/alxcore
    ${PROJECT_SOURCE_DIR}/lib/alxlib/include/alxcomm
    ${PROJECT_SOURCE_DIR}/lib/alxlib/include/alxscpt
)
target_link_libraries(${LINK_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/bin/alxlib/libalxbase.so
    ${CMAKE_SOURCE_DIR}/bin/alxlib/libalxcomm.so
)
set_target_properties(${LINK_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME ${LINK_NAME}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/links
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/../alxlib"
)
set_property(TARGET ${LINK_NAME} APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/net.json)
add_custom_command(TARGET ${LINK_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/net.json
        ${CMAKE_SOURCE_DIR}/bin/links/net.json
)
