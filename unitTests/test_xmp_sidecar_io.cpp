// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <exiv2/exiv2.hpp>
#include <fstream>
#include <iterator>
#include <random>
#include "xmp_sidecar_io_int.hpp"

#ifdef EXV_ENABLE_FILESYSTEM
#ifdef _WIN32
#include <windows.h>
#endif
namespace {
using namespace Exiv2;
namespace fs = std::filesystem;

// Each test owns its directory; parallel test processes never share fixtures.
class XmpFileWrite : public testing::Test {
 protected:
  fs::path directory;
  fs::path target;
  void SetUp() override {
    std::random_device random;
    do {
      directory = fs::temp_directory_path() / ("exiv2-xmp-test-" + std::to_string(random()));
    } while (!fs::create_directory(directory));
    target = directory / "sidecar.xmp";
  }
  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(directory, ignored);
  }
  static std::string contents(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  }
  void seed() {
    std::ofstream(target, std::ios::binary) << "original-canary";
  }
  size_t files() {
    return static_cast<size_t>(std::distance(fs::directory_iterator(directory), fs::directory_iterator()));
  }
  Image::UniquePtr deferred() {
    return ImageFactory::create(ImageType::xmp, target.string(), CreateMode::deferred);
  }
  static void raw(Image& image, const std::string& value) {
    image.xmpPacket() =
        "<?xml version=\"1.0\"?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
        "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
        "<rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" xmp:Label=\"" +
        value + "\"/></rdf:RDF></x:xmpmeta>";
    image.writeXmpFromPacket(true);
  }
};

// Legacy creation remains destructive; unsupported deferred requests are not.
TEST_F(XmpFileWrite, PreservesImmediateFactoryContract) {
  seed();
  EXPECT_THROW(ImageFactory::create(ImageType::jpeg, target.string(), CreateMode::deferred), Error);
  EXPECT_EQ("original-canary", contents(target));
  ImageFactory::create(ImageType::xmp, target.string());
  EXPECT_EQ(54U, fs::file_size(target));
  seed();
  ImageFactory::create(ImageType::xmp, target.string(), CreateMode::immediate);
  EXPECT_EQ(54U, fs::file_size(target));
}

// Creating, inspecting and abandoning a staged object never touches its path.
TEST_F(XmpFileWrite, DefersCreationAndSupportsReadMetadata) {
  for (const bool existing : {false, true}) {
    if (existing)
      seed();
    {
      auto image = deferred();
      EXPECT_TRUE(image->good());
      EXPECT_EQ(target.string(), image->io().path());
      EXPECT_NO_THROW(image->readMetadata());
      EXPECT_TRUE(image->xmpData().empty());
    }
    EXPECT_EQ(existing, fs::exists(target));
    if (existing) {
      EXPECT_EQ("original-canary", contents(target));
    }
  }
}

// Raw writes work with or without the XMP toolkit, and support repeated saves.
TEST_F(XmpFileWrite, CommitsRawPacketsAndEmptySidecars) {
  auto empty = deferred();
  empty->writeMetadata();
  EXPECT_EQ(54U, fs::file_size(target));
  auto image = deferred();
  for (const auto* label : {"first", "second"}) {
    raw(*image, label);
    const auto expected = image->xmpPacket();
    image->writeMetadata();
    EXPECT_EQ(expected, contents(target));
    EXPECT_TRUE(image->good());
    EXPECT_EQ(1U, files());
  }
}

#ifdef EXV_HAVE_XMP_TOOLKIT
// The issue's three native values fail as text without replacing unrelated XMP.
TEST_F(XmpFileWrite, RejectsInvalidExifWithoutPublishingAndAllowsRetry) {
  for (const auto* key : {"Exif.Photo.UserComment", "Exif.Image.Model", "Exif.Image.Copyright"}) {
    for (const bool existing : {false, true}) {
      fs::remove(target);
      if (existing)
        seed();
      auto image = deferred();
      image->readMetadata();
      ExifData exif;
      if (std::string(key) == "Exif.Photo.UserComment") {
        const byte data[]{10, 0, 0, 0, 74, 75, 74, 75, 39, 3, 171};
        CommentValue value;
        value.read(data, sizeof(data), littleEndian);
        exif.add(ExifKey(key), &value);
      } else {
        exif[key] = std::string(key) == "Exif.Image.Model" ? "\xe5\x80\xf0" : "\xa9 Example";
      }
      image->setExifData(exif);
      image->xmpData()["Xmp.xmp.Label"] = "keep";
      EXPECT_THROW(image->writeMetadata(), Error);
      EXPECT_EQ(existing, fs::exists(target));
      if (existing) {
        EXPECT_EQ("original-canary", contents(target));
      }
      EXPECT_EQ(existing ? 1U : 0U, files());
      image->clearExifData();
      image->clearXmpData();
      image->xmpData()["Xmp.xmp.Label"] = "repaired";
      ASSERT_NO_THROW(image->writeMetadata());
      image->readMetadata();
      EXPECT_EQ("repaired", image->xmpData()["Xmp.xmp.Label"].toString());
    }
  }
}

