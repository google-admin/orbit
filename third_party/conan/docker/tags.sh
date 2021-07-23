# Copyright (c) 2020 The Orbit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Maps conan profiles to docker image tags. The images are expected to be named after the conan profile,
# but the tag can be adjusted here to support versioning of docker containers. This map only lists
# docker images that have versioning support enabled. For all other docker containers the tag `latest`
# should be / will be assumed.
declare -rA docker_image_tag_mapping=( \
  [clang_format]="20210622T084030.289589958" \
  [clang7_release]="20210623T115018.050778297" \
  [clang7_relwithdebinfo]="20210623T115524.844271965" \
  [clang7_debug]="20210623T115534.007364778" \
  [clang8_release]="20210623T115629.862456327" \
  [clang8_relwithdebinfo]="20210623T115813.431147006" \
  [clang8_debug]="20210623T115816.468538889" \
  [clang9_release]="20210623T115201.435218253" \
  [clang9_relwithdebinfo]="20210623T115528.044499634" \
  [clang9_debug]="20210623T115537.014113242" \
  [coverage_clang9]="20210623T115542.912026605" \
  [gcc9_release]="20210623T115343.808432117" \
  [gcc9_relwithdebinfo]="20210623T115531.065509473" \
  [gcc9_debug]="20210623T115540.009801343" \
  [gcc10_release]="20210623T103558.579517276" \
  [gcc10_relwithdebinfo]="20210623T103741.229831546" \
  [gcc10_debug]="20210623T103744.163574415" \
  [ggp_release]="20210721T085124.088699028" \
  [ggp_relwithdebinfo]="20210721T085221.961491965" \
  [ggp_debug]="20210721T085225.097344550" \
  [iwyu]="20210623T060051.145174476" \
  [license_headers]="20210622T081800.703185280" \
  [msvc2019_release]="20210623T062112.000000000" \
  [msvc2019_relwithdebinfo]="20210623T062112.000000000" \
  [msvc2019_debug]="20210623T062112.000000000" \
)

# Use `readonly DOCKER_IMAGE_TAG="${docker_image_tag_mapping[${CONAN_PROFILE}]-latest}"`
# to obtain the tag and fall back to `latest` if none is given.
