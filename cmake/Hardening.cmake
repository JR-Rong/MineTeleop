include_guard(GLOBAL)

include(CMakeParseArguments)
include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)

option(
  MINE_TELEOP_ENABLE_HARDENING
  "Enable feature-detected, target-scoped compiler and linker hardening"
  ON
)
option(
  MINE_TELEOP_ENABLE_SANITIZERS
  "Instrument Mine Teleop targets with the requested sanitizer set"
  OFF
)
set(
  MINE_TELEOP_SANITIZERS
  "address,undefined"
  CACHE STRING
  "Comma-separated sanitizer set used when MINE_TELEOP_ENABLE_SANITIZERS=ON"
)

if(MINE_TELEOP_ENABLE_HARDENING)
  if(MSVC)
    check_cxx_compiler_flag("/guard:cf" MINE_TELEOP_HAVE_MSVC_GUARD_CF_COMPILE)
    check_linker_flag(CXX "/guard:cf" MINE_TELEOP_HAVE_MSVC_GUARD_CF_LINK)
    check_linker_flag(CXX "/DYNAMICBASE" MINE_TELEOP_HAVE_MSVC_DYNAMICBASE)
    check_linker_flag(CXX "/NXCOMPAT" MINE_TELEOP_HAVE_MSVC_NXCOMPAT)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    check_cxx_compiler_flag(
      "-fstack-protector-strong"
      MINE_TELEOP_HAVE_STACK_PROTECTOR_STRONG
    )

    if(UNIX AND NOT APPLE)
      set(_mine_teleop_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
      set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -O2")
      check_cxx_compiler_flag(
        "-D_FORTIFY_SOURCE=3"
        MINE_TELEOP_HAVE_FORTIFY_SOURCE_3
      )
      set(CMAKE_REQUIRED_FLAGS "${_mine_teleop_saved_required_flags}")
      unset(_mine_teleop_saved_required_flags)

      check_linker_flag(
        CXX
        "-Wl,-z,relro,-z,now"
        MINE_TELEOP_HAVE_RELRO_NOW
      )
      check_linker_flag(
        CXX
        "-Wl,-z,noexecstack"
        MINE_TELEOP_HAVE_NOEXECSTACK
      )

      if(POLICY CMP0083)
        cmake_policy(SET CMP0083 NEW)
      endif()
      include(CheckPIESupported)
      check_pie_supported(
        OUTPUT_VARIABLE MINE_TELEOP_PIE_CHECK_OUTPUT
        LANGUAGES CXX
      )
      if(CMAKE_CXX_LINK_PIE_SUPPORTED)
        set(MINE_TELEOP_HAVE_PIE ON)
      endif()
    endif()
  endif()
endif()

if(MINE_TELEOP_ENABLE_SANITIZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR
      "MINE_TELEOP_ENABLE_SANITIZERS requires a GCC- or Clang-compatible C++ compiler. "
      "Use a dedicated ASan/UBSan preset; do not silently produce an uninstrumented build.")
  endif()
  if(MINE_TELEOP_SANITIZERS STREQUAL "")
    message(FATAL_ERROR "MINE_TELEOP_SANITIZERS must not be empty when sanitizers are enabled")
  endif()

  set(_mine_teleop_sanitizer_flag "-fsanitize=${MINE_TELEOP_SANITIZERS}")
  set(_mine_teleop_saved_required_link_options "${CMAKE_REQUIRED_LINK_OPTIONS}")
  list(APPEND CMAKE_REQUIRED_LINK_OPTIONS "${_mine_teleop_sanitizer_flag}")
  check_cxx_compiler_flag(
    "${_mine_teleop_sanitizer_flag}"
    MINE_TELEOP_HAVE_SANITIZER_COMPILE
  )
  set(CMAKE_REQUIRED_LINK_OPTIONS "${_mine_teleop_saved_required_link_options}")
  unset(_mine_teleop_saved_required_link_options)
  check_linker_flag(
    CXX
    "${_mine_teleop_sanitizer_flag}"
    MINE_TELEOP_HAVE_SANITIZER_LINK
  )
  check_cxx_compiler_flag(
    "-fno-sanitize-recover=all"
    MINE_TELEOP_HAVE_NO_SANITIZER_RECOVERY
  )
  check_cxx_compiler_flag(
    "-fno-omit-frame-pointer"
    MINE_TELEOP_HAVE_SANITIZER_FRAME_POINTERS
  )
  if(NOT MINE_TELEOP_HAVE_SANITIZER_COMPILE OR
     NOT MINE_TELEOP_HAVE_SANITIZER_LINK OR
     NOT MINE_TELEOP_HAVE_NO_SANITIZER_RECOVERY)
    message(FATAL_ERROR
      "The requested sanitizer set (${MINE_TELEOP_SANITIZERS}) is not fully supported by "
      "this compiler/linker. Refuse to run a partial sanitizer build.")
  endif()
  unset(_mine_teleop_sanitizer_flag)
endif()

# check_*() intentionally needs an undefined result variable. Default only
# after probes have run so unsupported platforms remain explicit without
# suppressing feature detection on supported toolchains.
foreach(_mine_teleop_feature
    MINE_TELEOP_HAVE_STACK_PROTECTOR_STRONG
    MINE_TELEOP_HAVE_FORTIFY_SOURCE_3
    MINE_TELEOP_HAVE_PIE
    MINE_TELEOP_HAVE_RELRO_NOW
    MINE_TELEOP_HAVE_NOEXECSTACK
    MINE_TELEOP_HAVE_MSVC_GUARD_CF_COMPILE
    MINE_TELEOP_HAVE_MSVC_GUARD_CF_LINK
    MINE_TELEOP_HAVE_MSVC_DYNAMICBASE
    MINE_TELEOP_HAVE_MSVC_NXCOMPAT)
  if(NOT DEFINED ${_mine_teleop_feature})
    set(${_mine_teleop_feature} OFF)
  endif()
endforeach()
unset(_mine_teleop_feature)

function(mine_teleop_enable_target_hardening target)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "Cannot harden unknown target: ${target}")
  endif()

  cmake_parse_arguments(ARG "STATIC_EXECUTABLE" "" "" ${ARGN})
  get_target_property(_mine_teleop_target_type "${target}" TYPE)
  set(_mine_teleop_target_links OFF)
  if(_mine_teleop_target_type STREQUAL "EXECUTABLE" OR
     _mine_teleop_target_type STREQUAL "SHARED_LIBRARY" OR
     _mine_teleop_target_type STREQUAL "MODULE_LIBRARY")
    set(_mine_teleop_target_links ON)
  endif()

  if(MINE_TELEOP_ENABLE_HARDENING)
    if(MSVC)
      if(MINE_TELEOP_HAVE_MSVC_GUARD_CF_COMPILE)
        target_compile_options("${target}" PRIVATE /guard:cf)
      endif()
      if(_mine_teleop_target_links)
        if(MINE_TELEOP_HAVE_MSVC_GUARD_CF_LINK)
          target_link_options("${target}" PRIVATE /guard:cf)
        endif()
        if(MINE_TELEOP_HAVE_MSVC_DYNAMICBASE)
          target_link_options("${target}" PRIVATE /DYNAMICBASE)
        endif()
        if(MINE_TELEOP_HAVE_MSVC_NXCOMPAT)
          target_link_options("${target}" PRIVATE /NXCOMPAT)
        endif()
      endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      if(MINE_TELEOP_HAVE_STACK_PROTECTOR_STRONG)
        target_compile_options("${target}" PRIVATE -fstack-protector-strong)
      endif()
      if(UNIX AND NOT APPLE AND MINE_TELEOP_HAVE_FORTIFY_SOURCE_3)
        # Ubuntu's compiler specs may inject _FORTIFY_SOURCE=2 at -O2. Undefine
        # that default immediately before selecting level 3 so hardened builds
        # do not emit a redefinition warning or silently retain level 2.
        target_compile_options(
          "${target}"
          PRIVATE
            "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:-U_FORTIFY_SOURCE>"
            "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:-D_FORTIFY_SOURCE=3>"
        )
      endif()

      if(UNIX AND NOT APPLE AND NOT ARG_STATIC_EXECUTABLE)
        if(_mine_teleop_target_type STREQUAL "EXECUTABLE" AND MINE_TELEOP_HAVE_PIE)
          set_property(TARGET "${target}" PROPERTY POSITION_INDEPENDENT_CODE ON)
        endif()
        if(_mine_teleop_target_links)
          if(MINE_TELEOP_HAVE_RELRO_NOW)
            target_link_options("${target}" PRIVATE -Wl,-z,relro,-z,now)
          endif()
          if(MINE_TELEOP_HAVE_NOEXECSTACK)
            target_link_options("${target}" PRIVATE -Wl,-z,noexecstack)
          endif()
        endif()
      elseif(APPLE AND _mine_teleop_target_type STREQUAL "EXECUTABLE" AND
             NOT ARG_STATIC_EXECUTABLE)
        # CMake/Apple toolchains emit PIE executables when supported. Keep this
        # target property local rather than turning on -fPIE for every library.
        set_property(TARGET "${target}" PROPERTY POSITION_INDEPENDENT_CODE ON)
      endif()
    endif()
  endif()

  if(MINE_TELEOP_ENABLE_SANITIZERS)
    if(ARG_STATIC_EXECUTABLE)
      message(FATAL_ERROR
        "${target} is configured as a static executable, which is incompatible with the "
        "ASan/UBSan runtime. Configure with MINE_TELEOP_STATIC_LAUNCHER=OFF.")
    endif()
    set(_mine_teleop_sanitizer_flag "-fsanitize=${MINE_TELEOP_SANITIZERS}")
    target_compile_options(
      "${target}"
      PRIVATE
        "${_mine_teleop_sanitizer_flag}"
        -fno-sanitize-recover=all
    )
    if(MINE_TELEOP_HAVE_SANITIZER_FRAME_POINTERS)
      target_compile_options("${target}" PRIVATE -fno-omit-frame-pointer)
    endif()
    if(_mine_teleop_target_links)
      target_link_options("${target}" PRIVATE "${_mine_teleop_sanitizer_flag}")
    endif()
    unset(_mine_teleop_sanitizer_flag)
  endif()
endfunction()

message(STATUS
  "Mine Teleop hardening: enabled=${MINE_TELEOP_ENABLE_HARDENING}; "
  "stack_protector=${MINE_TELEOP_HAVE_STACK_PROTECTOR_STRONG}; "
  "fortify3=${MINE_TELEOP_HAVE_FORTIFY_SOURCE_3}; "
  "pie=${MINE_TELEOP_HAVE_PIE}; relro_now=${MINE_TELEOP_HAVE_RELRO_NOW}; "
  "noexecstack=${MINE_TELEOP_HAVE_NOEXECSTACK}; "
  "sanitizers=${MINE_TELEOP_ENABLE_SANITIZERS}")
