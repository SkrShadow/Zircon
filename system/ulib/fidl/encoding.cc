// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fidl/coding.h>
#include <lib/fidl/envelope_frames.h>
#include <lib/fidl/internal.h>
#include <lib/fidl/visitor.h>
#include <lib/fidl/walker.h>
#include <lib/fit/variant.h>
#include <stdalign.h>
#include <zircon/assert.h>
#include <zircon/compiler.h>

#include <cstdint>
#include <cstdlib>
#include <limits>

#ifdef __Fuchsia__
#include <zircon/syscalls.h>
#endif

namespace {

struct Position;

struct StartingPoint {
  // Pointer to the input object.
  void* const source;
  // The starting address of a contiguous destination buffer.
  uint8_t* const dest;
  Position ToPosition() const;
};

struct Position {
  // |source_object| points to one of the objects from the source pile.
  void* source_object;
  // |offset| is an offset into the destination buffer.
  uint32_t dest_offset;
  Position operator+(uint32_t size) const {
    return Position{
        .source_object = reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(source_object) + size),
        .dest_offset = dest_offset + size,
    };
  }
  Position& operator+=(uint32_t size) {
    *this = *this + size;
    return *this;
  }
  // By default, return the pointer to the destination buffer
  template <typename T>
  constexpr T* Get(StartingPoint start) const {
    return GetFromDest<T>(start.dest);
  }
  template <typename T>
  constexpr T* GetFromDest(uint8_t* dest_bytes) const {
    return reinterpret_cast<T*>(dest_bytes + dest_offset);
  }
  // Additional method to get a pointer to one of the source objects
  template <typename T>
  constexpr T* GetFromSource() const {
    return reinterpret_cast<T*>(source_object);
  }
};

Position StartingPoint::ToPosition() const {
  return Position{.source_object = source, .dest_offset = 0};
}

using EnvelopeState = ::fidl::EnvelopeFrames::EnvelopeState;

enum class Mode { EncodeOnly, LinearizeAndEncode };

