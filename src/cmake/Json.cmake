# Defines nlohmann_json::nlohmann_json as a header-only interface target via
# FetchContent. Idempotent.
if(TARGET nlohmann_json::nlohmann_json)
    return()
endif()

include(FetchCache)
include(FetchContent)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(nlohmann_json)
