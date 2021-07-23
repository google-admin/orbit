// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ObjectUtils/ElfFile.h"

#include <absl/base/casts.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/Twine.h>
#include <llvm/ADT/iterator_range.h>
#include <llvm/BinaryFormat/ELF.h>
#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/DebugInfo/DWARF/DWARFDebugLine.h>
#include <llvm/DebugInfo/DWARF/DWARFFormValue.h>
#include <llvm/DebugInfo/Symbolize/Symbolize.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/ELF.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ELFTypes.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Object/SymbolicFile.h>
#include <llvm/Support/CRC.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MathExtras.h>
#include <llvm/Support/MemoryBuffer.h>

#include <outcome.hpp>
#include <type_traits>
#include <utility>
#include <vector>

#include "OrbitBase/File.h"
#include "OrbitBase/Logging.h"
#include "OrbitBase/ReadFileToString.h"
#include "OrbitBase/Result.h"
#include "symbol.pb.h"

namespace orbit_object_utils {

namespace {

using orbit_grpc_protos::LineInfo;
using orbit_grpc_protos::ModuleSymbols;
using orbit_grpc_protos::SymbolInfo;

template <typename ElfT>
class ElfFileImpl : public ElfFile {
 public:
  ElfFileImpl(std::filesystem::path file_path,
              llvm::object::OwningBinary<llvm::object::ObjectFile>&& owning_binary);

  ErrorMessageOr<void> Initialize();

  // Loads symbols from the .symtab section.
  [[nodiscard]] ErrorMessageOr<ModuleSymbols> LoadDebugSymbols() override;
  [[nodiscard]] ErrorMessageOr<ModuleSymbols> LoadSymbolsFromDynsym() override;
  [[nodiscard]] uint64_t GetLoadBias() const override;
  [[nodiscard]] uint64_t GetExecutableSegmentOffset() const override;
  [[nodiscard]] bool HasDebugSymbols() const override;
  [[nodiscard]] bool HasDynsym() const override;
  [[nodiscard]] bool HasDebugInfo() const override;
  [[nodiscard]] bool HasGnuDebuglink() const override;
  [[nodiscard]] bool Is64Bit() const override;
  [[nodiscard]] bool IsElf() const override;
  [[nodiscard]] bool IsCoff() const override;
  [[nodiscard]] std::string GetBuildId() const override;
  [[nodiscard]] std::string GetName() const override;
  [[nodiscard]] std::string GetSoname() const override;
  [[nodiscard]] const std::filesystem::path& GetFilePath() const override;
  [[nodiscard]] ErrorMessageOr<LineInfo> GetLineInfo(uint64_t address) override;
  [[nodiscard]] ErrorMessageOr<LineInfo> GetDeclarationLocationOfFunction(
      uint64_t address) override;
  [[nodiscard]] std::optional<GnuDebugLinkInfo> GetGnuDebugLinkInfo() const override;

 private:
  ErrorMessageOr<void> InitSections();
  ErrorMessageOr<void> InitProgramHeaders();
  ErrorMessageOr<void> InitDynamicEntries();
  ErrorMessageOr<SymbolInfo> CreateSymbolInfo(const llvm::object::ELFSymbolRef& symbol_ref);

  const std::filesystem::path file_path_;
  llvm::object::OwningBinary<llvm::object::ObjectFile> owning_binary_;
  llvm::object::ELFObjectFile<ElfT>* object_file_;
  llvm::symbolize::LLVMSymbolizer symbolizer_;
  std::string build_id_;
  std::string soname_;
  bool has_symtab_section_;
  bool has_dynsym_section_;
  bool has_debug_info_section_;
  std::optional<GnuDebugLinkInfo> gnu_debuglink_info_;

