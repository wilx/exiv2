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
  static void raw(Image& image, const std::string& value) {
    image.xmpPacket() =
        "<?xml version=\"1.0\"?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
        "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
        "<rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" xmp:Label=\"" +
        value + "\"/></rdf:RDF></x:xmpmeta>";
    image.writeXmpFromPacket(true);
  }
};

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

}  // namespace
#endif
