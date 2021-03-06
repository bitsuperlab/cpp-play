list( APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/libraries/fc/GitVersionGen" )
include( GetGitRevisionDescription )
get_git_head_revision(GIT_REFSPEC BITSHARES_GIT_REVISION_SHA)
get_git_unix_timestamp(BITSHARES_GIT_REVISION_UNIX_TIMESTAMP)
git_describe(BITSHARES_GIT_REVISION_DESCRIPTION --tags)
if(NOT BITSHARES_GIT_REVISION_DESCRIPTION)
    set(BITSHARES_GIT_REVISION_DESCRIPTION "unknown")
endif(NOT BITSHARES_GIT_REVISION_DESCRIPTION)

IF( WIN32 )
  add_definitions(-DCURL_STATICLIB) # force statick link
ENDIF(WIN32)


file(GLOB headers "include/bts/utilities/*.hpp")

set(sources http_downloader.cpp key_conversion.cpp string_escape.cpp
            words.cpp combinatorics.cpp
            ${headers})

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/git_revision.cpp.in" "${CMAKE_CURRENT_BINARY_DIR}/git_revision.cpp" @ONLY)
list(APPEND sources "${CMAKE_CURRENT_BINARY_DIR}/git_revision.cpp")

add_library( bts_utilities
             ${sources}
             ${HEADERS} )
target_link_libraries( bts_utilities fc ${CURL_LIBRARY})
target_include_directories( bts_utilities
                            PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" ${CURL_INCLUDE_DIRS})
if (USE_PCH)
  set_target_properties(bts_utilities PROPERTIES COTIRE_ADD_UNITY_BUILD FALSE)
  cotire(bts_utilities)
endif(USE_PCH)