template <Mode mode>
class FidlEncoder final
    : public fidl::Visitor<fidl::MutatingVisitorTrait, StartingPoint, Position> {
 public:
  FidlEncoder(void* bytes, uint32_t num_bytes, zx_handle_t* handles, uint32_t num_handles,
              uint32_t next_out_of_line, const char** out_error_msg)
      : bytes_(static_cast<uint8_t*>(bytes)),
        num_bytes_(num_bytes),
        num_handles_(num_handles),
        next_out_of_line_(next_out_of_line),
        out_error_msg_(out_error_msg) {
    if (handles != nullptr) {
      handles_ = handles;
    }
  }

  FidlEncoder(void* bytes, uint32_t num_bytes, zx_handle_disposition_t* handle_dispositions,
              uint32_t num_handle_dispositions, uint32_t next_out_of_line,
              const char** out_error_msg)
      : bytes_(static_cast<uint8_t*>(bytes)),
        num_bytes_(num_bytes),
        num_handles_(num_handle_dispositions),
        next_out_of_line_(next_out_of_line),
        out_error_msg_(out_error_msg) {
    if (handle_dispositions != nullptr) {
      handles_ = handle_dispositions;
    }
  }

  using StartingPoint = StartingPoint;

  using Position = Position;

  static constexpr bool kContinueAfterConstraintViolation = true;

  // TODO(fxb/53258) The template parameter can be removed, reducing code size if this is a
  // function.
  static constexpr bool kAllowNonNullableCollectionsToBeAbsent = (mode == Mode::LinearizeAndEncode);

  Status VisitPointer(Position ptr_position, PointeeType pointee_type,
                      ObjectPointerPointer object_ptr_ptr, uint32_t inline_size,
                      Position* out_position) {
    // For pointers in types other than vectors and strings, the LSB is reserved to mark ownership
    // and may be set to 1 if the object is heap allocated. However, the original pointer has this
    // bit cleared. For vectors and strings, any value is accepted.
    auto object_ptr =
        pointee_type == PointeeType::kVector || pointee_type == PointeeType::kString
            ? *object_ptr_ptr
            : reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(*object_ptr_ptr) &
                                      ~fidl::internal::kNonArrayTrackingPtrOwnershipMask);

    uint32_t new_offset;
    if (!FidlAddOutOfLine(next_out_of_line_, inline_size, &new_offset)) {
      SetError("overflow updating out-of-line offset");
      return Status::kMemoryError;
    }
    if (new_offset > num_bytes_) {
      SetError("pointed offset exceeds buffer size");
      return Status::kConstraintViolationError;
    }

    if (mode == Mode::LinearizeAndEncode) {
      // Copy the pointee to the desired location in secondary storage
      memcpy(&bytes_[next_out_of_line_], object_ptr, inline_size);
    } else if (object_ptr != &bytes_[next_out_of_line_]) {
      SetError("noncontiguous out of line storage during encode");
      return Status::kMemoryError;
    }

    // Zero padding between out of line storage.
    memset(&bytes_[next_out_of_line_] + inline_size, 0,
           (new_offset - next_out_of_line_) - inline_size);

    // Validate that we have a UTF8 string.
    // TODO(fxb/52215): For strings, it would most likely be more efficient
    // to validate and copy at the same time.
    if (pointee_type == PointeeType::kString) {
      auto status =
          fidl_validate_string(reinterpret_cast<char*>(&bytes_[next_out_of_line_]), inline_size);
      if (status != ZX_OK) {
        SetError("encoder encountered invalid UTF8 string");
        return Status::kConstraintViolationError;
      }
    }

    // Instruct the walker to traverse the pointee afterwards.
    *out_position = Position{.source_object = object_ptr, .dest_offset = next_out_of_line_};

    next_out_of_line_ = new_offset;

    // Rewrite pointer as "present" placeholder
    *object_ptr_ptr = reinterpret_cast<void*>(FIDL_ALLOC_PRESENT);

    return Status::kSuccess;
  }

  Status VisitHandle(Position handle_position, HandlePointer dest_handle, zx_rights_t handle_rights,
                     zx_obj_type_t handle_subtype) {
    if (handle_idx_ == num_handles_) {
      SetError("message tried to encode too many handles");
      ThrowAwayHandle(dest_handle);
      return Status::kConstraintViolationError;
    }

    if (has_handles()) {
      handles()[handle_idx_] = *dest_handle;
    } else if (has_handle_dispositions()) {
      handle_dispositions()[handle_idx_] = zx_handle_disposition_t{
          .operation = ZX_HANDLE_OP_MOVE,
          .handle = *dest_handle,
          .type = handle_subtype,
          .rights = handle_rights,
          .result = ZX_OK,
      };
    } else {
      SetError("did not provide place to store handles");
      ThrowAwayHandle(dest_handle);
      return Status::kConstraintViolationError;
    }

    *dest_handle = FIDL_HANDLE_PRESENT;
    if (mode == Mode::LinearizeAndEncode) {
      *handle_position.GetFromSource<zx_handle_t>() = ZX_HANDLE_INVALID;
    }
    handle_idx_++;
    return Status::kSuccess;
  }

  Status VisitVectorOrStringCount(CountPointer ptr) {
    if (mode == Mode::LinearizeAndEncode) {
      // Clear the MSB that is used for storing ownership information for vectors and strings.
      // While this operation could be considered part of encoding, it is LLCPP specific so it
      // is done during linearization.
      *ptr &= ~fidl::internal::kVectorOwnershipMask;
    }
    return Status::kSuccess;
  }

  Status VisitInternalPadding(Position padding_position, uint32_t padding_length) {
    auto padding_ptr = padding_position.template GetFromDest<uint8_t>(bytes_);
    memset(padding_ptr, 0, padding_length);
    return Status::kSuccess;
  }

  Status EnterEnvelope(Position envelope_position, EnvelopePointer envelope,
                       const fidl_type_t* payload_type) {
    if (mode == Mode::EncodeOnly) {
      // Validate envelope data/bytes invariants
      if (envelope->data == nullptr && (envelope->num_bytes != 0 || envelope->num_handles != 0)) {
        SetError("Envelope has absent data pointer, yet has data and/or handles");
        return Status::kConstraintViolationError;
      }
      if (envelope->data != nullptr && envelope->num_bytes == 0) {
        SetError("Envelope has present data pointer, but zero byte count");
        return Status::kConstraintViolationError;
      }
      if (envelope->data != nullptr && envelope->num_handles > 0 && payload_type == nullptr) {
        // Since we do not know the shape of the objects in this envelope,
        // we cannot move the handles scattered in the message.
        SetError("Does not know how to encode for this ordinal");
        return Status::kConstraintViolationError;
      }
    }
    // Remember the current watermark of bytes and handles, so that after processing
    // the envelope, we can either:
    // - EncodeOnly: validate the claimed num_bytes/num_handles matches the reality.
    // - LinearizeAndEncode: write the num_bytes/num_handles to the output.
    if (!envelope_frames_.Push(EnvelopeState(next_out_of_line_, handle_idx_))) {
      SetError("Overly deep nested envelopes");
      return Status::kConstraintViolationError;
    }
    return Status::kSuccess;
  }

  Status LeaveEnvelope(Position envelope_position, EnvelopePointer envelope) {
    // Now that the envelope has been consumed, check the correctness of the envelope header.
    auto& starting_state = envelope_frames_.Pop();
    uint32_t num_bytes = next_out_of_line_ - starting_state.bytes_so_far;
    uint32_t num_handles = handle_idx_ - starting_state.handles_so_far;
    if (mode == Mode::LinearizeAndEncode) {
      // Write the num_bytes/num_handles.
      envelope->num_bytes = num_bytes;
      envelope->num_handles = num_handles;
    } else {
      // Validate the claimed num_bytes/num_handles.
      if (envelope->num_bytes != num_bytes) {
        SetError("Envelope num_bytes was mis-sized");
        return Status::kConstraintViolationError;
      }
      if (envelope->num_handles != num_handles) {
        SetError("Envelope num_handles was mis-sized");
        return Status::kConstraintViolationError;
      }
    }
    return Status::kSuccess;
  }

  void OnError(const char* error) { SetError(error); }

  zx_status_t status() const { return status_; }

  uint32_t num_out_handles() const { return handle_idx_; }
  uint32_t num_out_bytes() const { return next_out_of_line_; }

 private:
  void SetError(const char* error) {
    if (status_ == ZX_OK) {
      status_ = ZX_ERR_INVALID_ARGS;
      if (out_error_msg_ != nullptr) {
        *out_error_msg_ = error;
      }
    }
  }

  void ThrowAwayHandle(HandlePointer handle) {
#ifdef __Fuchsia__
    zx_handle_close(*handle);
#endif
    *handle = ZX_HANDLE_INVALID;
  }

  bool has_handles() const { return fit::holds_alternative<zx_handle_t*>(handles_); }
  bool has_handle_dispositions() const {
    return fit::holds_alternative<zx_handle_disposition_t*>(handles_);
  }
  zx_handle_t* handles() const { return fit::get<zx_handle_t*>(handles_); }
  zx_handle_disposition_t* handle_dispositions() const {
    return fit::get<zx_handle_disposition_t*>(handles_);
  }

  // Message state initialized in the constructor.
  uint8_t* const bytes_;
  const uint32_t num_bytes_;
  fit::variant<fit::monostate, zx_handle_t*, zx_handle_disposition_t*> handles_;
  const uint32_t num_handles_;
  uint32_t next_out_of_line_;
  const char** const out_error_msg_;

  // Encoder state
  zx_status_t status_ = ZX_OK;
  uint32_t handle_idx_ = 0;
  fidl::EnvelopeFrames envelope_frames_;
};

