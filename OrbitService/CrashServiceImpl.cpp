// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "CrashServiceImpl.h"

#include "OrbitBase/Logging.h"
#include "services.pb.h"

using grpc::ServerContext;
using grpc::Status;

static void InfiniteRecursion(int num) {
  if (num != 1) {
    InfiniteRecursion(num);
  }
  LOG("%i", num);
}

Status CrashServiceImpl::CrashOrbitService(ServerContext*,
                                           const GetCrashRequest* request,
                                           GetCrashResponse*) {
  switch (request->crash_type()) {
    case GetCrashRequest_CrashType_CHECK_FALSE: {
      CHECK(false);
      break;
    }
    case GetCrashRequest_CrashType_NULL_POINTER_DEREFERENCE: {
      int* null_pointer = nullptr;
      *null_pointer = 0;
      break;
    }
    case GetCrashRequest_CrashType_STACK_OVERFLOW: {
      InfiniteRecursion(0);
      break;
    }
    default:
      break;
  }

  return Status::OK;
}
