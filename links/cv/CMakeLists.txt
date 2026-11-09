# cv link library — OpenCV image operators
set(LINK_NAME cv)
set(LINK_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/cv.cpp)

set(ASCRIPT_OPENCV_ROOT "${CMAKE_SOURCE_DIR}/lib/opencv" CACHE PATH "Vendored OpenCV install tree")

if(NOT EXISTS "${ASCRIPT_OPENCV_ROOT}/lib/cmake/opencv4/OpenCVConfig.cmake")
    message(WARNING
        "OpenCV not found at ${ASCRIPT_OPENCV_ROOT}; the cv link is skipped.\n"
        "Install the static package as described in lib/opencv/README.md, "
        "or pass -DASCRIPT_OPENCV_ROOT=<install tree>.")
    return()
endif()

find_package(OpenCV 4.14 CONFIG REQUIRED COMPONENTS core imgproc imgcodecs
    PATHS "${ASCRIPT_OPENCV_ROOT}/lib/cmake/opencv4"
    NO_DEFAULT_PATH)

add_library(${LINK_NAME} SHARED ${LINK_SRC})
target_include_directories(${LINK_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/lib/alxlib/include/alxbase
    ${PROJECT_SOURCE_DIR}/lib/alxlib/include/alxcore
    ${PROJECT_SOURCE_DIR}/lib/alxlib/include/alxcomm
    ${PROJECT_SOURCE_DIR}/lib/alxlib/include/alxscpt
    ${OpenCV_INCLUDE_DIRS}
)
# libalxcore exports its own libjpeg/zlib symbols; the bundled OpenCV archives
# must resolve first, or the loader binds them to the ABI-incompatible copy.
file(GLOB OPENCV_3RDPARTY_LIBS "${ASCRIPT_OPENCV_ROOT}/lib/opencv4/3rdparty/*.a")
target_link_libraries(${LINK_NAME} PRIVATE
    ${OpenCV_LIBS}
    ${OPENCV_3RDPARTY_LIBS}
    ${CMAKE_SOURCE_DIR}/bin/alxlib/libalxbase.so
    ${CMAKE_SOURCE_DIR}/bin/alxlib/libalxcore.so
    ${CMAKE_SOURCE_DIR}/bin/alxlib/libalxcomm.so
)
# keep the statically linked OpenCV symbols local to cv.so
target_link_options(${LINK_NAME} PRIVATE "-Wl,--exclude-libs,ALL")
set_target_properties(${LINK_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME ${LINK_NAME}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/links
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/../alxlib"
)
# completion docs live next to the .so; LINK_DEPENDS makes a doc edit relink
set_property(TARGET ${LINK_NAME} APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cv.json)
add_custom_command(TARGET ${LINK_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/cv.json
        ${CMAKE_SOURCE_DIR}/bin/links/cv.json
)
