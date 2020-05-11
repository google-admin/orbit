// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "FramepointerValidatorClient.h"

#include <absl/strings/str_format.h>
#include <absl/strings/str_join.h>

#include <string>

#include "CoreApp.h"
#include "Message.h"
#include "Pdb.h"

FramepointerValidatorClient::FramepointerValidatorClient(
    CoreApp* core_app, TransactionClient* transaction_client)
    : core_app_{core_app}, transaction_client_{transaction_client} {
  auto on_response = [this](const Message& msg, uint64_t id) {
    HandleResponse(msg, id);
  };

  transaction_client_->RegisterTransactionResponseHandler(
      {on_response, Msg_ValidateFramepointer, "Validate Framepointers"});
}

void FramepointerValidatorClient::AnalyzeModule(
    Process* process, const std::vector<std::shared_ptr<Module>>& modules) {
  if (modules.empty()) {
    ERROR("No module to validate, cancelling");
    return;
  }

  std::vector<ModuleDebugInfo> remote_module_infos;

  for (const std::shared_ptr<Module>& module : modules) {
    if (module == nullptr) continue;
    ModuleDebugInfo module_info;
    const char* name = module->m_Name.c_str();
    module_info.m_Name = name;
    module_info.m_PID = process->GetID();
    remote_module_infos.push_back(std::move(module_info));
  }

  if (remote_module_infos.empty()) {
    return;
  }

  uint64_t id = transaction_client_->EnqueueRequest(Msg_ValidateFramepointer,
                                                    remote_module_infos);

  absl::MutexLock lock(&id_mutex_);
  modules_map_[id] = modules;
}

void FramepointerValidatorClient::HandleResponse(const Message& message,
                                                 uint64_t id) {
  std::vector<std::shared_ptr<Function>> functions;
  transaction_client_->ReceiveResponse(message, &functions);

  id_mutex_.Lock();
  std::vector<std::shared_ptr<Module>> modules = modules_map_[id];
  modules_map_.erase(id);
  id_mutex_.Unlock();

  uint64_t num_functions = 0;
  for (const auto& module : modules) {
    num_functions += module->m_Pdb->GetFunctions().size();
  }

  std::string text = absl::StrFormat(
      "info:Framepointer Validation\nFailed to validate %d out of %d functions",
      functions.size(), num_functions);
  core_app_->SendToUiNow(text);
}
