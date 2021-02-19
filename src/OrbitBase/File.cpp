// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "OrbitBase/File.h"

#include "OrbitBase/Logging.h"

namespace orbit_base {

static void FdDeleter(int fd) {
  if (fd != -1) close(fd);
};

ErrorMessageOr<unique_fd> OpenFileForReading(const std::filesystem::path& path) {
#if defined(__linux)
  constexpr int open_flags = O_RDONLY | O_CLOEXEC;
#elif defined(_WIN32)
  constexpr int open_flags = O_RDONLY | O_BINARY;
#endif  // defined(__linux)
  int fd = TEMP_FAILURE_RETRY(open(path.string().c_str(), open_flags));
  if (fd == -1) {
    return ErrorMessage(
        absl::StrFormat("Unable to open file \"%s\": %s", path.string(), SafeStrerror(errno)));
  }

  return unique_fd{fd, FdDeleter};
}

ErrorMessageOr<unique_fd> OpenFileForWriting(const std::filesystem::path& path) {
#if defined(__linux)
  constexpr int open_flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
  constexpr int open_mode = 0600;
#elif defined(_WIN32)
  constexpr int open_flags = O_WRONLY | O_CREAT | O_TRUNC | O_BINARY;
  constexpr int open_mode = _S_IREAD | _S_IWRITE;
#endif  // defined(__linux)
  int fd = TEMP_FAILURE_RETRY(open(path.string().c_str(), open_flags, open_mode));

  if (fd == -1) {
    return ErrorMessage(
        absl::StrFormat("Unable to open file \"%s\": %s", path.string(), SafeStrerror(errno)));
  }

  return unique_fd{fd, FdDeleter};
}

ErrorMessageOr<void> WriteFully(const unique_fd& fd, std::string_view content) {
  int64_t bytes_left = content.size();
  const char* current_position = content.data();
  while (bytes_left > 0) {
    int64_t bytes_written = TEMP_FAILURE_RETRY(write(fd, current_position, bytes_left));
    if (bytes_written == -1) {
      return ErrorMessage{SafeStrerror(errno)};
    }
    current_position += bytes_written;
    bytes_left -= bytes_written;
  }

  CHECK(bytes_left == 0);
  return outcome::success();
}

}  // namespace orbit_base