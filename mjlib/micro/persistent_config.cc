// Copyright 2015-2019 Josh Pieper, jjp@pobox.com.
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

#include "mjlib/micro/persistent_config.h"

#include <cstring>

#include "mjlib/base/assert.h"
#include "mjlib/base/buffer_stream.h"
#include "mjlib/base/crc.h"
#include "mjlib/base/tokenizer.h"

#include "mjlib/telemetry/format.h"

#include "mjlib/micro/flash.h"
#include "mjlib/micro/pool_map.h"

/// @file
///
/// The persistent storage is an unordered list of elements:
///
///   * Element
///     * pstring - name
///     * 32bit schema CRC
///     * pstring - data
///
/// It is terminated by an element with a 0 length name.
///
/// A pstring is a 32 byte unsigned integer followed by that many
/// bytes of data.

namespace mjlib {
namespace micro {
namespace {
constexpr int kMaxSize = 16;

class SizeCountingStream : public base::WriteStream {
 public:
  void write(const std::string_view& data) override {
    size_ += data.size();
  }

  size_t size() const { return size_; }

 private:
  size_t size_ = 0;
};
}

class PersistentConfig::Impl {
 public:
  Impl(Pool& pool, CommandManager& command_manager, FlashInterface& flash)
      : pool_(pool), flash_(flash), elements_(&pool, kMaxSize) {
    command_manager.Register("conf", [this](auto&& a, auto&& b) {
        this->Command(a, b);
      });
  }

  void Command(const std::string_view& command,
               const CommandManager::Response& response) {
    base::Tokenizer tokenizer(command, " ");
    auto cmd = tokenizer.next();
    if (cmd == "enumerate") {
      Enumerate(response);
    } else if (cmd == "get") {
      Get(tokenizer.remaining(), response);
    } else if (cmd == "set") {
      Set(tokenizer.remaining(), response);
    } else if (cmd == "load") {
      Load(response);
    } else if (cmd == "write") {
      Write(response);
    } else if (cmd == "default") {
      Default(response);
    } else {
      UnknownCommand(cmd, response);
    }
  }

  struct Element {
    SerializableHandlerBase* serializable = nullptr;
    base::inplace_function<void ()> updated;
  };

  using ElementMap = PoolMap<std::string_view, Element>;

  void Enumerate(const CommandManager::Response& response) {
    current_response_ = response;
    current_enumerate_index_ = 0;

    EnumerateCallback({});
  }

  void EnumerateCallback(error_code error) {
    if (error) {
      current_response_.callback(error);
      return;
    }

    const auto element_it = elements_.begin() + current_enumerate_index_;
    if (element_it == elements_.end()) {
      WriteOK(current_response_);
      return;
    }

    current_enumerate_index_++;

    element_it->second.serializable->Enumerate(
        &this->enumerate_context_,
        this->send_buffer_,
        element_it->first,
        *current_response_.stream,
        [this](error_code err) { this->EnumerateCallback(err); });
  }

  void Get(const std::string_view& field,
           const CommandManager::Response& response) {
    base::Tokenizer tokenizer(field, ".");
    auto group = tokenizer.next();
    const auto element_it = elements_.find(group);
    if (element_it == elements_.end()) {
      WriteMessage("ERR unknown group\r\n", response);
    } else {
      current_response_ = response;
      auto& element = element_it->second;
      const int err =
          element.serializable->Read(
              tokenizer.remaining(),
              send_buffer_,
              *current_response_.stream,
              [this](error_code error) {
                if (error) {
                  this->current_response_.callback(error);
                  return;
                }
                WriteMessage("\r\n",
                             this->current_response_);
              });
      if (err) {
        WriteMessage("ERR error reading\r\n", response);
      }
    }
  }

  void Set(const std::string_view& command,
           const CommandManager::Response& response) {
    base::Tokenizer tokenizer(command, ".");
    auto group = tokenizer.next();
    const auto element_it = elements_.find(group);
    if (element_it == elements_.end()) {
      WriteMessage("ERR unknown group\r\n", response);
    } else {
      base::Tokenizer name_value(tokenizer.remaining(), " ");
      auto key = name_value.next();
      auto value = name_value.remaining();
      auto& element = element_it->second;
      const int result = element.serializable->Set(key, value);
      if (result == 0) {
        element.updated();
        WriteOK(response);
      } else {
        WriteMessage("ERR error setting\r\n", response);
      }
    }
  }

  void Load(const CommandManager::Response& response) {
    DoLoad();

    WriteOK(response);
  }