  uint64_t load_bias_;
  uint64_t executable_segment_offset_;
};

template <typename ElfT>
ErrorMessageOr<GnuDebugLinkInfo> ReadGnuDebuglinkSection(
    const typename ElfT::Shdr& section_header, const llvm::object::ELFFile<ElfT>& elf_file) {
  llvm::Expected<llvm::ArrayRef<uint8_t>> contents_or_error =
      elf_file.getSectionContents(section_header);
  if (!contents_or_error) return ErrorMessage("Could not obtain contents.");

  const llvm::ArrayRef<uint8_t>& contents = contents_or_error.get();

  using ChecksumType = uint32_t;
  constexpr size_t kMinimumPathLength = 1;
  if (contents.size() < kMinimumPathLength + sizeof(ChecksumType)) {
    return ErrorMessage{"Section is too short."};
  }

  constexpr size_t kOneHundredKiB = 100 * 1024;
  if (contents.size() > kOneHundredKiB) {
    return ErrorMessage{"Section is longer than 100KiB. Something is not right."};
  }

  std::string path{};
  path.reserve(contents.size());

  for (const auto& byte : contents) {
    if (byte == '\0') break;
    path.push_back(static_cast<char>(byte));
  }

  if (path.size() > contents.size() - sizeof(ChecksumType)) {
    return ErrorMessage{"No CRC32 checksum found"};
  }

  static_assert(ElfT::TargetEndianness == llvm::support::little,
                "This code only supports little endian architectures.");
  const auto checksum_storage = contents.slice(contents.size() - sizeof(ChecksumType));
  uint32_t reference_crc{};
  std::memcpy(&reference_crc, checksum_storage.data(), sizeof(reference_crc));

  GnuDebugLinkInfo gnu_debuglink_info{};
  gnu_debuglink_info.path = std::filesystem::path{std::move(path)};
  gnu_debuglink_info.crc32_checksum = reference_crc;
  return gnu_debuglink_info;
}

template <typename ElfT>
ElfFileImpl<ElfT>::ElfFileImpl(std::filesystem::path file_path,
                               llvm::object::OwningBinary<llvm::object::ObjectFile>&& owning_binary)
    : file_path_(std::move(file_path)),
      owning_binary_(std::move(owning_binary)),
      object_file_(llvm::dyn_cast<llvm::object::ELFObjectFile<ElfT>>(owning_binary_.getBinary())),
      has_symtab_section_(false),
      has_dynsym_section_(false),
      has_debug_info_section_(false),
      load_bias_{0},
      executable_segment_offset_{0} {}

template <typename ElfT>
ErrorMessageOr<void> ElfFileImpl<ElfT>::Initialize() {
  OUTCOME_TRY(InitSections());
  OUTCOME_TRY(InitDynamicEntries());
  OUTCOME_TRY(InitProgramHeaders());
  return outcome::success();
}

template <typename ElfT>
ErrorMessageOr<void> ElfFileImpl<ElfT>::InitDynamicEntries() {
  const llvm::object::ELFFile<ElfT>& elf_file = object_file_->getELFFile();
  auto dynamic_entries_or_error = elf_file.dynamicEntries();
  if (!dynamic_entries_or_error) {
    auto error_message =
        absl::StrFormat("Unable to get dynamic entries from \"%s\": %s", file_path_.string(),
                        llvm::toString(dynamic_entries_or_error.takeError()));
    ERROR("%s (ignored)", error_message);
    // Apparently empty dynamic section results in error - we are going to ignore it.
    return outcome::success();
  }

  std::optional<uint64_t> soname_offset;
  std::optional<uint64_t> dynamic_string_table_addr;
  std::optional<uint64_t> dynamic_string_table_size;
  auto dyn_range = dynamic_entries_or_error.get();
  for (const auto& dyn_entry : dyn_range) {
    switch (dyn_entry.getTag()) {
      case llvm::ELF::DT_SONAME:
        soname_offset.emplace(dyn_entry.getVal());
        break;
      case llvm::ELF::DT_STRTAB:
        dynamic_string_table_addr.emplace(dyn_entry.getPtr());
        break;
      case llvm::ELF::DT_STRSZ:
        dynamic_string_table_size.emplace(dyn_entry.getVal());
        break;
      default:
        break;
    }
  }

  if (!soname_offset.has_value() || !dynamic_string_table_addr.has_value() ||
      !dynamic_string_table_size.has_value()) {
    return outcome::success();
  }

  auto strtab_last_byte_or_error = elf_file.toMappedAddr(dynamic_string_table_addr.value() +
                                                         dynamic_string_table_size.value() - 1);
  if (!strtab_last_byte_or_error) {
    auto error_message =
        absl::StrFormat("Unable to get last byte address of dynamic string table \"%s\": %s",
                        file_path_.string(), llvm::toString(strtab_last_byte_or_error.takeError()));
    ERROR("%s", error_message);
    return ErrorMessage{error_message};
  }

  if (soname_offset.value() >= dynamic_string_table_size.value()) {
    auto error_message = absl::StrFormat(
        "Soname offset is out of bounds of the string table (file=\"%s\", offset=%u "
        "strtab size=%u)",
        file_path_.string(), soname_offset.value(), dynamic_string_table_size.value());
    ERROR("%s", error_message);
    return ErrorMessage{error_message};
  }

  if (*strtab_last_byte_or_error.get() != 0) {
    auto error_message = absl::StrFormat(
        "Dynamic string table is not null-termintated (file=\"%s\")", file_path_.string());
    ERROR("%s", error_message);
    return ErrorMessage{error_message};
  }

  auto strtab_addr_or_error = elf_file.toMappedAddr(dynamic_string_table_addr.value());
  if (!strtab_addr_or_error) {
    auto error_message =
        absl::StrFormat("Unable to get dynamic string table from DT_STRTAB in \"%s\": %s",
                        file_path_.string(), llvm::toString(strtab_addr_or_error.takeError()));
    ERROR("%s", error_message);
    return ErrorMessage{error_message};
  }

  soname_ = absl::bit_cast<const char*>(strtab_addr_or_error.get()) + soname_offset.value();

  return outcome::success();
}

template <typename ElfT>
ErrorMessageOr<void> ElfFileImpl<ElfT>::InitSections() {
  const llvm::object::ELFFile<ElfT>& elf_file = object_file_->getELFFile();

  llvm::Expected<typename ElfT::ShdrRange> sections_or_error = elf_file.sections();
  if (!sections_or_error) {
    auto error_message = absl::StrFormat("Unable to load sections: %s",
                                         llvm::toString(sections_or_error.takeError()));
    ERROR("%s", error_message);
    return ErrorMessage{error_message};
  }

  for (const typename ElfT::Shdr& section : sections_or_error.get()) {
    llvm::Expected<llvm::StringRef> name_or_error = elf_file.getSectionName(section);
    if (!name_or_error) {
      return ErrorMessage{absl::StrFormat("Unable to get section name: %s",
                                          llvm::toString(name_or_error.takeError()))};
    }
    llvm::StringRef name = name_or_error.get();

    if (name.str() == ".symtab") {
      has_symtab_section_ = true;
      continue;
    }

    if (section.sh_type == llvm::ELF::SHT_DYNSYM) {
      has_dynsym_section_ = true;
      continue;
    }

    if (name.str() == ".debug_info") {
      has_debug_info_section_ = true;
      continue;
    }

    if (name.str() == ".note.gnu.build-id" && section.sh_type == llvm::ELF::SHT_NOTE) {
      llvm::Error error = llvm::Error::success();
      for (const typename ElfT::Note& note : elf_file.notes(section, error)) {
        if (note.getType() != llvm::ELF::NT_GNU_BUILD_ID) continue;

        llvm::ArrayRef<uint8_t> desc = note.getDesc();
        for (const uint8_t& byte : desc) {
          absl::StrAppend(&build_id_, absl::Hex(byte, absl::kZeroPad2));
        }
      }
      if (error) {
        return ErrorMessage{
            absl::StrFormat("Error while reading elf notes: %s", llvm::toString(std::move(error)))};
      }
      continue;
    }

    if (name.str() == ".gnu_debuglink") {
      ErrorMessageOr<GnuDebugLinkInfo> error_or_info = ReadGnuDebuglinkSection(section, elf_file);
      if (error_or_info.has_value()) {
        gnu_debuglink_info_ = std::move(error_or_info.value());
      } else {
        return ErrorMessage{absl::StrFormat("Invalid .gnu_debuglink section in \"%s\". %s",
                                            file_path_.string(), error_or_info.error().message())};
      }
      continue;
    }
  }

  return outcome::success();
}

template <typename ElfT>
ErrorMessageOr<SymbolInfo> ElfFileImpl<ElfT>::CreateSymbolInfo(
    const llvm::object::ELFSymbolRef& symbol_ref) {
  std::string name;
  if (auto maybe_name = symbol_ref.getName(); maybe_name) name = maybe_name.get().str();

  llvm::Expected<uint32_t> maybe_flags = symbol_ref.getFlags();
  if (!maybe_flags) {
    LOG("WARNING: Flags are not set for symbol \"%s\" in \"%s\", skipping.", name,
        file_path_.string());
    return ErrorMessage(absl::StrFormat(R"(Flags are not set for symbol "%s" in "%s", skipping.)",
                                        name, file_path_.string()));
  }

  if ((maybe_flags.get() & llvm::object::BasicSymbolRef::SF_Undefined) != 0) {
    return ErrorMessage("Symbol is defined in another object file (SF_Undefined flag is set).");
  }

  // Unknown type - skip and generate a warning.
  llvm::Expected<llvm::object::SymbolRef::Type> maybe_type = symbol_ref.getType();
  if (!maybe_type) {
    LOG("WARNING: Type is not set for symbol \"%s\" in \"%s\", skipping.", name,
        file_path_.string());
    return ErrorMessage(absl::StrFormat(R"(Type is not set for symbol "%s" in "%s", skipping.)",
                                        name, file_path_.string()));
  }
  // Limit list of symbols to functions. Ignore sections and variables.
  if (maybe_type.get() != llvm::object::SymbolRef::ST_Function) {
    return ErrorMessage("Symbol is not a function.");
  }

  llvm::Expected<uint64_t> maybe_value = symbol_ref.getValue();
  if (!maybe_value) {
    LOG("WARNING: Address is not set for symbol \"%s\" in \"%s\", skipping.", name,
        file_path_.string());
    return ErrorMessage(absl::StrFormat(R"(Address is not set for symbol "%s" in "%s", skipping.)",
                                        name, file_path_.string()));
  }

  SymbolInfo symbol_info;
  symbol_info.set_name(name);
  symbol_info.set_demangled_name(llvm::demangle(name));
  symbol_info.set_address(maybe_value.get());
  symbol_info.set_size(symbol_ref.getSize());
  return symbol_info;
}

template <typename ElfT>
ErrorMessageOr<ModuleSymbols> ElfFileImpl<ElfT>::LoadDebugSymbols() {
  if (!has_symtab_section_) {
    return ErrorMessage("ELF file does not have a .symtab section.");
  }

  ModuleSymbols module_symbols;
  module_symbols.set_load_bias(load_bias_);
  module_symbols.set_symbols_file_path(file_path_.string());

  for (const llvm::object::ELFSymbolRef& symbol_ref : object_file_->symbols()) {
    auto symbol_or_error = CreateSymbolInfo(symbol_ref);
    if (symbol_or_error.has_value()) {
      *module_symbols.add_symbol_infos() = std::move(symbol_or_error.value());
    }
  }

  if (module_symbols.symbol_infos_size() == 0) {
    return ErrorMessage(
        "Unable to load symbols from ELF file, not even a single symbol of "
        "type function found.");
  }
  return module_symbols;
}

template <typename ElfT>
ErrorMessageOr<ModuleSymbols> ElfFileImpl<ElfT>::LoadSymbolsFromDynsym() {
  if (!has_dynsym_section_) {
    return ErrorMessage("ELF file does not have a .dynsym section.");
  }

  ModuleSymbols module_symbols;
  module_symbols.set_load_bias(load_bias_);
  module_symbols.set_symbols_file_path(file_path_.string());

  for (const llvm::object::ELFSymbolRef& symbol_ref : object_file_->getDynamicSymbolIterators()) {
    auto symbol_or_error = CreateSymbolInfo(symbol_ref);
    if (symbol_or_error.has_value()) {
      *module_symbols.add_symbol_infos() = std::move(symbol_or_error.value());
    }
  }

  if (module_symbols.symbol_infos_size() == 0) {
    return ErrorMessage(
        "Unable to load symbols from .dynsym section, not even a single symbol of type function "
        "found.");
  }
  return module_symbols;
}

template <typename ElfT>
uint64_t ElfFileImpl<ElfT>::GetLoadBias() const {
  return load_bias_;
}

template <typename ElfT>
ErrorMessageOr<void> ElfFileImpl<ElfT>::InitProgramHeaders() {
  const llvm::object::ELFFile<ElfT>& elf_file = object_file_->getELFFile();
  llvm::Expected<typename ElfT::PhdrRange> range = elf_file.program_headers();

  if (!range) {
    std::string error = absl::StrFormat(
        "Unable to get load bias of ELF file: \"%s\". Error loading program headers: %s",
        file_path_.string(), llvm::toString(range.takeError()));
    ERROR("%s", error);
    return ErrorMessage(std::move(error));
  }

  // Find the executable segment and calculate the load bias based on that segment.
  for (const typename ElfT::Phdr& phdr : range.get()) {
    if (phdr.p_type != llvm::ELF::PT_LOAD) {
      continue;
    }

    if ((phdr.p_flags & llvm::ELF::PF_X) == 0) {
      continue;
    }

    load_bias_ = phdr.p_vaddr - phdr.p_offset;
    executable_segment_offset_ = phdr.p_offset;
    return outcome::success();
  }

  std::string error = absl::StrFormat(
      "Unable to get load bias of ELF file: \"%s\". No executable PT_LOAD segment found.",
      file_path_.string());
  ERROR("%s", error);
  return ErrorMessage(std::move(error));
}

template <typename ElfT>
bool ElfFileImpl<ElfT>::HasDebugSymbols() const {
  return has_symtab_section_;
}

template <typename ElfT>
bool ElfFileImpl<ElfT>::HasDynsym() const {
  return has_dynsym_section_;
}

template <typename ElfT>
bool ElfFileImpl<ElfT>::HasDebugInfo() const {
  return has_debug_info_section_;
}

template <typename ElfT>
bool ElfFileImpl<ElfT>::HasGnuDebuglink() const {
  return gnu_debuglink_info_.has_value();
}

template <typename ElfT>
std::string ElfFileImpl<ElfT>::GetBuildId() const {
  return build_id_;
}

template <typename ElfT>
std::string ElfFileImpl<ElfT>::GetName() const {
  return soname_.empty() ? std::filesystem::path{file_path_}.filename().string() : soname_;
}

template <typename ElfT>
std::string ElfFileImpl<ElfT>::GetSoname() const {
  return soname_;
}

template <typename ElfT>
const std::filesystem::path& ElfFileImpl<ElfT>::GetFilePath() const {
  return file_path_;
}

template <typename ElfT>
ErrorMessageOr<LineInfo> orbit_object_utils::ElfFileImpl<ElfT>::GetLineInfo(uint64_t address) {
  CHECK(has_debug_info_section_);
  auto line_info_or_error =
      symbolizer_.symbolizeInlinedCode(std::string{object_file_->getFileName()},
                                       {address, llvm::object::SectionedAddress::UndefSection});
  if (!line_info_or_error) {
    return ErrorMessage(
        absl::StrFormat("Unable to get line number info for \"%s\", address=0x%x: %s",
                        std::string{object_file_->getFileName()}, address,
                        llvm::toString(line_info_or_error.takeError())));
  }

  auto& symbolizer_line_info = line_info_or_error.get();
  const uint32_t number_of_frames = symbolizer_line_info.getNumberOfFrames();

  // Getting back zero frames means there was some kind of problem. We will return a error.
  if (number_of_frames == 0) {
    return ErrorMessage(absl::StrFormat("Unable to get line info for address=0x%x", address));
  }

  const auto& last_frame = symbolizer_line_info.getFrame(number_of_frames - 1);

  // This is what symbolizer returns in case of an error. We convert it to a ErrorMessage here.
  if (last_frame.FileName == "<invalid>" && last_frame.Line == 0) {
    return ErrorMessage(absl::StrFormat("Unable to get line info for address=0x%x", address));
  }

  LineInfo line_info;
  line_info.set_source_file(last_frame.FileName);
  line_info.set_source_line(last_frame.Line);
  return line_info;
}

template <typename ElfT>
ErrorMessageOr<LineInfo> orbit_object_utils::ElfFileImpl<ElfT>::GetDeclarationLocationOfFunction(
    uint64_t address) {
  const auto dwarf_context = llvm::DWARFContext::create(*owning_binary_.getBinary());
  if (dwarf_context == nullptr) return ErrorMessage{"Could not read DWARF information."};

  const auto offset = dwarf_context->getDebugAranges()->findAddress(address);
  auto* const compile_unit = dwarf_context->getCompileUnitForOffset(offset);
  if (compile_unit == nullptr) return ErrorMessage{"Invalid address"};

  const auto subroutine = compile_unit->getSubroutineForAddress(address);
  if (!subroutine.isValid()) return ErrorMessage{"Address not associated with any subroutine"};

  const auto decl_file_index =
      llvm::dwarf::toUnsigned(subroutine.findRecursively(llvm::dwarf::Attribute::DW_AT_decl_file));
  if (!decl_file_index) return ErrorMessage{"Could not find source file location"};

  const llvm::DWARFDebugLine::LineTable* const line_table =
      dwarf_context->getLineTableForUnit(compile_unit);
  if (line_table == nullptr) return ErrorMessage{"Line Table was missing in debug information"};

  std::string file_path;
  const auto result = line_table->getFileNameByIndex(
      *decl_file_index, compile_unit->getCompilationDir(),
      llvm::DILineInfoSpecifier::FileLineInfoKind::AbsoluteFilePath, file_path);
  if (!result) return ErrorMessage{"Source declaration file path not found in debug information."};

  LineInfo line_info{};
  line_info.set_source_line(subroutine.getDeclLine());
  line_info.set_source_file(file_path);
  return line_info;
}

template <>
bool ElfFileImpl<llvm::object::ELF64LE>::Is64Bit() const {
  return true;
}

template <>
bool ElfFileImpl<llvm::object::ELF32LE>::Is64Bit() const {
  return false;
}

template <typename ElfT>
bool ElfFileImpl<ElfT>::IsCoff() const {
  return false;
}

template <typename ElfT>
bool ElfFileImpl<ElfT>::IsElf() const {
  return true;
}

template <typename ElfT>
std::optional<GnuDebugLinkInfo> ElfFileImpl<ElfT>::GetGnuDebugLinkInfo() const {
  return gnu_debuglink_info_;
}

template <typename ElfT>
uint64_t ElfFileImpl<ElfT>::GetExecutableSegmentOffset() const {
  return executable_segment_offset_;
}
}  // namespace

ErrorMessageOr<std::unique_ptr<ElfFile>> CreateElfFileFromBuffer(
    const std::filesystem::path& file_path, const void* buf, size_t len) {
  std::unique_ptr<llvm::MemoryBuffer> buffer = llvm::MemoryBuffer::getMemBuffer(
      llvm::StringRef(static_cast<const char*>(buf), len), llvm::StringRef("buffer name"), false);
  llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>> object_file_or_error =
      llvm::object::ObjectFile::createObjectFile(buffer->getMemBufferRef());

  if (!object_file_or_error) {
    return ErrorMessage(absl::StrFormat("Unable to load ELF file \"%s\": %s", file_path.string(),
                                        llvm::toString(object_file_or_error.takeError())));
  }

  return CreateElfFile(file_path, llvm::object::OwningBinary<llvm::object::ObjectFile>(
                                      std::move(object_file_or_error.get()), std::move(buffer)));
}

ErrorMessageOr<std::unique_ptr<ElfFile>> CreateElfFile(const std::filesystem::path& file_path) {
  llvm::Expected<llvm::object::OwningBinary<llvm::object::ObjectFile>> object_file_or_error =
      llvm::object::ObjectFile::createObjectFile(file_path.string());

  if (!object_file_or_error) {
    return ErrorMessage(absl::StrFormat("Unable to load ELF file \"%s\": %s", file_path.string(),
                                        llvm::toString(object_file_or_error.takeError())));
  }

  llvm::object::OwningBinary<llvm::object::ObjectFile>& file = object_file_or_error.get();

  return CreateElfFile(file_path, std::move(file));
}

ErrorMessageOr<std::unique_ptr<ElfFile>> CreateElfFile(
    const std::filesystem::path& file_path,
    llvm::object::OwningBinary<llvm::object::ObjectFile>&& file) {
  llvm::object::ObjectFile* object_file = file.getBinary();

  // Create appropriate ElfFile implementation
  if (llvm::dyn_cast<llvm::object::ELF32LEObjectFile>(object_file) != nullptr) {
    auto elf_file =
        std::make_unique<ElfFileImpl<llvm::object::ELF32LE>>(file_path, std::move(file));
    OUTCOME_TRY(elf_file->Initialize());
    return elf_file;
  }

  if (llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(object_file) != nullptr) {
    auto elf_file =
        std::make_unique<ElfFileImpl<llvm::object::ELF64LE>>(file_path, std::move(file));
    OUTCOME_TRY(elf_file->Initialize());
    return elf_file;
  }

  return ErrorMessage(absl::StrFormat(
      "Unable to load \"%s\": Big-endian architectures are not supported.", file_path.string()));
}

ErrorMessageOr<uint32_t> ElfFile::CalculateDebuglinkChecksum(
    const std::filesystem::path& file_path) {
  // TODO(b/180995172): Make this read operation iterative since the potentially read files
  // can be very large (gigabytes).
  ErrorMessageOr<orbit_base::unique_fd> fd_or_error = orbit_base::OpenFileForReading(file_path);

  if (fd_or_error.has_error()) {
    return fd_or_error.error();
  }

  constexpr size_t kBufferSize = 4 * 1024 * 1024;  // 4 MiB
  std::vector<unsigned char> buffer(kBufferSize);

  uint32_t rolling_checksum = 0;

  while (true) {
    ErrorMessageOr<size_t> chunksize_or_error =
        orbit_base::ReadFully(fd_or_error.value(), buffer.data(), buffer.size());
    if (chunksize_or_error.has_error()) return chunksize_or_error.error();

    if (chunksize_or_error.value() == 0) break;

    const llvm::ArrayRef<unsigned char> str{buffer.data(), chunksize_or_error.value()};
    rolling_checksum = llvm::crc32(rolling_checksum, str);
  }

  return rolling_checksum;
}

}  // namespace orbit_object_utils
