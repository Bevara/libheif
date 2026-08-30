cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBHEIF_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/reframe_heif.c
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_heif.c
)

SET(LIBHEIF_LIB
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libheif.a
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libde265.a
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libc++.a
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libc++abi.a
)

SET(LIBHEIF_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libheif
        "${LIBHEIF_SRC}"
        "${LIBHEIF_LIB}"
        []
        ""
        "${LIBHEIF_INC}"
        "-s USE_ZLIB=1"
        "1")
