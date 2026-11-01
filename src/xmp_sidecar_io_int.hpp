// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef EXIV2_XMP_SIDECAR_IO_INT_HPP
#define EXIV2_XMP_SIDECAR_IO_INT_HPP

#include "basicio.hpp"
#include "config.h"

#ifdef EXV_ENABLE_FILESYSTEM
#include <cstdio>
#include <filesystem>

namespace Exiv2::Internal {
// Injectable operations keep filesystem failure tests deterministic and local.
struct XmpFileOps {
  virtual ~XmpFileOps() = default;
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