// Structured errors must not be hidden by an old packet, even after prior success.
TEST_F(XmpFileWrite, PreservesOpenedSidecarOnEncodingFailure) {
  auto created = deferred();
  raw(*created, "original");
  created->writeMetadata();
  const auto before = contents(target);
  auto image = ImageFactory::open(target.string());
  image->readMetadata();
  image->xmpData()["Xmp.xmpMM.DerivedFrom/stRef:documentID"] = "child";
  image->xmpData()["Xmp.xmpMM.DerivedFrom"] = "conflict";
  EXPECT_THROW(image->writeMetadata(), Error);
  EXPECT_EQ(before, contents(target));
  EXPECT_EQ(before, image->xmpPacket());
}
#endif

#ifndef EXV_HAVE_XMP_TOOLKIT
// A deferred save must not claim success when structured output is unavailable.
TEST_F(XmpFileWrite, RequiresToolkitForStructuredDeferredWrites) {
  seed();
  auto image = deferred();
  image->xmpData()["Xmp.xmp.Label"] = "cannot encode";
  EXPECT_THROW(image->writeMetadata(), Error);
  EXPECT_EQ("original-canary", contents(target));
  image->clearXmpData();
  raw(*image, "raw still works");
  image->writeMetadata();
  const auto before = contents(target);
  image->xmpData()["Xmp.xmp.Label"] = "still cannot encode";
  EXPECT_THROW(image->writeMetadata(), Error);
  EXPECT_EQ(before, contents(target));
}
#endif

struct FailureOps : Internal::XmpFileOps {
  enum Stage { writeFailure, closeFailure, replaceFailure } stage;
  explicit FailureOps(Stage value) : stage(value) {
  }
  size_t write(FILE* file, const byte* data, size_t size) const override {
    return XmpFileOps::write(file, data, stage == writeFailure ? size / 2 : size);
  }
  int close(FILE* file) const override {
    const auto result = XmpFileOps::close(file);
    return stage == closeFailure ? EOF : result;
  }
  void replace(const fs::path& source, const fs::path& destination) const override {
    if (stage == replaceFailure)
      throw Error(ErrorCode::kerImageWriteFailed);
    XmpFileOps::replace(source, destination);
  }
};

// Source failures must be detected before any destination mutation.
TEST_F(XmpFileWrite, RejectsUnreadableAndIncompleteSources) {
  struct Source : MemIo {
    using MemIo::read;
    bool failOpen = false;
    int open() override {
      return failOpen ? 1 : MemIo::open();
    }
    size_t read(byte*, size_t) override {
      return 0;
    }
    size_t size() const override {
      return 10;
    }
  } source;
  seed();
  for (const bool failOpen : {false, true}) {
    source.failOpen = failOpen;
    EXPECT_THROW(Internal::replaceXmpFile(target.string(), source), Error);
    EXPECT_EQ("original-canary", contents(target));
    EXPECT_EQ(1U, files());
  }
}

// The public path is UTF-8 on Windows as well as on Unix.
TEST_F(XmpFileWrite, WritesUnicodePaths) {
  target = directory / u8"Žluť-側車.xmp";
  const auto utf8 = target.u8string();
  auto image = ImageFactory::create(ImageType::xmp, std::string(utf8.begin(), utf8.end()), CreateMode::deferred);
  raw(*image, "unicode-path");
  const auto expected = image->xmpPacket();
  image->writeMetadata();
  EXPECT_EQ(expected, contents(target));
  image->writeMetadata();
  EXPECT_EQ(expected, contents(target));
}

