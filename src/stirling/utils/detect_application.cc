/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "src/stirling/utils/detect_application.h"

#include <string_view>

#include <absl/strings/str_cat.h>

#include "src/common/exec/exec.h"
#include "src/common/system/config.h"
#include "src/common/system/proc_parser.h"

namespace px {
namespace stirling {

Application DetectApplication(const std::filesystem::path& exe) {
  constexpr std::string_view kJavaFileName = "java";
  constexpr std::string_view kNodeFileName = "node";
  constexpr std::string_view kNodejsFileName = "nodejs";

  if (exe.filename() == kNodeFileName || exe.filename() == kNodejsFileName) {
    return Application::kNode;
  }
  if (exe.filename() == kJavaFileName) {
    return Application::kJava;
  }

  return Application::kUnknown;
}

StatusOr<std::string> GetVersion(const std::filesystem::path& exe, std::string_view version_flag) {
  PL_ASSIGN_OR_RETURN(std::string version, px::Exec(absl::StrCat(exe.string(), " ", version_flag)));
  absl::StripLeadingAsciiWhitespace(&version);
  absl::StripTrailingAsciiWhitespace(&version);
  return version;
}

}  // namespace stirling
}  // namespace px