template <typename HandleType>
zx_status_t fidl_linearize_and_encode_impl(const fidl_type_t* type, void* value, uint8_t* out_bytes,
                                           uint32_t num_bytes, HandleType* out_handles,
                                           uint32_t num_handles, uint32_t* out_num_actual_bytes,
                                           uint32_t* out_num_actual_handles,
                                           const char** out_error_msg,
                                           void (*close_handles)(const HandleType*, uint32_t)) {
  auto set_error = [&out_error_msg](const char* msg) {
    if (out_error_msg)
      *out_error_msg = msg;
  };
  if (value == nullptr) {
    set_error("Cannot encode null value");
    return ZX_ERR_INVALID_ARGS;
  }
  if (out_bytes == nullptr) {
    set_error("Cannot encode to null byte array");
    return ZX_ERR_INVALID_ARGS;
  }
  if (!FidlIsAligned(reinterpret_cast<uint8_t*>(value))) {
    set_error("Value must be aligned to FIDL_ALIGNMENT");
    return ZX_ERR_INVALID_ARGS;
  }
  if (!FidlIsAligned(out_bytes)) {
    set_error("Bytes must be aligned to FIDL_ALIGNMENT");
    return ZX_ERR_INVALID_ARGS;
  }
  if (num_bytes % FIDL_ALIGNMENT != 0) {
    set_error("num_bytes must be aligned to FIDL_ALIGNMENT");
    return ZX_ERR_INVALID_ARGS;
  }

  zx_status_t status;
  uint32_t next_out_of_line;
  if ((status = fidl::StartingOutOfLineOffset(type, num_bytes, &next_out_of_line, out_error_msg)) !=
      ZX_OK) {
    return status;
  }

  // Zero region between primary object and next out of line object.
  size_t primary_size;
  if ((status = fidl::PrimaryObjectSize(type, &primary_size, out_error_msg)) != ZX_OK) {
    return status;
  }

  // Copy the primary object
  memcpy(out_bytes, value, primary_size);

  // Zero the padding gaps
  memset(out_bytes + primary_size, 0, next_out_of_line - primary_size);

  FidlEncoder<Mode::LinearizeAndEncode> encoder(out_bytes, num_bytes, out_handles, num_handles,
                                                next_out_of_line, out_error_msg);
  fidl::Walk(encoder, type, StartingPoint{.source = value, .dest = out_bytes});

  auto drop_all_handles = [&]() {
    if (out_num_actual_handles) {
      *out_num_actual_handles = 0;
    }
    close_handles(out_handles, encoder.num_out_handles());
  };

  if (encoder.status() == ZX_OK) {
    if (out_num_actual_bytes == nullptr) {
      set_error("Cannot encode with null out_actual_bytes");
      drop_all_handles();
      return ZX_ERR_INVALID_ARGS;
    }
    if (out_num_actual_handles == nullptr) {
      set_error("Cannot encode with null out_actual_handles");
      drop_all_handles();
      return ZX_ERR_INVALID_ARGS;
    }
    *out_num_actual_bytes = encoder.num_out_bytes();
    *out_num_actual_handles = encoder.num_out_handles();
  } else {
    drop_all_handles();
  }

  if (out_handles == nullptr && num_handles != 0) {
    set_error("Cannot provide non-zero handle count and null handle pointer");
    // When |handles| is nullptr, handles are closed as part of traversal.
    return ZX_ERR_INVALID_ARGS;
  }

  return encoder.status();
}

