/*
 *
 * Copyright 2017 gRPC authors.
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
 */

/* Test of gpr spin-lock support. */

#include "src/core/lib/gpr/spinlock.h"

#include <stdint.h>
#include <stdio.h>

#include "gtest/gtest.h"

#include <grpc/support/alloc.h>
#include <grpc/support/time.h>

#include "src/core/lib/gprpp/thd.h"
#include "test/core/util/test_config.h"

/* ------------------------------------------------- */
/* Tests for gpr_spinlock. */
struct test {
  int thread_count; /* number of threads */
  grpc_core::Thread* threads;

  int64_t iterations; /* number of iterations per thread */
  int64_t counter;
  int incr_step; /* how much to increment/decrement refcount each time */

  gpr_spinlock mu; /* protects iterations, counter */
};

/* Return pointer to a new struct test. */
static struct test* test_new(int threads, int64_t iterations, int incr_step) {
  struct test* m = static_cast<struct test*>(gpr_malloc(sizeof(*m)));
  m->thread_count = threads;
  m->threads = static_cast<grpc_core::Thread*>(
      gpr_malloc(sizeof(*m->threads) * static_cast<size_t>(threads)));
  m->iterations = iterations;
  m->counter = 0;
  m->thread_count = 0;
  m->incr_step = incr_step;
  m->mu = GPR_SPINLOCK_INITIALIZER;
  return m;
}

/* Return pointer to a new struct test. */
static void test_destroy(struct test* m) {
  gpr_free(m->threads);
  gpr_free(m);
}

/* Create m->threads threads, each running (*body)(m) */
static void test_create_threads(struct test* m, void (*body)(void* arg)) {
  int i;
  for (i = 0; i != m->thread_count; i++) {
    m->threads[i] = grpc_core::Thread("grpc_create_threads", body, m);
    m->threads[i].Start();
  }
}

/* Wait until all threads report done. */
static void test_wait(struct test* m) {
  int i;
  for (i = 0; i != m->thread_count; i++) {
    m->threads[i].Join();
  }
}

/* Test several threads running (*body)(struct test *m) for increasing settings
   of m->iterations, until about timeout_s to 2*timeout_s seconds have elapsed.
   If extra!=NULL, run (*extra)(m) in an additional thread.
   incr_step controls by how much m->refcount should be incremented/decremented
   (if at all) each time in the tests.
   */
static void test(void (*body)(void* m), int timeout_s, int incr_step) {
  int64_t iterations = 1024;
  struct test* m;
  gpr_timespec start = gpr_now(GPR_CLOCK_REALTIME);
  gpr_timespec time_taken;
  gpr_timespec deadline = gpr_time_add(
      start, gpr_time_from_micros(static_cast<int64_t>(timeout_s) * 1000000,
                                  GPR_TIMESPAN));
  while (gpr_time_cmp(gpr_now(GPR_CLOCK_REALTIME), deadline) < 0) {
    if (iterations < INT64_MAX / 2) iterations <<= 1;
    fprintf(stderr, " %ld", static_cast<long>(iterations));
    fflush(stderr);
    m = test_new(10, iterations, incr_step);
    test_create_threads(m, body);
    test_wait(m);
    if (m->counter != m->thread_count * m->iterations * m->incr_step) {
      fprintf(stderr, "counter %ld  threads %d  iterations %ld\n",
              static_cast<long>(m->counter), m->thread_count,
              static_cast<long>(m->iterations));
      fflush(stderr);
      FAIL();
    }
    test_destroy(m);
  }
  time_taken = gpr_time_sub(gpr_now(GPR_CLOCK_REALTIME), start);
  fprintf(stderr, " done %lld.%09d s\n",
          static_cast<long long>(time_taken.tv_sec),
          static_cast<int>(time_taken.tv_nsec));
  fflush(stderr);
}

/* Increment m->counter on each iteration; then mark thread as done.  */
static void inc(void* v /*=m*/) {
  struct test* m = static_cast<struct test*>(v);
  int64_t i;
  for (i = 0; i != m->iterations; i++) {
    gpr_spinlock_lock(&m->mu);
    m->counter++;
    gpr_spinlock_unlock(&m->mu);
  }
}

/* Increment m->counter under lock acquired with trylock, m->iterations times;
   then mark thread as done.  */
static void inctry(void* v /*=m*/) {
  struct test* m = static_cast<struct test*>(v);
  int64_t i;
  for (i = 0; i != m->iterations;) {
    if (gpr_spinlock_trylock(&m->mu)) {
      m->counter++;
      gpr_spinlock_unlock(&m->mu);
      i++;
    }
  }
}

/* ------------------------------------------------- */

TEST(SpinlockTest, Spinlock) { test(&inc, 1, 1); }

TEST(SpinlockTest, SpinlockTry) { test(&inctry, 1, 1); }

int main(int argc, char** argv) {
  grpc::testing::TestEnvironment env(&argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
