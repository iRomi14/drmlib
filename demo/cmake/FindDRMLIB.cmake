
set(DRMLIB_FOUND false)

message(STATUS "Searching for DRMLib using ${DRMLIB_DIR}")

find_path(DRMLIB_INCLUDE_DIRS
  NAMES accelize/drm.h
  PATHS ${DRMLIB_DIR}/include/
)

find_library(DRMLIB_LIBRARIES
  NAMES accelize_drm
  PATHS ${DRMLIB_DIR}
  ${DRMLIB_DIR}
  ${DRMLIB_DIR}/lib
  ${DRMLIB_DIR}/build
)

find_library(DRMLIB_C_LIBRARIES
  NAMES accelize_drmc
  PATHS ${DRMLIB_DIR}
  ${DRMLIB_DIR}
  ${DRMLIB_DIR}/lib
  ${DRMLIB_DIR}/build
)

if(DRMLIB_LIBRARIES)
  message(STATUS "DRMLib Found lib : ${DRMLIB_LIBRARIES}")
endif()

if(DRMLIB_C_LIBRARIES)
  message(STATUS "DRMLib-C Found lib : ${DRMLIB_C_LIBRARIES}")
endif()

if(DRMLIB_INCLUDE_DIRS)
  message(STATUS "DRMLib Found includes : ${DRMLIB_INCLUDE_DIRS}")
endif()

if(DRMLIB_LIBRARIES AND DRMLIB_C_LIBRARIES AND DRMLIB_INCLUDE_DIRS)
  set(DRMLIB_FOUND true)
endif()

if(${DRMLIB_FIND_REQUIRED} AND NOT DRMLIB_FOUND)
  message(FATAL_ERROR "DRMLib not Found, please try to set DRMLIB_DIR to point to build directory")
endif()
