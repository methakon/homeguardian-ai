# generate_web_assets.cmake - embed src/web/index.html into a generated header.
#
# Run at BUILD time via `cmake -P`, so editing the dashboard cannot leave a stale
# page embedded in a built binary (the previous configure_time `file(READ)` +
# `configure_file` pair produced exactly that: `make` had no dependency on
# index.html, so the demo kept serving the previous dashboard until someone
# happened to re-run cmake).
#
# Required -D arguments:
#   HG_HTML      absolute path to src/web/index.html (single source of truth)
#   HG_TEMPLATE  absolute path to cmake/hg_web_assets.h.in
#   HG_OUT       absolute path of the header to write
#
# The output is only rewritten when its content actually changes, so a no-op
# build stays a no-op (no needless recompiles/relinks).

foreach(required HG_HTML HG_TEMPLATE HG_OUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "generate_web_assets.cmake: ${required} is required")
    endif()
endforeach()

if(NOT EXISTS "${HG_HTML}")
    message(FATAL_ERROR "generate_web_assets.cmake: missing ${HG_HTML}")
endif()
if(NOT EXISTS "${HG_TEMPLATE}")
    message(FATAL_ERROR "generate_web_assets.cmake: missing ${HG_TEMPLATE}")
endif()

file(READ "${HG_HTML}" html)
file(READ "${HG_TEMPLATE}" tmpl)

string(REPLACE "@HG_DASHBOARD_HTML@" "${html}" generated "${tmpl}")

set(changed TRUE)
if(EXISTS "${HG_OUT}")
    file(READ "${HG_OUT}" existing)
    if(existing STREQUAL generated)
        set(changed FALSE)
    endif()
endif()

if(changed)
    get_filename_component(out_dir "${HG_OUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${out_dir}")
    file(WRITE "${HG_OUT}" "${generated}")
    message(STATUS "generate_web_assets: embedded ${HG_HTML} -> ${HG_OUT}")
else()
    message(STATUS "generate_web_assets: up to date")
endif()
