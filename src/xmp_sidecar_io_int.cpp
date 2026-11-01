// SPDX-License-Identifier: GPL-2.0-or-later
#include "xmp_sidecar_io_int.hpp"

#ifdef EXV_ENABLE_FILESYSTEM
#include <array>
#include <atomic>
#include <cerrno>
#include <random>
#include <vector>
#include "error.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Exiv2::Internal {
namespace {
namespace fs = std::filesystem;

// Own the exclusive temporary file until replacement has succeeded.
struct TemporaryFile {
  fs::path path;
  FILE* file{};
  ~TemporaryFile() {
    if (file)
      std::fclose(file);
    std::error_code ignored;
    if (!path.empty())
      fs::remove(path, ignored);
  }
};

// Follow a symlink to its referent, including a missing final component.
fs::path destination(const std::string& name) {
  auto path = fs::absolute(fs::path(std::u8string(name.begin(), name.end())));
  for (unsigned depth = 0; depth != 40; ++depth) {
    if (!fs::is_symlink(fs::symlink_status(path)))
      return path;
    const auto link = fs::read_symlink(path);
    path = link.is_absolute() ? link : path.parent_path() / link;
  }
  throw Error(ErrorCode::kerImageWriteFailed);
}

// Create in the destination directory so replacement never crosses filesystems.
void createTemporary(TemporaryFile& temp, const fs::path& target) {
  static std::atomic<unsigned long> sequence{0};
  std::random_device random;
  for (unsigned attempt = 0; attempt != 100; ++attempt) {
    const auto candidate =
        target.parent_path() / (".exiv2-xmp-" + std::to_string(random()) + "-" + std::to_string(sequence++));
#ifdef _WIN32
    const int fd = _wopen(candidate.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    const int fd = ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
#endif
    if (fd < 0) {
      if (errno == EEXIST)
        continue;
      throw Error(ErrorCode::kerImageWriteFailed);
    }
    temp.path = candidate;
#ifdef _WIN32
    temp.file = _fdopen(fd, "wb");
    if (!temp.file)
      _close(fd);
#else
    temp.file = fdopen(fd, "wb");
    if (!temp.file)
      ::close(fd);
#endif
    if (!temp.file)
      throw Error(ErrorCode::kerImageWriteFailed);
    return;
  }
  throw Error(ErrorCode::kerImageWriteFailed);
}

// Preserve permissions before commit, and respect an unwritable old file.
void preservePermissions(const fs::path& target, const fs::path& temp) {
  if (!fs::exists(target))
    return;
  if (!fs::is_regular_file(target))
    throw Error(ErrorCode::kerImageWriteFailed);
#ifdef _WIN32
  const auto attributes = GetFileAttributesW(target.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_READONLY))
    throw Error(ErrorCode::kerImageWriteFailed);
  DWORD length = 0;
  GetFileSecurityW(target.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &length);
  if (!length)
    throw Error(ErrorCode::kerImageWriteFailed);
  std::vector<byte> descriptor(length);
  if (!GetFileSecurityW(target.c_str(), DACL_SECURITY_INFORMATION, descriptor.data(), length, &length) ||
      !SetFileSecurityW(temp.c_str(), DACL_SECURITY_INFORMATION, descriptor.data()))
    throw Error(ErrorCode::kerImageWriteFailed);
#else
  if (::access(target.c_str(), W_OK) != 0)
    throw Error(ErrorCode::kerImageWriteFailed);
  fs::permissions(temp, fs::status(target).permissions());
#endif
}
}  // namespace

size_t XmpFileOps::write(FILE* file, const byte* data, size_t size) const {
  return std::fwrite(data, 1, size, file);
}

int XmpFileOps::close(FILE* file) const {
  return std::fclose(file);
}

void XmpFileOps::replace(const fs::path& source, const fs::path& target) const {
#ifdef _WIN32
  if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
    throw Error(ErrorCode::kerImageWriteFailed);
#else
  fs::rename(source, target);
#endif
}

void replaceXmpFile(const std::string& path, BasicIo& source, const XmpFileOps& ops) {
  try {
    // Open the source before creating anything, and never truncate the target.
    if (source.open() != 0)
      throw Error(ErrorCode::kerImageWriteFailed);
    IoCloser sourceCloser(source);
    const auto target = destination(path);
    TemporaryFile temp;
    createTemporary(temp, target);
    preservePermissions(target, temp.path);
    std::array<byte, 8192> buffer;
    const auto expected = source.size();
    size_t total = 0;
    while (const auto count = source.read(buffer.data(), buffer.size())) {
      if (count > expected - total)
        throw Error(ErrorCode::kerImageWriteFailed);
      total += count;
      if (ops.write(temp.file, buffer.data(), count) != count)
        throw Error(ErrorCode::kerImageWriteFailed);
    }
    if (source.error() || total != expected)
      throw Error(ErrorCode::kerImageWriteFailed);
    auto* file = temp.file;
    temp.file = nullptr;
    if (ops.close(file) != 0)
      throw Error(ErrorCode::kerImageWriteFailed);
    ops.replace(temp.path, target);
    temp.path.clear();
  } catch (const fs::filesystem_error&) {
    throw Error(ErrorCode::kerImageWriteFailed);
  }
}
}  // namespace Exiv2::Internal
#endif
