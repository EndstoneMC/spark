#include "sampler/symbol_guess_dwarf.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      std::cerr << __FILE__ << ':' << __LINE__                                \
                << ": CHECK failed: " #expr << '\n';                         \
      ++failures;                                                              \
    }                                                                          \
  } while (false)

class SyntheticImage : public spark::symbol_guess::dwarf::ImageView {
public:
  explicit SyntheticImage(std::size_t size) : bytes_(size) {}

  bool read(std::uint64_t rva, void *out,
            std::size_t length) const override {
    if (out == nullptr || rva > bytes_.size() || length > bytes_.size() - rva) {
      return false;
    }
    std::memcpy(out, bytes_.data() + rva, length);
    return true;
  }

  bool executable(std::uint64_t rva,
                  std::size_t length) const override {
    return rva >= executable_begin_ && rva < executable_end_ &&
           length <= executable_end_ - rva;
  }

  std::uint64_t readableEnd(std::uint64_t rva) const override {
    return rva < bytes_.size() ? bytes_.size() : 0;
  }

  template <typename T> void put(std::uint64_t rva, T value) {
    CHECK(rva <= bytes_.size() && sizeof(value) <= bytes_.size() - rva);
    if (rva <= bytes_.size() && sizeof(value) <= bytes_.size() - rva) {
      std::memcpy(bytes_.data() + rva, &value, sizeof(value));
    }
  }

  void putByte(std::uint64_t rva, std::uint8_t value) { put(rva, value); }

  void executableRange(std::uint64_t begin, std::uint64_t end) {
    executable_begin_ = begin;
    executable_end_ = end;
  }

private:
  std::vector<std::uint8_t> bytes_;
  std::uint64_t executable_begin_ = 0;
  std::uint64_t executable_end_ = 0;
};

constexpr std::uint64_t kHeader = 0x100;
constexpr std::uint64_t kCie = 0x200;

void putCie(SyntheticImage &image) {
  // CIE v1, augmentation "zR", code alignment 1, data alignment -8,
  // return register 16, FDE pointers encoded as pcrel|sdata4.
  image.put<std::uint32_t>(kCie, 13);
  image.put<std::uint32_t>(kCie + 4, 0);
  image.putByte(kCie + 8, 1);
  image.putByte(kCie + 9, 'z');
  image.putByte(kCie + 10, 'R');
  image.putByte(kCie + 11, 0);
  image.putByte(kCie + 12, 1);
  image.putByte(kCie + 13, 0x78);
  image.putByte(kCie + 14, 16);
  image.putByte(kCie + 15, 1);
  image.putByte(kCie + 16, 0x1b);
}

void putFde(SyntheticImage &image, std::uint64_t fde,
            std::uint64_t initial, std::uint32_t length) {
  image.put<std::uint32_t>(fde, 13);
  image.put<std::uint32_t>(fde + 4,
                           static_cast<std::uint32_t>(fde + 4 - kCie));
  image.put<std::int32_t>(fde + 8,
                          static_cast<std::int32_t>(initial - (fde + 8)));
  image.put<std::uint32_t>(fde + 12, length);
  image.putByte(fde + 16, 0);
}

void putHeader(SyntheticImage &image,
               std::span<const std::pair<std::uint64_t, std::uint64_t>> rows) {
  image.putByte(kHeader, 1);
  image.putByte(kHeader + 1, 0x1b);
  image.putByte(kHeader + 2, 0x03);
  image.putByte(kHeader + 3, 0x3b);
  image.put<std::int32_t>(kHeader + 4,
                          static_cast<std::int32_t>(kCie - (kHeader + 4)));
  image.put<std::uint32_t>(kHeader + 8,
                           static_cast<std::uint32_t>(rows.size()));
  std::uint64_t cursor = kHeader + 12;
  for (const auto &[initial, fde] : rows) {
    image.put<std::int32_t>(cursor,
                            static_cast<std::int32_t>(initial - kHeader));
    image.put<std::int32_t>(cursor + 4,
                            static_cast<std::int32_t>(fde - kHeader));
    cursor += 8;
  }
}

