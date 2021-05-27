// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ObjectUtils/CoffFile.h"

#include <absl/strings/str_format.h>
#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>

#include "OrbitBase/Logging.h"
#include "OrbitBase/Result.h"
#include "symbol.pb.h"

namespace orbit_object_utils {

namespace {

using orbit_grpc_protos::ModuleSymbols;
using orbit_grpc_protos::SymbolInfo;

class CoffFileImpl : public CoffFile {
 public:
  CoffFileImpl(std::filesystem::path file_path,
               llvm::object::OwningBinary<llvm::object::ObjectFile>&& owning_binary);
  [[nodiscard]] ErrorMessageOr<orbit_grpc_protos::ModuleSymbols> LoadDebugSymbols() override;
  [[nodiscard]] bool HasDebugSymbols() const override;
  [[nodiscard]] std::string GetName() const override;
  [[nodiscard]] const std::filesystem::path& GetFilePath() const override;
  [[nodiscard]] uint64_t GetLoadBias() const override;
  [[nodiscard]] uint64_t GetExecutableSegmentOffset() const override;
  [[nodiscard]] bool IsElf() const override;
  [[nodiscard]] bool IsCoff() const override;

 private:
  ErrorMessageOr<uint64_t> GetSectionOffsetForSymbol(const llvm::object::SymbolRef& symbol_ref);
  ErrorMessageOr<SymbolInfo> CreateSymbolInfo(const llvm::object::SymbolRef& symbol_ref);
  const std::filesystem::path file_path_;
  llvm::object::OwningBinary<llvm::object::ObjectFile> owning_binary_;
  llvm::object::COFFObjectFile* object_file_;
  bool has_debug_info_;
};

CoffFileImpl::CoffFileImpl(std::filesystem::path file_path,
                           llvm::object::OwningBinary<llvm::object::ObjectFile>&& owning_binary)
    : file_path_(std::move(file_path)),
      owning_binary_(std::move(owning_binary)),
      has_debug_info_(false) {
  object_file_ = llvm::dyn_cast<llvm::object::COFFObjectFile>(owning_binary_.getBinary());
  const auto dwarf_context = llvm::DWARFContext::create(*owning_binary_.getBinary());
  if (dwarf_context != nullptr) {
    has_debug_info_ = true;
  }
}

static void FillDebugSymbolsFromDWARF(llvm::DWARFContext* dwarf_context,
                                      ModuleSymbols* module_symbols) {
  for (const auto& info_section : dwarf_context->compile_units()) {
    for (uint32_t index = 0; index < info_section->getNumDIEs(); ++index) {
      llvm::DWARFDie full_die = info_section->getDIEAtIndex(index);
      // We only want symbols of functions, which are DIEs with isSubprogramDIR()
      // and not of inlined functions, which are DIEs with isSubroutineDIE().
      if (full_die.isSubprogramDIE()) {
        SymbolInfo symbol_info;
        uint64_t low_pc;
        uint64_t high_pc;
        uint64_t unused_section_index;
        if (!full_die.getLowAndHighPC(low_pc, high_pc, unused_section_index)) {
          continue;
        }
        // The method getName will fallback to ShortName if LinkageName is
        // not present, so this should never return an empty name.
        std::string name(full_die.getName(llvm::DINameKind::LinkageName));
        CHECK(!name.empty());
        symbol_info.set_name(name);
        symbol_info.set_demangled_name(llvm::demangle(name));
        symbol_info.set_address(low_pc);
        symbol_info.set_size(high_pc - low_pc);
        *(module_symbols->add_symbol_infos()) = std::move(symbol_info);
      }
    }
  }
}

ErrorMessageOr<ModuleSymbols> CoffFileImpl::LoadDebugSymbols() {
  const auto dwarf_context = llvm::DWARFContext::create(*owning_binary_.getBinary());
  if (dwarf_context == nullptr) {
    return ErrorMessage("Could not create DWARF context.");
  }

  ModuleSymbols module_symbols;
  module_symbols.set_symbols_file_path(file_path_.string());

  FillDebugSymbolsFromDWARF(dwarf_context.get(), &module_symbols);

  if (module_symbols.symbol_infos_size() == 0) {
    return ErrorMessage(
        "Unable to load symbols from object file, not even a single symbol of "
        "type function found.");
  }
  return module_symbols;
}

bool CoffFileImpl::HasDebugSymbols() const { return has_debug_info_; }

const std::filesystem::path& CoffFileImpl::GetFilePath() const { return file_path_; }

std::string CoffFileImpl::GetName() const { return file_path_.filename().string(); }

uint64_t CoffFileImpl::GetLoadBias() const { return object_file_->getImageBase(); }
uint64_t CoffFileImpl::GetExecutableSegmentOffset() const {
  CHECK(object_file_->is64());
  return object_file_->getPE32PlusHeader()->BaseOfCode;
}

bool CoffFileImpl::IsElf() const { return false; }
bool CoffFileImpl::IsCoff() const { return true; }

}  // namespace

ErrorMessageOr<std::unique_ptr<CoffFile>> CreateCoffFile(const std::filesystem::path& file_path) {
  // TODO(hebecker): Remove this explicit construction of StringRef when we
  // switch to LLVM10.
  const std::string file_path_str = file_path.string();
  const llvm::StringRef file_path_llvm{file_path_str};

  llvm::Expected<llvm::object::OwningBinary<llvm::object::ObjectFile>> object_file_or_error =
      llvm::object::ObjectFile::createObjectFile(file_path_llvm);

  if (!object_file_or_error) {
    return ErrorMessage(absl::StrFormat("Unable to load COFF file \"%s\": %s", file_path.string(),
                                        llvm::toString(object_file_or_error.takeError())));
  }

  llvm::object::OwningBinary<llvm::object::ObjectFile>& file = object_file_or_error.get();

  return CreateCoffFile(file_path, std::move(file));
}

ErrorMessageOr<std::unique_ptr<CoffFile>> CreateCoffFile(
    const std::filesystem::path& file_path,
    llvm::object::OwningBinary<llvm::object::ObjectFile>&& file) {
  llvm::object::COFFObjectFile* coff_object_file =
      llvm::dyn_cast<llvm::object::COFFObjectFile>(file.getBinary());
  if (coff_object_file != nullptr) {
    if (!coff_object_file->is64()) {
      return ErrorMessage(absl::StrFormat("Only 64-bit object files are supported."));
    }
    return std::unique_ptr<CoffFile>(new CoffFileImpl(file_path, std::move(file)));
  } else {
    return ErrorMessage(absl::StrFormat("Unable to load object file \"%s\":", file_path.string()));
  }
}

}  // namespace orbit_object_utils
