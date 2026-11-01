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
void createTemporary(TemporaryFile& temp, const fs::path& target, bool replacement, const XmpFileOps& ops) {
  static std::atomic<unsigned long> sequence{0};
  std::random_device random;
  for (unsigned attempt = 0; attempt != 100; ++attempt) {
    const auto candidate =
        target.parent_path() / (".exiv2-xmp-" + std::to_string(random()) + "-" + std::to_string(sequence++));
    const int fd = ops.create(candidate, replacement);
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

#ifdef _WIN32
// Preserve permissions before commit, and respect an unwritable old file.
void preservePermissions(const fs::path& target, const fs::path& temp) {
  if (!fs::exists(target))
    return;
  if (!fs::is_regular_file(target))
    throw Error(ErrorCode::kerImageWriteFailed);
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
}
#else
struct Descriptor {
  int fd;
  explicit Descriptor(int value) : fd(value) {
  }
  ~Descriptor() {
    if (fd >= 0)
      ::close(fd);
  }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
};

#ifdef EXV_HAVE_POSIX_ACL
bool unsupportedAcl(int error) {
  return error == ENOTSUP || error == EOPNOTSUPP || error == ENOSYS;
}

std::string aclText(const XmpAcl& acl) {
  ssize_t length = 0;
  std::unique_ptr<char, XmpAclDeleter> text(acl_to_text(acl.get(), &length));
  if (!text || length < 0)
    throw Error(ErrorCode::kerImageWriteFailed);
  return {text.get(), static_cast<size_t>(length)};
}
#endif

// Snapshot through one descriptor before creating a replacement inode.
struct FilePermissions {
  struct stat status{};
  bool exists = false;
#ifdef EXV_HAVE_POSIX_ACL
  XmpAcl acl;
#endif
  FilePermissions(const fs::path& target, const XmpFileOps& ops) {
    Descriptor original(::open(target.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
    if (original.fd < 0) {
      if (errno == ENOENT)
        return;
      throw Error(ErrorCode::kerImageWriteFailed);
    }
    exists = true;
    if (fstat(original.fd, &status) != 0 || !S_ISREG(status.st_mode) || ::access(target.c_str(), W_OK) != 0)
      throw Error(ErrorCode::kerImageWriteFailed);
#ifdef EXV_HAVE_POSIX_ACL
    acl = ops.readAcl(original.fd);
#else
    (void)ops;
#endif
  }
  bool apply(int fd, const XmpFileOps& ops) const {
    if (!exists)
      return false;
    bool copiedAcl = false;
    struct stat temp{};
    if (fstat(fd, &temp) != 0)
      throw Error(ErrorCode::kerImageWriteFailed);
    if (temp.st_uid != status.st_uid || temp.st_gid != status.st_gid)
      ops.setOwner(fd, status.st_uid, status.st_gid);
#ifdef EXV_HAVE_POSIX_ACL
    // Setting the complete ACL also sets its corresponding mode bits atomically.
    // Never expose the mask as ordinary group access before installing the ACL.
    if (acl)
      copiedAcl = ops.writeAcl(fd, acl);
#endif
    ops.setMode(fd, status.st_mode & 07777);
    verify(fd, ops, copiedAcl);
    return copiedAcl;
  }
  void verify(int fd, const XmpFileOps& ops, [[maybe_unused]] bool copiedAcl) const {
    if (!exists)
      return;
    struct stat temp{};
    if (fstat(fd, &temp) != 0 || temp.st_uid != status.st_uid || temp.st_gid != status.st_gid ||
        (temp.st_mode & 07777) != (status.st_mode & 07777))
      throw Error(ErrorCode::kerImageWriteFailed);
#ifdef EXV_HAVE_POSIX_ACL
    if (copiedAcl) {
      const auto actual = ops.readAcl(fd);
      if (!actual || aclText(actual) != aclText(acl))
        throw Error(ErrorCode::kerImageWriteFailed);
    }
#else
    (void)ops;
#endif
  }
};
#endif
}  // namespace

int XmpFileOps::create(const fs::path& path, [[maybe_unused]] bool replacement) const {
#ifdef _WIN32
  return _wopen(path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
  // The returned descriptor remains writable even with mode 0000. Never give
  // another user a chance to retain a descriptor before permissions are copied.
  return ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, replacement ? 0000 : 0666);
#endif
}

#ifndef _WIN32
#ifdef EXV_HAVE_POSIX_ACL
XmpAcl XmpFileOps::readAcl(int fd) const {
#if defined(__FreeBSD__) || defined(__NetBSD__)
#ifdef _PC_ACL_NFS4
  errno = 0;
  const auto nfs4 = fpathconf(fd, _PC_ACL_NFS4);
  if (nfs4 > 0)
    return {};  // This writer supports POSIX access ACLs, not NFSv4 ACLs.
  if (nfs4 < 0 && errno != 0 && errno != EINVAL && !unsupportedAcl(errno))
    throw Error(ErrorCode::kerImageWriteFailed);
#endif
#endif
  XmpAcl acl(acl_get_fd(fd));
  if (!acl && !unsupportedAcl(errno))
    throw Error(ErrorCode::kerImageWriteFailed);
  return acl;
}

bool XmpFileOps::writeAcl(int fd, const XmpAcl& acl) const {
  if (acl_set_fd(fd, acl.get()) == 0)
    return true;
  if (unsupportedAcl(errno))
    return false;
  throw Error(ErrorCode::kerImageWriteFailed);
}
#endif

void XmpFileOps::setOwner(int fd, uid_t owner, gid_t group) const {
  if (fchown(fd, owner, group) != 0)
    throw Error(ErrorCode::kerImageWriteFailed);
}

void XmpFileOps::setMode(int fd, mode_t mode) const {
  if (fchmod(fd, mode) != 0)
    throw Error(ErrorCode::kerImageWriteFailed);
}
#endif

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
#ifndef _WIN32
    const FilePermissions permissions(target, ops);
    const bool replacement = permissions.exists;
#else
    const bool replacement = fs::exists(target);
#endif
    TemporaryFile temp;
    createTemporary(temp, target, replacement, ops);
#ifdef _WIN32
    preservePermissions(target, temp.path);
#else
    const bool copiedAcl = permissions.apply(fileno(temp.file), ops);
#endif
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
#ifndef _WIN32
    // Writes can clear special mode bits. Verify after flushing, before commit.
    if (std::fflush(temp.file) != 0)
      throw Error(ErrorCode::kerImageWriteFailed);
    permissions.verify(fileno(temp.file), ops, copiedAcl);
#endif
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
namespace {
// Present an empty image until the first successful filesystem commit.
class DeferredXmpIo final : public BasicIo {
 public:
  explicit DeferredXmpIo(std::string path) : path_(std::move(path)), current_(std::make_unique<MemIo>()) {
  }
  int open() override {
    return current_->open();
  }
  int close() override {
    return current_->close();
  }
  size_t write(const byte* data, size_t size) override {
    return current_->write(data, size);
  }
  size_t write(BasicIo& source) override {
    return current_->write(source);
  }
  int putb(byte value) override {
    return current_->putb(value);
  }
  DataBuf read(size_t size) override {
    return current_->read(size);
  }
  size_t read(byte* data, size_t size) override {
    return current_->read(data, size);
  }
  int getb() override {
    return current_->getb();
  }
  int seek(int64_t offset, Position position) override {
    return current_->seek(offset, position);
  }
  byte* mmap(bool writable) override {
    return current_->mmap(writable);
  }
  int munmap() override {
    return current_->munmap();
  }
  size_t tell() const override {
    return current_->tell();
  }
  size_t size() const override {
    return current_->size();
  }
  bool isopen() const override {
    return current_->isopen();
  }
  int error() const override {
    return current_->error();
  }
  bool eof() const override {
    return current_->eof();
  }
  const std::string& path() const noexcept override {
    return path_;
  }
  void populateFakeData() override {
    current_->populateFakeData();
  }
  void transfer(BasicIo& source) override {
    // Allocate before commit: nothing after publication is allowed to fail.
    auto file = std::make_unique<FileIo>(path_);
    replaceXmpFile(path_, source);
    current_ = std::move(file);
    committed_ = true;
  }
  bool committed() const {
    return committed_;
  }

 private:
  std::string path_;
  BasicIo::UniquePtr current_;
  bool committed_{};
};
}  // namespace

BasicIo::UniquePtr deferredXmpIo(const std::string& path) {
  return std::make_unique<DeferredXmpIo>(path);
}

bool isUncommittedXmpIo(const BasicIo& io) {
  const auto* deferred = dynamic_cast<const DeferredXmpIo*>(&io);
  return deferred && !deferred->committed();
}

bool isDeferredXmpIo(const BasicIo& io) {
  return dynamic_cast<const DeferredXmpIo*>(&io) != nullptr;
}
}  // namespace Exiv2::Internal
#endif
