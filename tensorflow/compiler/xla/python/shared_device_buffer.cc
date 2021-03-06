/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/python/shared_device_buffer.h"

#include <iterator>
#include <memory>

#include "absl/synchronization/mutex.h"
#include "tensorflow/compiler/xla/python/local_device_state.h"
#include "tensorflow/compiler/xla/service/shaped_buffer.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/stream_executor/device_memory.h"
#include "tensorflow/stream_executor/device_memory_allocator.h"
#include "tensorflow/stream_executor/event.h"
#include "tensorflow/stream_executor/stream.h"

namespace xla {

void BufferDefinitionEvent::SetDefinitionEvent(EventPool::Handle event,
                                               se::Stream* stream) {
  absl::MutexLock lock(&mu_);
  CHECK(!event_.event());
  event_ = std::move(event);
  CHECK(streams_defined_on_.empty());
  streams_defined_on_.push_back(stream);
}

bool BufferDefinitionEvent::EventHasBeenRecorded() const {
  return event_.event() != nullptr;
}

uint64 BufferDefinitionEvent::sequence_number() const {
  absl::MutexLock lock(&mu_);
  CHECK(EventHasBeenRecorded());
  return event_.sequence_number();
}

void BufferDefinitionEvent::WaitForEventOnStream(se::Stream* stream) {
  absl::MutexLock lock(&mu_);

  // We cannot wait for an event until ThenRecordEvent has been called; on GPU
  // newly created events are deemed to have already happened past.
  mu_.Await(
      absl::Condition(this, &BufferDefinitionEvent::EventHasBeenRecorded));

  // The set of defined streams is expected to be very small indeed (usually
  // 1-2), so a simple linear scan should be fast enough.
  if (std::find(streams_defined_on_.begin(), streams_defined_on_.end(),
                stream) != streams_defined_on_.end()) {
    // stream is in streams_defined_on_; it doesn't need to be waited on.
    return;
  }

  stream->ThenWaitFor(event_.event());
  streams_defined_on_.push_back(stream);
}

bool BufferDefinitionEvent::DefinedOn(se::Stream* stream) {
  absl::MutexLock lock(&mu_);

  // We cannot wait for an event until ThenRecordEvent has been called; on GPU
  // newly created events are deemed to have already happened past.
  mu_.Await(
      absl::Condition(this, &BufferDefinitionEvent::EventHasBeenRecorded));

  // The set of defined streams is expected to be very small indeed (usually
  // 1-2), so a simple linear scan should be fast enough.
  return std::find(streams_defined_on_.begin(), streams_defined_on_.end(),
                   stream) != streams_defined_on_.end();
}

bool BufferDefinitionEvent::IsComplete() {
  absl::MutexLock lock(&mu_);

  // We cannot wait for an event until ThenRecordEvent has been called; on
  // GPU newly created events are deemed to have already happened past.
  mu_.Await(
      absl::Condition(this, &BufferDefinitionEvent::EventHasBeenRecorded));

  return event_.event()->PollForStatus() == se::Event::Status::kComplete;
}

/* static */ std::shared_ptr<SharedDeviceBuffer>
SharedDeviceBuffer::FromScopedShapedBuffer(
    ScopedShapedBuffer* shaped_buffer,
    absl::Span<const std::shared_ptr<BufferDefinitionEvent>>
        definition_events) {
  ShapeTree<se::DeviceMemoryBase>::iterator iterator =
      shaped_buffer->buffers().begin();
  std::vector<se::DeviceMemoryBase> buffers;
  buffers.reserve(1);

  ShapeUtil::ForEachSubshape(
      shaped_buffer->on_device_shape(), [&](const Shape&, const ShapeIndex&) {
        CHECK(iterator != shaped_buffer->buffers().end());
        buffers.push_back(iterator->second);
        iterator->second = se::DeviceMemoryBase();
        ++iterator;
      });
  CHECK(iterator == shaped_buffer->buffers().end());
  return std::make_shared<SharedDeviceBuffer>(
      shaped_buffer->memory_allocator(), shaped_buffer->device_ordinal(),
      absl::Span<se::DeviceMemoryBase>(buffers), definition_events,
      /*on_delete_callback=*/nullptr);
}

ShapedBuffer SharedDeviceBuffer::AsShapedBuffer(const Shape& on_host_shape,
                                                const Shape& on_device_shape,
                                                se::Platform* platform) const {
  ShapedBuffer shaped_buffer(on_host_shape, on_device_shape, platform,
                             device_ordinal_);
  ShapeTree<se::DeviceMemoryBase>::iterator iterator =
      shaped_buffer.buffers().begin();
  for (const se::DeviceMemoryBase& buf : device_memory_) {
    CHECK(iterator != shaped_buffer.buffers().end());
    iterator->second = buf;
    ++iterator;
  }
  CHECK(iterator == shaped_buffer.buffers().end());
  return shaped_buffer;
}

namespace {

using MoveIterator =
    absl::Span<const std::shared_ptr<BufferDefinitionEvent>>::iterator;

}  // namespace

SharedDeviceBuffer::ScopedUsage::~ScopedUsage() {
  if (parent_ != nullptr) {
    parent_->DropUsageHold();
  }
}

SharedDeviceBuffer::ScopedUsage& SharedDeviceBuffer::ScopedUsage::Acquire(
    std::shared_ptr<SharedDeviceBuffer> parent) {
  CHECK(parent_ == nullptr);
  if (parent != nullptr) {
    parent_ = std::move(parent);
    parent_->AddUsageHold();
  }
  return *this;
}

std::shared_ptr<SharedDeviceBuffer> SharedDeviceBuffer::ScopedUsage::Release() {
  return std::move(parent_);
}

void SharedDeviceBuffer::ScopedUsage::Transfer(
    std::shared_ptr<SharedDeviceBuffer> parent) {
  CHECK(parent_ == nullptr);
  parent_ = parent;
}

void SharedDeviceBuffer::ScopedUsage::Convert(
    se::Stream* usage_stream, std::shared_ptr<BufferDefinitionEvent> event,
    bool reference_held) {
  CHECK(parent_ != nullptr);
  parent_->ConvertUsageHold(usage_stream, std::move(event), reference_held);
  parent_ = nullptr;
}

SharedDeviceBuffer::SharedDeviceBuffer(
    se::DeviceMemoryAllocator* allocator, int device_ordinal,
    absl::Span<se::DeviceMemoryBase const> device_memory,
    absl::Span<const std::shared_ptr<BufferDefinitionEvent>> definition_events,
    std::function<void()> on_delete_callback)
    : allocator_(allocator),
      device_ordinal_(device_ordinal),
      device_memory_(device_memory.begin(), device_memory.end()),
      definition_events_(
          std::move_iterator<MoveIterator>(definition_events.begin()),
          std::move_iterator<MoveIterator>(definition_events.end())),
      in_use_(true),
      usage_holds_(0),
      external_references_(0),
      on_delete_callback_(std::move(on_delete_callback)) {}

SharedDeviceBuffer::~SharedDeviceBuffer() {
  CHECK_EQ(external_references_, 0);
  if (allocator_) {
    for (const se::DeviceMemoryBase& buffer : device_memory_) {
      Status status = allocator_->Deallocate(device_ordinal_, buffer);
      if (!status.ok()) {
        LOG(ERROR) << "Buffer deallocation failed: " << status;
      }
    }
  }
  if (on_delete_callback_) {
    on_delete_callback_();
  }
}

void SharedDeviceBuffer::AddUsageHold() {
  absl::MutexLock lock(&mu_);
  CHECK(in_use_);
  ++usage_holds_;
}

void SharedDeviceBuffer::DropUsageHold() {
  absl::MutexLock lock(&mu_);
  CHECK(in_use_);
  CHECK_GT(usage_holds_, 0);
  --usage_holds_;
}

void SharedDeviceBuffer::AddExternalReference() {
  absl::MutexLock lock(&mu_);
  CHECK(in_use_);
  ++external_references_;
}

void SharedDeviceBuffer::DropExternalReference() {
  absl::MutexLock lock(&mu_);
  CHECK_GT(external_references_, 0);
  --external_references_;
}

void SharedDeviceBuffer::ConvertUsageHold(
    se::Stream* usage_stream, std::shared_ptr<BufferDefinitionEvent> event,
    bool reference_held) {
  absl::MutexLock lock(&mu_);
  CHECK(in_use_);
  CHECK_GT(usage_holds_, 0);
  --usage_holds_;

  for (auto& existing : usage_events_) {
    if (existing.stream == usage_stream) {
      if (*existing.event < *event) {
        existing.event = event;
        existing.reference_held = reference_held;
      }
      return;
    }
  }
  usage_events_.push_back({usage_stream, event, reference_held});
}

SharedDeviceBuffer::StreamAndEventContainer
SharedDeviceBuffer::LockUseAndTransferUsageEvents() {
  auto holds_converted = [&]() {
    mu_.AssertHeld();
    return usage_holds_ == 0;
  };
  absl::MutexLock lock(&mu_);
  CHECK(in_use_);
  mu_.Await(absl::Condition(&holds_converted));
  CHECK(in_use_);
  in_use_ = false;
  return std::move(usage_events_);
}

void GetDeviceBufferDefinitionEvents(
    const SharedDeviceBuffer& buffer,
    absl::flat_hash_set<BufferDefinitionEvent*>* events) {
  for (const auto& e : buffer.definition_events()) {
    events->insert(e.get());
  }
}

void WaitForBufferDefinitionEventsOnStream(const SharedDeviceBuffer& buffer,
                                           se::Stream* stream) {
  absl::flat_hash_set<BufferDefinitionEvent*> events;
  GetDeviceBufferDefinitionEvents(buffer, &events);
  for (BufferDefinitionEvent* event : events) {
    event->WaitForEventOnStream(stream);
  }
}

}  // namespace xla