template <typename HandleType>
zx_status_t fidl_encode_impl(const fidl_type_t* type, void* bytes, uint32_t num_bytes,
                             HandleType* handles, uint32_t max_handles,
                             uint32_t* out_actual_handles, const char** out_error_msg,
                             void (*close_handles)(const HandleType*, uint32_t)) {
  auto set_error = [&out_error_msg](const char* msg) {
    if (out_error_msg)
      *out_error_msg = msg;
  };
  if (bytes == nullptr) {
    set_error("Cannot encode null bytes");
    return ZX_ERR_INVALID_ARGS;
  }
  if (!FidlIsAligned(reinterpret_cast<uint8_t*>(bytes))) {
    set_error("Bytes must be aligned to FIDL_ALIGNMENT");
    return ZX_ERR_INVALID_ARGS;
  }
  if (num_bytes % FIDL_ALIGNMENT != 0) {
    set_error("num_bytes must be aligned to FIDL_ALIGNMENT");
    return ZX_ERR_INVALID_ARGS;
  }

  zx_status_t status;
  uint32_t next_out_of_line;
  if ((status = fidl::StartingOutOfLineOffset(type, num_bytes, &next_out_of_line, out_error_msg)) !=
      ZX_OK) {
    return status;
  }

  // Zero region between primary object and next out of line object.
  size_t primary_size;
  if ((status = fidl::PrimaryObjectSize(type, &primary_size, out_error_msg)) != ZX_OK) {
    return status;
  }
  memset(reinterpret_cast<uint8_t*>(bytes) + primary_size, 0, next_out_of_line - primary_size);

  FidlEncoder<Mode::EncodeOnly> encoder(bytes, num_bytes, handles, max_handles, next_out_of_line,
                                        out_error_msg);
  fidl::Walk(encoder, type,
             StartingPoint{.source = bytes, .dest = reinterpret_cast<uint8_t*>(bytes)});

  auto drop_all_handles = [&]() {
    if (out_actual_handles) {
      *out_actual_handles = 0;
    }
    close_handles(handles, encoder.num_out_handles());
  };

  if (encoder.status() == ZX_OK) {
    if (encoder.num_out_bytes() != num_bytes) {
      set_error("message did not encode all provided bytes");
      drop_all_handles();
      return ZX_ERR_INVALID_ARGS;
    }
    if (out_actual_handles == nullptr) {
      set_error("Cannot encode with null out_actual_handles");
      drop_all_handles();
      return ZX_ERR_INVALID_ARGS;
    }
    *out_actual_handles = encoder.num_out_handles();
  } else {
    drop_all_handles();
  }

  if (handles == nullptr && max_handles != 0) {
    set_error("Cannot provide non-zero handle count and null handle pointer");
    // When |handles| is nullptr, handles are closed as part of traversal.
    return ZX_ERR_INVALID_ARGS;
  }

  return encoder.status();
}