SyntheticImage baseImage() {
  SyntheticImage image(0x1000);
  image.executableRange(0x400, 0x800);
  putCie(image);
  return image;
}

void testExactRangesAndGap() {
  SyntheticImage image = baseImage();
  putFde(image, 0x240, 0x400, 0x20);
  putFde(image, 0x260, 0x440, 0x20);
  const std::pair<std::uint64_t, std::uint64_t> rows[] = {
      {0x400, 0x240}, {0x440, 0x260}};
  putHeader(image, rows);

  spark::symbol_guess::dwarf::ParseStats stats{};
  const auto ranges = spark::symbol_guess::dwarf::parseEhFrameHeader(
      image, kHeader, 0x80, {}, &stats);
  CHECK(ranges ==
        (std::vector<spark::symbol_guess::dwarf::FunctionRange>{
            {0x400, 0x420, 0x400}, {0x440, 0x460, 0x440}}));
  CHECK(stats.table_entries == 2);
  CHECK(stats.function_ranges == 2);
  CHECK(stats.rejected_entries == 0);
  CHECK(stats.gap_ranges == 1);
  CHECK(stats.gap_bytes == 0x20);
  CHECK(spark::symbol_guess::dwarf::functionContaining(ranges, 0x400) ==
        &ranges[0]);
  CHECK(spark::symbol_guess::dwarf::functionContaining(ranges, 0x41f) ==
        &ranges[0]);
  CHECK(spark::symbol_guess::dwarf::functionContaining(ranges, 0x420) ==
        nullptr);
  CHECK(spark::symbol_guess::dwarf::functionContaining(ranges, 0x43f) ==
        nullptr);
  CHECK(spark::symbol_guess::dwarf::functionContaining(ranges, 0x440) ==
        &ranges[1]);
  CHECK(spark::symbol_guess::dwarf::functionContaining(ranges, 0x460) ==
        nullptr);
}

void testDuplicateAndMismatch() {
  SyntheticImage image = baseImage();
  putFde(image, 0x240, 0x400, 0x20);
  const std::pair<std::uint64_t, std::uint64_t> rows[] = {
      {0x400, 0x240}, {0x400, 0x240}, {0x440, 0x240}};
  putHeader(image, rows);

  spark::symbol_guess::dwarf::ParseStats stats{};
  const auto ranges = spark::symbol_guess::dwarf::parseEhFrameHeader(
      image, kHeader, 0x80, {}, &stats);
  CHECK(ranges.size() == 1);
  CHECK(ranges[0] ==
        (spark::symbol_guess::dwarf::FunctionRange{0x400, 0x420, 0x400}));
  CHECK(stats.duplicate_ranges == 1);
  CHECK(stats.rejected_entries == 1);
}

void testOverlapIsRejected() {
  SyntheticImage image = baseImage();
  putFde(image, 0x240, 0x400, 0x50);
  putFde(image, 0x260, 0x440, 0x20);
  const std::pair<std::uint64_t, std::uint64_t> rows[] = {
      {0x400, 0x240}, {0x440, 0x260}};
  putHeader(image, rows);

  spark::symbol_guess::dwarf::ParseStats stats{};
  const auto ranges = spark::symbol_guess::dwarf::parseEhFrameHeader(
      image, kHeader, 0x80, {}, &stats);
  CHECK(ranges.empty());
  CHECK(stats.overlap_ranges == 2);
  CHECK(stats.rejected_entries == 2);
}

