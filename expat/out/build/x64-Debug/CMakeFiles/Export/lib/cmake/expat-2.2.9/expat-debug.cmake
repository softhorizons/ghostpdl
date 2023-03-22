#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "expat::libexpat" for configuration "Debug"
set_property(TARGET expat::libexpat APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(expat::libexpat PROPERTIES
  IMPORTED_IMPLIB_DEBUG "${_IMPORT_PREFIX}/lib/libexpatd.lib"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/bin/libexpatd.dll"
  )

list(APPEND _IMPORT_CHECK_TARGETS expat::libexpat )
list(APPEND _IMPORT_CHECK_FILES_FOR_expat::libexpat "${_IMPORT_PREFIX}/lib/libexpatd.lib" "${_IMPORT_PREFIX}/bin/libexpatd.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
