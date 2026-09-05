include_guard(GLOBAL)

# MSVC rejects a single narrow string literal once its encoded payload reaches
# roughly 16 KiB (C2026). Keep a generous margin for source/execution encoding
# differences and split only at line boundaries so UTF-8 code points are never
# divided between raw-string tokens.
set(MINE_TELEOP_RAW_STRING_CHUNK_MAX_BYTES 12000)

function(
  mine_teleop_make_adjacent_raw_literals
  body
  delimiter
  output_literals
  output_chunk_count
  output_max_chunk_bytes
)
  set(remaining "${body}")
  set(literals "")
  set(roundtrip "")
  set(chunk_count 0)
  set(max_chunk_bytes 0)
  set(terminator ")${delimiter}\"")

  string(FIND "${remaining}" "${terminator}" delimiter_position)
  if(NOT delimiter_position EQUAL -1)
    message(FATAL_ERROR "embedded text contains the C++ raw-string terminator ${terminator}")
  endif()

  string(LENGTH "${remaining}" remaining_bytes)
  if(remaining_bytes EQUAL 0)
    set(literals "R\"${delimiter}()${delimiter}\"")
    set(chunk_count 1)
  endif()

  while(remaining_bytes GREATER 0)
    if(remaining_bytes GREATER MINE_TELEOP_RAW_STRING_CHUNK_MAX_BYTES)
      string(SUBSTRING "${remaining}" 0 ${MINE_TELEOP_RAW_STRING_CHUNK_MAX_BYTES} window)
      string(FIND "${window}" "\n" split_position REVERSE)
      if(split_position EQUAL -1)
        # Minified text may not have newlines. Any ASCII separator is also a
        # safe UTF-8 boundary, and adjacent C++ literals preserve the bytes on
        # both sides even when the split lands inside JavaScript or HTML syntax.
        foreach(separator " " ";" "," "}" "{" ")" "(" "]" "[" ">" "<" ":" "=")
          string(FIND "${window}" "${separator}" candidate_position REVERSE)
          if(candidate_position GREATER split_position)
            set(split_position ${candidate_position})
          endif()
        endforeach()
        if(split_position EQUAL -1)
          message(FATAL_ERROR
            "embedded text has no safe UTF-8 split point within "
            "${MINE_TELEOP_RAW_STRING_CHUNK_MAX_BYTES} bytes; add an ASCII separator")
        endif()
      endif()
      math(EXPR chunk_bytes "${split_position} + 1")
    else()
      set(chunk_bytes ${remaining_bytes})
    endif()

    string(SUBSTRING "${remaining}" 0 ${chunk_bytes} chunk)
    # A separator is required here: without it, Clang parses the next raw
    # literal prefix as a user-defined-literal suffix on the previous token.
    string(APPEND literals "R\"${delimiter}(${chunk})${delimiter}\" ")
    string(APPEND roundtrip "${chunk}")
    math(EXPR chunk_count "${chunk_count} + 1")
    if(chunk_bytes GREATER max_chunk_bytes)
      set(max_chunk_bytes ${chunk_bytes})
    endif()

    if(chunk_bytes EQUAL remaining_bytes)
      set(remaining "")
    else()
      string(SUBSTRING "${remaining}" ${chunk_bytes} -1 remaining)
    endif()
    string(LENGTH "${remaining}" remaining_bytes)
  endwhile()

  string(SHA256 expected_hash "${body}")
  string(SHA256 actual_hash "${roundtrip}")
  if(NOT actual_hash STREQUAL expected_hash)
    message(FATAL_ERROR "raw-string chunking changed the embedded text")
  endif()
  if(max_chunk_bytes GREATER MINE_TELEOP_RAW_STRING_CHUNK_MAX_BYTES)
    message(FATAL_ERROR "generated raw-string chunk exceeds the configured safe limit")
  endif()

  set(${output_literals} "${literals}" PARENT_SCOPE)
  set(${output_chunk_count} ${chunk_count} PARENT_SCOPE)
  set(${output_max_chunk_bytes} ${max_chunk_bytes} PARENT_SCOPE)
endfunction()

function(mine_teleop_split_cpp_raw_literals input_path output_path delimiter)
  file(READ "${input_path}" source)
  # Make generated output deterministic across Windows and Unix checkouts.
  string(REPLACE "\r\n" "\n" source "${source}")
  string(REPLACE "\r" "\n" source "${source}")

  set(opener "R\"${delimiter}(")
  set(terminator ")${delimiter}\"")
  string(LENGTH "${opener}" opener_bytes)
  string(LENGTH "${terminator}" terminator_bytes)
  set(remaining "${source}")
  set(generated "")
  set(raw_literal_count 0)

  while(TRUE)
    string(FIND "${remaining}" "${opener}" opener_position)
    if(opener_position EQUAL -1)
      string(APPEND generated "${remaining}")
      break()
    endif()

    string(SUBSTRING "${remaining}" 0 ${opener_position} prefix)
    string(APPEND generated "${prefix}")
    math(EXPR body_position "${opener_position} + ${opener_bytes}")
    string(SUBSTRING "${remaining}" ${body_position} -1 after_opener)
    string(FIND "${after_opener}" "${terminator}" terminator_position)
    if(terminator_position EQUAL -1)
      message(FATAL_ERROR "unterminated ${delimiter} raw string in ${input_path}")
    endif()

    string(SUBSTRING "${after_opener}" 0 ${terminator_position} body)
    mine_teleop_make_adjacent_raw_literals(
      "${body}" "${delimiter}" adjacent_literals chunk_count max_chunk_bytes)
    string(APPEND generated "${adjacent_literals}")
    math(EXPR raw_literal_count "${raw_literal_count} + 1")

    math(EXPR suffix_position "${terminator_position} + ${terminator_bytes}")
    string(SUBSTRING "${after_opener}" ${suffix_position} -1 remaining)
  endwhile()

  if(raw_literal_count EQUAL 0)
    message(FATAL_ERROR "no ${delimiter} raw strings found in ${input_path}")
  endif()
  get_filename_component(output_directory "${output_path}" DIRECTORY)
  file(MAKE_DIRECTORY "${output_directory}")
  file(WRITE "${output_path}" "${generated}")
endfunction()