void testMalformedAndBounds() {
  {
    SyntheticImage image = baseImage();
    putFde(image, 0x240, 0x400, 0x20);
    putFde(image, 0x260, 0x440, 0x20);
    const std::pair<std::uint64_t, std::uint64_t> rows[] = {
        {0x440, 0x260}, {0x400, 0x240}};
    putHeader(image, rows);
    spark::symbol_guess::dwarf::ParseStats stats{};
    CHECK(spark::symbol_guess::dwarf::parseEhFrameHeader(
              image, kHeader, 0x40, {}, &stats)
              .empty());
    CHECK(stats.rejected_entries == 2);
  }
  {
    SyntheticImage image = baseImage();
    putFde(image, 0x240, 0x400, 0);
    const std::pair<std::uint64_t, std::uint64_t> rows[] = {{0x400, 0x240}};
    putHeader(image, rows);
    spark::symbol_guess::dwarf::ParseStats stats{};
    CHECK(spark::symbol_guess::dwarf::parseEhFrameHeader(
              image, kHeader, 0x40, {}, &stats)
              .empty());
    CHECK(stats.rejected_entries == 1);
  }
  {
    SyntheticImage image = baseImage();
    putFde(image, 0x240, 0x7f0, 0x20);
    const std::pair<std::uint64_t, std::uint64_t> rows[] = {{0x7f0, 0x240}};
    putHeader(image, rows);
    CHECK(spark::symbol_guess::dwarf::parseEhFrameHeader(image, kHeader, 0x40)
              .empty());
  }
  {
    SyntheticImage image = baseImage();
    image.putByte(kHeader, 2);
    CHECK(spark::symbol_guess::dwarf::parseEhFrameHeader(image, kHeader, 0x40)
              .empty());
    CHECK(spark::symbol_guess::dwarf::parseEhFrameHeader(
              image, (std::numeric_limits<std::uint64_t>::max)() - 1, 8)
              .empty());
  }
}

void testUnindexedFdeRecoveryRequiresTerminator() {
  {
    SyntheticImage image = baseImage();
    // CIE ends at 0x211. A valid contiguous FDE is absent from the header,
    // while the indexed FDE for the same start has a zero range.
    putFde(image, 0x211, 0x400, 0x20);
    putFde(image, 0x260, 0x400, 0);
    const std::pair<std::uint64_t, std::uint64_t> rows[] = {{0x400, 0x260}};
    putHeader(image, rows);

    spark::symbol_guess::dwarf::ParseStats stats{};
    const auto ranges = spark::symbol_guess::dwarf::parseEhFrameHeader(
        image, kHeader, 0x40, {}, &stats);
    CHECK(ranges ==
          (std::vector<spark::symbol_guess::dwarf::FunctionRange>{
              {0x400, 0x420, 0x400}}));
    CHECK(stats.table_entries == 1);
    CHECK(stats.rejected_entries == 1);
    CHECK(stats.eh_frame_records == 2);
    CHECK(stats.unindexed_ranges == 1);
  }
  {
    SyntheticImage image = baseImage();
    putFde(image, 0x211, 0x400, 0x20);
    // Replace the required zero terminator with a malformed record. The
    // incomplete sequential walk must not contribute its tentative recovery.
    image.put<std::uint32_t>(0x222, 0x2000);
    putFde(image, 0x260, 0x400, 0);
    const std::pair<std::uint64_t, std::uint64_t> rows[] = {{0x400, 0x260}};
    putHeader(image, rows);

    spark::symbol_guess::dwarf::ParseStats stats{};
    CHECK(spark::symbol_guess::dwarf::parseEhFrameHeader(
              image, kHeader, 0x40, {}, &stats)
              .empty());
    CHECK(stats.eh_frame_records == 0);
    CHECK(stats.unindexed_ranges == 0);
  }
}

} // namespace

int main() {
  testExactRangesAndGap();
  testDuplicateAndMismatch();
  testOverlapIsRejected();
  testMalformedAndBounds();
  testUnindexedFdeRecoveryRequiresTerminator();
  if (failures != 0) {
    std::cerr << failures << " DWARF symbol guess test(s) failed\n";
    return 1;
  }
  std::cout << "DWARF symbol guess tests passed\n";
  return 0;
}
