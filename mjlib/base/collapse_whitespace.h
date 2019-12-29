// Copyright 2019 Josh Pieper, jjp@pobox.com.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cctype>

#include <ostream>
#include <string>

namespace mjlib {
namespace base {

std::string CollapseWhitespace(const std::string& str) {
  std::ostringstream ostr;
  bool was_whitespace = true;
  for (char c : str) {
    if (!was_whitespace || !std::isspace(c)) {
      ostr.put(c);
    }
    was_whitespace = std::isspace(c);
  }
  return ostr.str();
}

}
}
