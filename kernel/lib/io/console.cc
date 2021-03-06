// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2008-2015 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <assert.h>
#include <ctype.h>
#include <debug.h>
#include <err.h>
#include <lib/debuglog.h>
#include <lib/io.h>
#include <platform.h>
#include <string.h>

#include <arch/ops.h>
#include <kernel/auto_lock.h>
#include <kernel/thread.h>
#include <platform/debug.h>
#include <vm/vm.h>

/* routines for dealing with main console io */

static SpinLock dputc_spin_lock;

void __kernel_serial_write(const char* str, size_t len) {
  AutoSpinLock guard(&dputc_spin_lock);

  /* write out the serial port */
  platform_dputs_irq(str, len);
}

static SpinLock print_spin_lock;
static fbl::DoublyLinkedList<PrintCallback*> print_callbacks;

void __kernel_console_write(const char* str, size_t len) {
  // Print to any registered loggers.
  if (!print_callbacks.is_empty()) {
    AutoSpinLock guard(&print_spin_lock);

    for (PrintCallback& print_callback : print_callbacks) {
      print_callback.Print(str, len);
    }
  }
}

static void __kernel_stdout_write(const char* str, size_t len) {
  if (dlog_bypass() == false) {
    if (dlog_write(DEBUGLOG_INFO, 0, str, len) == ZX_OK)
      return;
  }
  __kernel_console_write(str, len);
  __kernel_serial_write(str, len);
}

#if WITH_DEBUG_LINEBUFFER
static void __kernel_stdout_write_buffered(const char* str, size_t len) {
  Thread* t = Thread::Current::Get();

  if (unlikely(t == NULL)) {
    __kernel_stdout_write(str, len);
    return;
  }

  char* buf = t->linebuffer_;
  size_t pos = t->linebuffer_pos_;

  // look for corruption and don't continue
  if (unlikely(!is_kernel_address((uintptr_t)buf) || pos >= THREAD_LINEBUFFER_LENGTH)) {
    const char* str = "<linebuffer corruption>\n";
    __kernel_stdout_write(str, strlen(str));
    return;
  }

  while (len-- > 0) {
    char c = *str++;
    buf[pos++] = c;
    if (c == '\n') {
      __kernel_stdout_write(buf, pos);
      pos = 0;
      continue;
    }
    if (pos == (THREAD_LINEBUFFER_LENGTH - 1)) {
      buf[pos++] = '\n';
      __kernel_stdout_write(buf, pos);
      pos = 0;
      continue;
    }
  }
  t->linebuffer_pos_ = pos;
}

static constexpr auto kStdoutWrite = __kernel_stdout_write_buffered;

#else  // !WITH_DEBUG_LINEBUFFER

static constexpr auto kStdoutWrite = __kernel_stdout_write;

#endif  // WITH_DEBUG_LINEBUFFER

void register_print_callback(PrintCallback* cb) {
  AutoSpinLock guard(&print_spin_lock);

  print_callbacks.push_front(cb);
}

void unregister_print_callback(PrintCallback* cb) {
  AutoSpinLock guard(&print_spin_lock);

  print_callbacks.erase(*cb);
}

// This is what printf calls.  Really this could and should be const.
// But all the stdio function signatures require non-const `FILE*`.
FILE FILE::stdout_{[](void*, ktl::string_view str) {
                     kStdoutWrite(str.data(), str.size());
                     return static_cast<int>(str.size());
                   },
                   nullptr};
