// SPDX-License-Identifier: GPL-2.0-or-later
#include "xmp_sidecar_io_int.hpp"

#ifdef EXV_ENABLE_FILESYSTEM
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <random>
#include <vector>
#include "error.hpp"

#ifdef _WIN32
#include <aclapi.h>
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
    if (!path.empty()) {
#ifdef _WIN32
      // Remove only the temporary file, never an unexpected directory.
      DeleteFileW(path.c_str());
#else
      std::error_code ignored;
      fs::remove(path, ignored);
#endif
    }
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
struct WindowsHandleDeleter {
  void operator()(void* handle) const {
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};
using WindowsHandle = std::unique_ptr<void, WindowsHandleDeleter>;

struct WindowsSecurityDeleter {
  void operator()(void* descriptor) const {
    LocalFree(descriptor);
  }
};

std::vector<byte> windowsSecurity(HANDLE handle) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const auto result = GetSecurityInfo(
      handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      nullptr, nullptr, nullptr, nullptr, &descriptor);
  std::unique_ptr<void, WindowsSecurityDeleter> owner(descriptor);
  if (result != ERROR_SUCCESS) {
    throw Error(ErrorCode::kerImageWriteFailed);
  }
  const auto* bytes = static_cast<const byte*>(descriptor);
  return {bytes, bytes + GetSecurityDescriptorLength(descriptor)};
}

bool sameSid(PSID left, PSID right) {
  return left && right ? EqualSid(left, right) != FALSE : left == right;
}

struct WindowsSecurity {
  PSID owner = nullptr;
  PSID group = nullptr;
  PACL acl = nullptr;
  SECURITY_DESCRIPTOR_CONTROL control{};

  explicit WindowsSecurity(const std::vector<byte>& descriptor) {
    auto* data = const_cast<byte*>(descriptor.data());
    BOOL defaulted = FALSE, present = FALSE;
    DWORD revision = 0;
    if (!GetSecurityDescriptorOwner(data, &owner, &defaulted) ||
        !GetSecurityDescriptorGroup(data, &group, &defaulted) ||
        !GetSecurityDescriptorDacl(data, &present, &acl, &defaulted) ||
        !GetSecurityDescriptorControl(data, &control, &revision)) {
      throw Error(ErrorCode::kerImageWriteFailed);
    }
  }

  bool matches(const WindowsSecurity& other) const {
    constexpr auto inheritance =
        SE_DACL_PRESENT | SE_DACL_PROTECTED | SE_DACL_AUTO_INHERITED | SE_DACL_AUTO_INHERIT_REQ;
    if (!sameSid(owner, other.owner) || !sameSid(group, other.group) ||
        (control & inheritance) != (other.control & inheritance)) {
      return false;
    }
    if (!acl || !other.acl) {
      return acl == other.acl;
    }
    if (acl->AclRevision != other.acl->AclRevision || acl->AceCount != other.acl->AceCount) {
      return false;
    }
    // Compare complete ACEs in order, without depending on unused ACL padding.
    for (DWORD index = 0; index != acl->AceCount; ++index) {
      void* left = nullptr;
      void* right = nullptr;
      if (!GetAce(acl, index, &left) || !GetAce(other.acl, index, &right)) {
        throw Error(ErrorCode::kerImageWriteFailed);
      }
      const auto size = static_cast<ACE_HEADER*>(left)->AceSize;
      if (size != static_cast<ACE_HEADER*>(right)->AceSize || std::memcmp(left, right, size) != 0) {
        return false;
      }
    }
    return true;
  }
};

// Snapshot before creating a replacement, then restore only after writing/flushing.
struct FilePermissions {
  bool exists = false;
  std::vector<byte> descriptor;

