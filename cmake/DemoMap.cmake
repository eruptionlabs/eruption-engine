# Demo map download.
#
# The parana_field map ships its .env and .ter in the repository, but its .glb
# (about 210 MB) is larger than GitHub allows for a single file. It lives as a
# release asset instead. If the .glb is missing, the first configure downloads
# the zip, checks its SHA-256 and extracts it into the source tree.
#
# Turn it off with -DERUPTION_FETCH_DEMO_MAP=OFF. Offline configures only warn.
# Manual alternative: tools/fetch_demo_map.sh

option(ERUPTION_FETCH_DEMO_MAP "Download the demo map (release asset) when it is missing" ON)
set(ERUPTION_DEMO_MAP_URL
    "https://github.com/eruptionlabs/eruption-engine/releases/download/demo-map-v1/parana_field.zip"
    CACHE STRING "Release asset with the demo map")
set(ERUPTION_DEMO_MAP_SHA256 "40396b6ac5f3d97fb6ad636d8d6d6284a7f8c750f5a54c74ddd9695533a214a3")

set(_demo_glb "${CMAKE_SOURCE_DIR}/assets/external/parana_field/parana_field.glb")
if(ERUPTION_FETCH_DEMO_MAP AND NOT EXISTS "${_demo_glb}")
    set(_demo_zip "${CMAKE_BINARY_DIR}/parana_field.zip")
    message(STATUS "Demo map missing, downloading ${ERUPTION_DEMO_MAP_URL}")
    file(DOWNLOAD "${ERUPTION_DEMO_MAP_URL}" "${_demo_zip}"
         SHOW_PROGRESS STATUS _demo_status TLS_VERIFY ON)
    list(GET _demo_status 0 _demo_code)
    if(NOT _demo_code EQUAL 0)
        list(GET _demo_status 1 _demo_msg)
        message(WARNING "Demo map download failed (${_demo_msg}). The engine builds, but "
                        "parana_field will not load. Run tools/fetch_demo_map.sh later.")
    else()
        file(SHA256 "${_demo_zip}" _demo_hash)
        if(NOT _demo_hash STREQUAL ERUPTION_DEMO_MAP_SHA256)
            message(WARNING "Demo map checksum mismatch (got ${_demo_hash}). Not extracting.")
        else()
            file(ARCHIVE_EXTRACT INPUT "${_demo_zip}" DESTINATION "${CMAKE_SOURCE_DIR}")
            message(STATUS "Demo map installed: ${_demo_glb}")
        endif()
    endif()
    file(REMOVE "${_demo_zip}")
endif()
