// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// If this test is failing, regen goldens using zircon/tools/fidl/testdata/regen.sh.

#include <unittest/unittest.h>

#include "goldens.h"
#include "goldens_test.h"
#include "test_library.h"

namespace {

bool check_json_goldens() {
  BEGIN_TEST;

  auto result = check_goldens(Generator::kJson);

  // Add a sanity check that we have checked at least some number of goldens
  // so that the test doesn't silently pass if the goldens have moved and this
  // test doesn't find/test them
  ASSERT_GE(result.num_goldens, 10);
  ASSERT_FALSE(result.failed);

  END_TEST;
}

}  // namespace

BEGIN_TEST_CASE(json_generator_tests)
RUN_TEST(check_json_goldens)
END_TEST_CASE(json_generator_tests)
