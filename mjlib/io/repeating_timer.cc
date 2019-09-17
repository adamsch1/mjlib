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

#include "mjlib/io/repeating_timer.h"

#include <functional>

#include <boost/date_time/posix_time/posix_time.hpp>

#include "mjlib/io/now.h"

namespace mjlib {
namespace io {

RepeatingTimer::RepeatingTimer(boost::asio::io_context& service)
    : service_(service),
      timer_(service) {}

void RepeatingTimer::start(boost::posix_time::time_duration period,
                           io::ErrorCallback callback) {
  period_ = period;
  callback_ = callback;

  StartInternal();
}

std::size_t RepeatingTimer::cancel() {
  callback_ = {};
  return timer_.cancel();
}

void RepeatingTimer::StartInternal() {
  const auto now = io::Now(service_);
  const auto last_expires = timer_.expires_at();
  if (last_expires.is_special()) {
    // Just start this at a regular interval from now.
    timer_.expires_at(now + period_);
  } else {
    // See if we would have skipped a cycle.
    auto next = last_expires + period_;
    if (next < now) {
      // Yes, we would have skipped.
      service_.post(std::bind(callback_, boost::asio::error::operation_aborted));
      // Just try to catch back up by going as quickly as possible.
      next = now + period_;
    }
    timer_.expires_at(next);
  }
  timer_.async_wait(std::bind(&RepeatingTimer::HandleTimer,
                              this, std::placeholders::_1));
}

void RepeatingTimer::HandleTimer(const base::error_code& ec) {
  if (!callback_) { return; }
  StartInternal();
  service_.post(std::bind(callback_, ec));
}

}
}
