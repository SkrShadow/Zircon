// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FIDL_BIND_H_
#define LIB_FIDL_BIND_H_

#include <lib/async/dispatcher.h>
#include <zircon/fidl.h>

__BEGIN_CDECLS

// A generic FIDL dispatch function.
//
// For FIDL interfaces with [Layout="Simple"], the C backend generates a
// dispatch function that decodes the |msg| and calls through an |ops| table.
//
// This function signature matches the structure of these generated functions
// but with the type of the |ops| table erased.
//
// Example:
//
//  fidl_bind(dispatcher, channel, (fidl_dispatch_t*)spaceship_SpaceShip_dispatch, ctx, &kOps);
//
typedef zx_status_t(fidl_dispatch_t)(void* ctx, fidl_txn_t* txn, fidl_msg_t* msg, const void* ops);

// Binds a |dispatch| function to |channel| using |dispatcher|.
//
// This function adds an |async_wait_t| to the given |dispatcher| that waits
// asynchronously for new messages to arrive on |channel|. When a message
// arrives, the |dispatch| function is called on one of the threads associated
// with the |dispatcher| with the |fidl_msg_t| as well as the given |ctx| and
// |ops|.
//
// Typically, the |dispatch| function is generated by the C backend for FIDL
// interfaces with [Layout="Simple"] (see |fidl_dispatch_t|). These dispatch
// functions decode the |fidl_msg_t| and call through the |ops| table
// implementations of the interface's methods, passing along the |ctx| and a
// |fidl_txn_t| (if the method has a reply message).
//
// The |fidl_txn_t| passed to |dispatch| is valid only until |dispatch| returns.
// If the method has a reply message, the |reply| function on the |fidl_txn_t|
// object must be called synchronously within the |dispatch| call.
//
// If a client wishes to reply to the message asynchronously, |fidl_async_txn_create|
// must be invoked on |fidl_txn_t|, and ZX_ERR_ASYNC must be returned.
//
// Returns whether |fidl_bind| was able to begin waiting on the given |channel|.
// Upon any error, |channel| is closed and the binding is terminated.
//
// The |dispatcher| takes ownership of the channel. Shutting down the |dispatcher|
// results in |channel| being closed.
//
// It is safe to shutdown the dispatcher at any time.
//
// It is unsafe to destroy the dispatcher from within a dispatch function.
// It is unsafe to destroy the dispatcher while any |fidl_async_txn_t| objects
// are alive.
zx_status_t fidl_bind(async_dispatcher_t* dispatcher, zx_handle_t channel,
                      fidl_dispatch_t* dispatch, void* ctx, const void* ops);

// An asynchronous FIDL txn.
//
// This is an opaque wrapper around |fidl_txn_t| which can extend the lifetime
// of the object beyond the dispatched function.
typedef struct fidl_async_txn fidl_async_txn_t;

// Takes ownership of |txn| and allows usage of the txn beyond the currently
// dispatched function.
//
// If this function is invoked within a dispatched function, that function
// must return ZX_ERR_ASYNC.
//
// The result must be destroyed with a call to |fidl_async_txn_complete|.
fidl_async_txn_t* fidl_async_txn_create(fidl_txn_t* txn);

// Acquire a reference to the |fidl_txn_t| backing this txn object.
//
// It is unsafe to use this |fidl_txn_t| after |async_txn| is completed.
fidl_txn_t* fidl_async_txn_borrow(fidl_async_txn_t* async_txn);

// Destroys an asynchronous transaction created with |fidl_async_txn_create|.
// This function is intented to be the single termination function.
//
// If requested, rebinds the underlying txn against the binding.
// Returns an error if |rebind| is true and the transaction could not be
// re-bound.
//
// In all cases, the |async_txn| object is consumed.
//
// Note: Regardless of whether the client wants to rebind the txn or not,
// `fidl_async_txn_complete` should be the final function invoked. As such, if
// this function fails to rebind, there isn't much we can do with the
// |async_txn| beyond that point, and we therefore prefer to consume it in all
// cases.
zx_status_t fidl_async_txn_complete(fidl_async_txn_t* async_txn, bool rebind);

__END_CDECLS

#endif  // LIB_FIDL_BIND_H_