// All pre-commit failures preserve the old file and clean only our temporary file.
TEST_F(XmpFileWrite, PreservesDestinationOnWriteCloseAndReplacementFailures) {
  for (const bool existing : {false, true}) {
    if (existing)
      seed();
    for (const auto stage : {FailureOps::writeFailure, FailureOps::closeFailure, FailureOps::replaceFailure}) {
      const std::string replacement("new data");
      MemIo source(reinterpret_cast<const byte*>(replacement.data()), replacement.size());
      EXPECT_THROW(Internal::replaceXmpFile(target.string(), source, FailureOps(stage)), Error);
      EXPECT_EQ(existing, fs::exists(target));
      if (existing) {
        EXPECT_EQ("original-canary", contents(target));
      }
      EXPECT_EQ(existing ? 1U : 0U, files());
    }
  }
}

// FileIo subclasses retain their transfer override, and failed wrapping is local.
TEST_F(XmpFileWrite, RetainsCustomTransferAndPacketOnFailure) {
  struct CustomIo : FileIo {
    using FileIo::FileIo;
    bool transferred = false;
    void transfer(BasicIo&) override {
      transferred = true;
      throw Error(ErrorCode::kerImageWriteFailed);
    }
  };
  ImageFactory::create(ImageType::xmp, target.string());
  auto io = std::make_unique<CustomIo>(target.string());
  auto* custom = io.get();
  auto image = ImageFactory::open(std::move(io));
  ASSERT_NE(nullptr, image);
  const auto before = contents(target);
  raw(*image, "custom");
  image->xmpPacket().erase(0, std::string("<?xml version=\"1.0\"?>").size());
  const auto packet = image->xmpPacket();
  EXPECT_THROW(image->writeMetadata(), Error);
  EXPECT_TRUE(custom->transferred);
  EXPECT_EQ(before, contents(target));
  EXPECT_EQ(packet, static_cast<const Image&>(*image).xmpPacket());
}

// Destination errors are delayed until writing and remain retryable.
TEST_F(XmpFileWrite, ReportsMissingParentAndKeepsDirectoryDestination) {
  target = directory / "missing" / "sidecar.xmp";
  auto image = deferred();
  raw(*image, "valid");
  EXPECT_THROW(image->writeMetadata(), Error);
  EXPECT_EQ(0U, files());
  fs::create_directory(target.parent_path());
  ASSERT_NO_THROW(image->writeMetadata());
  target = directory / "directory.xmp";
  fs::create_directory(target);
  auto invalid = deferred();
  raw(*invalid, "valid");
  EXPECT_THROW(invalid->writeMetadata(), Error);
  EXPECT_TRUE(fs::is_directory(target));
}

// Replacement retains permissions and follows symlinks without deleting the link.
#ifndef _WIN32
TEST_F(XmpFileWrite, PreservesPermissionsAndSymlinkAndDocumentsHardlinks) {
  seed();
  fs::permissions(target, fs::perms::owner_read | fs::perms::owner_write);
  const auto alias = directory / "hardlink.xmp";
  fs::create_hard_link(target, alias);
  const auto link = directory / "symlink.xmp";
  fs::create_symlink(target.filename(), link);
  auto image = ImageFactory::create(ImageType::xmp, link.string(), CreateMode::deferred);
  raw(*image, "new");
  image->writeMetadata();
  EXPECT_TRUE(fs::is_symlink(link));
  EXPECT_EQ(contents(target), contents(link));
  EXPECT_EQ("original-canary", contents(alias));
  EXPECT_EQ(fs::perms::owner_read | fs::perms::owner_write, fs::status(target).permissions());
}
#endif

#ifdef _WIN32
// Windows must preserve the old file when sharing or read-only attributes deny replacement.
TEST_F(XmpFileWrite, PreservesReadOnlyAndLockedDestinations) {
  seed();
  auto image = deferred();
  raw(*image, "new");
  ASSERT_TRUE(SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_READONLY));
  EXPECT_THROW(image->writeMetadata(), Error);
  EXPECT_EQ("original-canary", contents(target));
  EXPECT_EQ(1U, files());
  ASSERT_TRUE(SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL));
  const auto handle = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(INVALID_HANDLE_VALUE, handle);
  EXPECT_THROW(image->writeMetadata(), Error);
  EXPECT_EQ("original-canary", contents(target));
  EXPECT_EQ(1U, files());
  ASSERT_TRUE(CloseHandle(handle));
  EXPECT_NO_THROW(image->writeMetadata());
  EXPECT_EQ(image->xmpPacket(), contents(target));
}
#endif
}  // namespace
#endif
