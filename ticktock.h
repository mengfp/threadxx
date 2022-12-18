#pragma once

#ifdef _WIN32
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

class ticktock {
 private:
  uint64_t& t;
  uint64_t t0;

 public:
  ticktock(uint64_t& t) : t(t), t0(__rdtsc()) {
  }

  ~ticktock() {
    t += __rdtsc() - t0;
  }
};
