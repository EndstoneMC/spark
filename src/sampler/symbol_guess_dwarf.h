#ifndef ENDSTONE_SPARK_SYMBOL_GUESS_DWARF_H
#define ENDSTONE_SPARK_SYMBOL_GUESS_DWARF_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace spark::symbol_guess::dwarf {

struct FunctionRange {
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
  std::uint64_t root = 0;

  bool operator==(const FunctionRange &) const = default;
};

struct ParseStats {
  std::size_t table_entries = 0;
  std::size_t eh_frame_records = 0;
  std::size_t function_ranges = 0;
  std::size_t rejected_entries = 0;
  std::size_t duplicate_ranges = 0;
  std::size_t overlap_ranges = 0;
  std::size_t unindexed_ranges = 0;
  std::size_t gap_ranges = 0;
  std::uint64_t gap_bytes = 0;
};

// Bounds-checked access to one already-mapped image. All addresses are RVAs;
// implementations decide how those RVAs map to storage.
class ImageView {
public:
  virtual ~ImageView() = default;
  virtual bool read(std::uint64_t rva, void *out,
                    std::size_t length) const = 0;
  virtual bool executable(std::uint64_t rva,
                          std::size_t length) const = 0;
  // End of the readable mapped segment containing rva, or zero when rva is
  // not readable. ELF section headers are not guaranteed to remain mapped, so
  // the parser uses this program-header-derived bound for a sequential FDE
  // recovery pass.
  virtual std::uint64_t readableEnd(std::uint64_t rva) const = 0;
};

struct EncodingBases {
  std::uint64_t text = 0;
  std::uint64_t data = 0;
  bool has_text = false;
  bool has_data = false;
};

// Parses the GNU .eh_frame_hdr search table and validates every entry against
// its referenced CIE/FDE. Function ends come from the FDE address_range, never
// from the next table entry. Malformed, conflicting, or overlapping entries are
// rejected conservatively.
std::vector<FunctionRange>
parseEhFrameHeader(const ImageView &image, std::uint64_t header_rva,
                   std::uint64_t header_size, EncodingBases bases = {},
                   ParseStats *stats = nullptr);

const FunctionRange *functionContaining(const std::vector<FunctionRange> &ranges,
                                        std::uint64_t rva);

} // namespace spark::symbol_guess::dwarf

#endif // ENDSTONE_SPARK_SYMBOL_GUESS_DWARF_H
