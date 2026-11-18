cmake_minimum_required(VERSION 3.20)

# Run at build time, including on incremental builds.
find_program(git_program git)

function(build_identity_git output)
  execute_process(
    COMMAND "${git_program}" --no-optional-locks -C "${source_dir}" ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE value
    ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(result STREQUAL "0" AND error STREQUAL "")
    set(${output} "${value}" PARENT_SCOPE)
  endif()
endfunction()

set(build_id unavailable)
if(git_program)
  # An exported source tree inside an unrelated checkout is not a Git build.
  build_identity_git(marker cat-file -e HEAD:./xmake.lua)
  if(DEFINED marker)
    build_identity_git(revision rev-parse --verify "HEAD^{commit}")
    string(LENGTH "${revision}" revision_length)
    if(revision MATCHES "^[0-9a-f]+$" AND (revision_length EQUAL 40 OR revision_length EQUAL 64))
      build_identity_git(status status --porcelain=v1 --untracked-files=normal --ignore-submodules=none)
      if(DEFINED status)
        set(build_id "${revision}")
        if(NOT status STREQUAL "")
          string(APPEND build_id "-dirty")
        endif()
      endif()
    endif()
  endif()
endif()

file(CONFIGURE OUTPUT "${output_file}" CONTENT "#define MODLIST_BUILD_ID \"@build_id@\"\n" @ONLY NEWLINE_STYLE LF)