  void DoLoad() {
    auto info = flash_.GetInfo();
    base::BufferReadStream flash_stream(
        std::string_view(info.start, info.end - info.start));
    telemetry::ReadStream stream(flash_stream);

    while (true) {
      const auto maybe_name_size = stream.ReadVaruint();
      if (!maybe_name_size) {
        // Whoops, an error of some sort.
        break;
      }
      const auto name_size = *maybe_name_size;
      typedef telemetry::Format TF;
      if (name_size == 0 ||
          name_size >= static_cast<uint32_t>(TF::kMaxStringSize)) {
        break;
      }
      std::string_view name(flash_stream.position(), name_size);
      flash_stream.ignore(name_size);

      if (flash_stream.remaining() < 8) { break; }

      const auto maybe_expected_crc = stream.Read<uint32_t>();
      const auto maybe_data_size = stream.Read<uint32_t>();

      if (!maybe_expected_crc || !maybe_data_size) { break; }
      const auto expected_crc = *maybe_expected_crc;
      const auto data_size = *maybe_data_size;

      // We are now committed to reading the entirety of the data one
      // way or another.

      const auto element_it = elements_.find(name);
      if (element_it == elements_.end()) {
        // TODO jpieper: It would be nice to warn about situations
        // like this.
        flash_stream.ignore(data_size);
        continue;
      }

      auto& element = element_it->second;

      const uint32_t actual_crc = CalculateSchemaCrc(element.serializable);
      if (actual_crc != expected_crc) {
        // TODO jpieper: It would be nice to warn about situations like
        // this.
        flash_stream.ignore(data_size);
        continue;
      }

      if (element.serializable->ReadBinary(flash_stream)) {
        // It would be nice to let the caller know this failed.
      }
    }

    // Notify everyone that they have changed.
    for (auto& element: elements_) {
      element.second.updated();
    }
  }

  uint32_t CalculateSchemaCrc(SerializableHandlerBase* base) const {
    char schema_buffer[2048] = {};
    base::BufferWriteStream schema_stream(
        {schema_buffer, sizeof(schema_buffer)});
    base->WriteSchema(schema_stream);

    // Calculate CRC of schema.
    const uint32_t crc =
        base::CalculateCrc(
            std::string_view(schema_buffer, schema_stream.offset()));

    return crc;
  }

  void Write(const CommandManager::Response& response) {
    auto info = flash_.GetInfo();
    flash_.Unlock();
    flash_.Erase();
    FlashWriteStream flash_stream(flash_, info.start);
    telemetry::WriteStream stream(flash_stream);

    for (const auto& item_pair : elements_) {
      const auto& element = item_pair.second;

      stream.WriteString(item_pair.first);
      stream.Write(static_cast<uint32_t>(
                       CalculateSchemaCrc(element.serializable)));

      SizeCountingStream size_stream;
      element.serializable->WriteBinary(size_stream);
      const uint32_t data_size = size_stream.size();

      stream.Write(data_size);
      element.serializable->WriteBinary(flash_stream);
    }

    stream.Write(static_cast<uint32_t>(0));

    flash_.Lock();

    WriteOK(response);
  }

  void Default(const CommandManager::Response& response) {
    for (auto& item_pair : elements_) {
      item_pair.second.serializable->SetDefault();
    }
    WriteOK(response);
  }

  void WriteOK(const CommandManager::Response& response) {
    WriteMessage("OK\r\n", response);
  }

  void UnknownCommand(const std::string_view&,
                      const CommandManager::Response& response) {
    WriteMessage("ERR unknown subcommand\r\n", response);
  }

  void WriteMessage(const std::string_view& message,
                    const CommandManager::Response& response) {
    AsyncWrite(*response.stream, message, response.callback);
  }

  Pool& pool_;
  FlashInterface& flash_;

  ElementMap elements_;

  // TODO jpieper: This buffer could be shared with other things that
  // have the same output stream, as only one should be writing at a
  // time anyways.
  char send_buffer_[256] = {};

  CommandManager::Response current_response_;
  std::size_t current_enumerate_index_ = 0;
  detail::EnumerateArchive::Context enumerate_context_;
};

PersistentConfig::PersistentConfig(
    Pool& pool, CommandManager& command_manager, FlashInterface& flash)
    : impl_(&pool, pool, command_manager, flash) {
}

PersistentConfig::~PersistentConfig() {
}

void PersistentConfig::Load() {
  impl_->DoLoad();
}

void PersistentConfig::RegisterDetail(
    const std::string_view& name, SerializableHandlerBase* base,
    base::inplace_function<void ()> updated) {
  Impl::Element element;
  element.serializable = base;
  element.updated = updated;

  const auto result = impl_->elements_.insert({name, element});
  // We do not allow duplicate names.
  MJ_ASSERT(result.second == true);
}

Pool* PersistentConfig::pool() const { return &impl_->pool_; }

}
}
