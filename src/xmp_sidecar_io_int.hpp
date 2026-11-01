// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef EXIV2_XMP_SIDECAR_IO_INT_HPP
#define EXIV2_XMP_SIDECAR_IO_INT_HPP

#include "basicio.hpp"
#include "config.h"

#ifdef EXV_ENABLE_FILESYSTEM
#include <cstdio>
#include <filesystem>
#ifndef _WIN32
#include <sys/types.h>
#ifdef EXV_HAVE_POSIX_ACL
#include <sys/acl.h>
#include <type_traits>
#endif
#endif

namespace Exiv2::Internal {
#ifdef EXV_HAVE_POSIX_ACL
struct XmpAclDeleter {
  void operator()(void* acl) const {
    acl_free(acl);
  }
};
using XmpAcl = std::unique_ptr<std::remove_pointer_t<acl_t>, XmpAclDeleter>;
#endif

// Injectable operations keep filesystem failure tests deterministic and local.
struct XmpFileOps {
  virtual ~XmpFileOps() = default;
  virtual int create(const std::filesystem::path& path, bool replacement) const;
#ifndef _WIN32
#ifdef EXV_HAVE_POSIX_ACL
  virtual XmpAcl readAcl(int fd) const;
  virtual bool writeAcl(int fd, const XmpAcl& acl) const;
#endif
  virtual void setOwner(int fd, uid_t owner, gid_t group) const;
  virtual void setMode(int fd, mode_t mode) const;
#endif
  virtual size_t write(FILE* file, const byte* data, size_t size) const;
  virtual int close(FILE* file) const;
  virtual void replace(const std::filesystem::path& source, const std::filesystem::path& target) const;
};

// Publish only complete output. No operation after replacement may throw.
void replaceXmpFile(const std::string& path, BasicIo& source, const XmpFileOps& ops = XmpFileOps{});
BasicIo::UniquePtr deferredXmpIo(const std::string& path);
bool isDeferredXmpIo(const BasicIo& io);
bool isUncommittedXmpIo(const BasicIo& io);
}  // namespace Exiv2::Internal
#endif
#endif