  explicit FilePermissions(const fs::path& target) {
    if (!fs::exists(target)) {
      return;
    }
    const auto attributes = GetFileAttributesW(target.c_str());
    if (!fs::is_regular_file(target) || attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_READONLY)) {
      throw Error(ErrorCode::kerImageWriteFailed);
    }
    // Check effective write access without truncating the destination. A copied
    // DACL does not revoke write access on the temporary's already-open handle.
    WindowsHandle original(CreateFileW(target.c_str(), GENERIC_WRITE | READ_CONTROL,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL, nullptr));
    if (original.get() == INVALID_HANDLE_VALUE) {
      throw Error(ErrorCode::kerImageWriteFailed);
    }
    descriptor = windowsSecurity(original.get());
    exists = true;
  }

  void apply(int fd, const fs::path& path, const XmpFileOps& ops) const {
    if (!exists) {
      return;
    }
    ops.setSecurity(fd, path, descriptor);
    const auto actual = windowsSecurity(reinterpret_cast<HANDLE>(_get_osfhandle(fd)));
    if (!WindowsSecurity(descriptor).matches(WindowsSecurity(actual))) {
      throw Error(ErrorCode::kerImageWriteFailed);
    }
  }
};
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
  if (!replacement) {
    return _wopen(path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
  }
  // Grant access only to the effective creator. In particular, retain DELETE
  // for cleanup even when the parent permits creation but not child deletion.
  HANDLE tokenHandle = nullptr;
  if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &tokenHandle)) {
    if (GetLastError() != ERROR_NO_TOKEN || !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenHandle)) {
      throw Error(ErrorCode::kerImageWriteFailed);
    }
  }
  WindowsHandle token(tokenHandle);
  DWORD size = 0;
  GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
  if (!size) {
    throw Error(ErrorCode::kerImageWriteFailed);
  }
  std::vector<byte> user(size);
  if (!GetTokenInformation(token.get(), TokenUser, user.data(), size, &size)) {
    throw Error(ErrorCode::kerImageWriteFailed);
  }
  const auto sid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
  size = static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)) + GetLengthSid(sid);
  std::vector<byte> aclBytes(size);
  auto* acl = reinterpret_cast<ACL*>(aclBytes.data());
  SECURITY_DESCRIPTOR descriptor{};
  // Install a protected DACL in CreateFileW itself: changing it afterward leaves
  // a window in which an unauthorized reader can retain a handle.
  if (!InitializeAcl(acl, size, ACL_REVISION) || !AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, sid) ||
      !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
      !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
      !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
    throw Error(ErrorCode::kerImageWriteFailed);
  }
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), &descriptor, FALSE};
  WindowsHandle handle(CreateFileW(path.c_str(), GENERIC_WRITE | READ_CONTROL | WRITE_DAC | WRITE_OWNER,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                   nullptr));
  if (handle.get() == INVALID_HANDLE_VALUE) {
    const auto error = GetLastError();
    if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
      errno = EEXIST;
      return -1;
    }
    throw Error(ErrorCode::kerImageWriteFailed);
  }
  const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle.get()), _O_WRONLY | _O_BINARY | _O_NOINHERIT);
  if (fd < 0) {
    handle.reset();
    DeleteFileW(path.c_str());
    throw Error(ErrorCode::kerImageWriteFailed);
  }
  handle.release();  // The CRT descriptor now owns the Windows handle.
  return fd;
#else
  // The returned descriptor remains writable even with mode 0000. Never give
  // another user a chance to retain a descriptor before permissions are copied.
  return ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, replacement ? 0000 : 0666);
#endif
}

#ifdef _WIN32
void XmpFileOps::setSecurity(int fd, const fs::path& path, const std::vector<byte>& descriptor) const {
  const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  const WindowsSecurity expected(descriptor);
  const auto currentDescriptor = windowsSecurity(handle);
  const WindowsSecurity current(currentDescriptor);
  DWORD information =
      DACL_SECURITY_INFORMATION | ((expected.control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION
                                                                          : UNPROTECTED_DACL_SECURITY_INFORMATION);
  if (!sameSid(expected.owner, current.owner)) {
    information |= OWNER_SECURITY_INFORMATION;
  }
  if (!sameSid(expected.group, current.group)) {
    information |= GROUP_SECURITY_INFORMATION;
  }
  if (expected.control & SE_DACL_AUTO_INHERITED) {
    // Use the inheritance-aware API so inherited ACEs and the AI flag survive.
    if (SetSecurityInfo(handle, SE_FILE_OBJECT, information, expected.owner, expected.group, expected.acl, nullptr) !=
        ERROR_SUCCESS) {
      throw Error(ErrorCode::kerImageWriteFailed);
    }
  } else {
    // Preserve the legacy inheritance model when the original AI flag is clear.
    if (!SetFileSecurityW(path.c_str(), information, const_cast<byte*>(descriptor.data()))) {
      throw Error(ErrorCode::kerImageWriteFailed);
    }
  }
}
#else
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
    const FilePermissions permissions(target);
    const bool replacement = permissions.exists;
#endif
    TemporaryFile temp;
    createTemporary(temp, target, replacement, ops);
#ifndef _WIN32
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
    // Flush the complete packet before restoring security or verifying mode bits.
    if (std::fflush(temp.file) != 0)
      throw Error(ErrorCode::kerImageWriteFailed);
#ifdef _WIN32
    permissions.apply(_fileno(temp.file), temp.path, ops);
#else
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