void close_handles_op(const zx_handle_t* handles, uint32_t max_idx) {
#ifdef __Fuchsia__
  if (handles) {
    // Return value intentionally ignored. This is best-effort cleanup.
    zx_handle_close_many(handles, max_idx);
  }
#endif
}

void close_handle_dispositions_op(const zx_handle_disposition_t* handle_dispositions,
                                  uint32_t max_idx) {
#ifdef __Fuchsia__
  if (handle_dispositions) {
    zx_handle_t* handles = reinterpret_cast<zx_handle_t*>(alloca(sizeof(zx_handle_t) * max_idx));
    for (uint32_t i = 0; i < max_idx; i++) {
      handles[i] = handle_dispositions[i].handle;
    }
    // Return value intentionally ignored. This is best-effort cleanup.
    zx_handle_close_many(handles, max_idx);
  }
#endif
}

}  // namespace

zx_status_t fidl_encode(const fidl_type_t* type, void* bytes, uint32_t num_bytes,
                        zx_handle_t* handles, uint32_t max_handles, uint32_t* out_actual_handles,
                        const char** out_error_msg) {
  return fidl_encode_impl(type, bytes, num_bytes, handles, max_handles, out_actual_handles,
                          out_error_msg, close_handles_op);
}

zx_status_t fidl_encode_etc(const fidl_type_t* type, void* bytes, uint32_t num_bytes,
                            zx_handle_disposition_t* handle_dispositions,
                            uint32_t max_handle_dispositions, uint32_t* out_actual_handles,
                            const char** out_error_msg) {
  return fidl_encode_impl(type, bytes, num_bytes, handle_dispositions, max_handle_dispositions,
                          out_actual_handles, out_error_msg, close_handle_dispositions_op);
}

zx_status_t fidl_encode_msg(const fidl_type_t* type, fidl_msg_t* msg, uint32_t* out_actual_handles,
                            const char** out_error_msg) {
  return fidl_encode(type, msg->bytes, msg->num_bytes, msg->handles, msg->num_handles,
                     out_actual_handles, out_error_msg);
}

zx_status_t fidl_linearize_and_encode(const fidl_type_t* type, void* value, uint8_t* out_bytes,
                                      uint32_t num_bytes, zx_handle_t* out_handles,
                                      uint32_t num_handles, uint32_t* out_num_actual_bytes,
                                      uint32_t* out_num_actual_handles,
                                      const char** out_error_msg) {
  return fidl_linearize_and_encode_impl(type, value, out_bytes, num_bytes, out_handles, num_handles,
                                        out_num_actual_bytes, out_num_actual_handles, out_error_msg,
                                        close_handles_op);
}
zx_status_t fidl_linearize_and_encode_etc(const fidl_type_t* type, void* value, uint8_t* out_bytes,
                                          uint32_t num_bytes, zx_handle_disposition_t* out_handles,
                                          uint32_t num_handles, uint32_t* out_num_actual_bytes,
                                          uint32_t* out_num_actual_handles,
                                          const char** out_error_msg) {
  return fidl_linearize_and_encode_impl(type, value, out_bytes, num_bytes, out_handles, num_handles,
                                        out_num_actual_bytes, out_num_actual_handles, out_error_msg,
                                        close_handle_dispositions_op);
}
zx_status_t fidl_linearize_and_encode_msg(const fidl_type_t* type, void* value, fidl_msg_t* msg,
                                          uint32_t* out_num_actual_bytes,
                                          uint32_t* out_num_actual_handles,
                                          const char** out_error_msg) {
  return fidl_linearize_and_encode(type, value, reinterpret_cast<uint8_t*>(msg->bytes),
                                   msg->num_bytes, msg->handles, msg->num_handles,
                                   out_num_actual_bytes, out_num_actual_handles, out_error_msg);
}
