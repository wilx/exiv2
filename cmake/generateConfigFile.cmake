include(CheckIncludeFileCXX)
include(CheckCXXSourceCompiles)
include(CheckCXXSymbolExists)

# Note that the scope of the EXV_ variables in local
if (${EXIV2_ENABLE_WEBREADY})
    set(EXV_USE_CURL  ${EXIV2_ENABLE_CURL})
endif()
set(EXV_ENABLE_BMFF       ${EXIV2_ENABLE_BMFF})
set(EXV_ENABLE_WEBREADY   ${EXIV2_ENABLE_WEBREADY})
set(EXV_HAVE_LENSDATA     ${EXIV2_ENABLE_LENSDATA})
set(EXV_ENABLE_INIH       ${EXIV2_ENABLE_INIH})
set(EXV_ENABLE_FILESYSTEM ${EXIV2_ENABLE_FILESYSTEM_ACCESS})

set(EXV_PACKAGE_NAME     ${PROJECT_NAME})
set(EXV_PACKAGE_VERSION  ${PROJECT_VERSION})
set(EXV_PACKAGE_STRING   "${PROJECT_NAME} ${PROJECT_VERSION}")
if (${EXIV2_ENABLE_XMP} OR ${EXIV2_ENABLE_EXTERNAL_XMP})
    set(EXV_HAVE_XMP_TOOLKIT ON)
else()
    set(EXV_HAVE_XMP_TOOLKIT OFF)
endif()
set(EXV_HAVE_ICONV       ${ICONV_FOUND})
set(EXV_HAVE_LIBZ        ${ZLIB_FOUND})
set(EXV_HAVE_BROTLI      ${brotli_FOUND})

check_cxx_source_compiles("#include <format>\nint main(){std::format(\"t\");}" EXV_HAVE_STD_FORMAT)
check_cxx_symbol_exists(strerror_r  string.h       EXV_HAVE_STRERROR_R )

# Check the common POSIX.1e API; unavailable support uses Unix mode bits.
if(EXIV2_ENABLE_FILESYSTEM_ACCESS AND (CMAKE_SYSTEM_NAME MATCHES "^(FreeBSD|NetBSD)$" OR
   (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ACL_INCLUDE_DIR AND ACL_LIBRARY)))
  set(CMAKE_REQUIRED_INCLUDES ${ACL_INCLUDE_DIR})
  set(CMAKE_REQUIRED_LIBRARIES ${ACL_LIBRARY})
  check_cxx_source_compiles("#include <sys/types.h>
#include <sys/acl.h>
int main(){ auto a=acl_get_fd(-1); auto t=acl_to_text(a,nullptr); acl_set_fd(-1,a); acl_free(a); acl_free(t); }" EXV_HAVE_POSIX_ACL)
  unset(CMAKE_REQUIRED_INCLUDES)
  unset(CMAKE_REQUIRED_LIBRARIES)
else()
  set(EXV_HAVE_POSIX_ACL OFF)
endif()

set(EXV_ENABLE_NLS ${EXIV2_ENABLE_NLS})
set(EXV_ENABLE_VIDEO ${EXIV2_ENABLE_VIDEO})

configure_file(cmake/config.h.cmake ${CMAKE_BINARY_DIR}/exv_conf.h @ONLY)
