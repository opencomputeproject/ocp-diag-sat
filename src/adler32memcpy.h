// Copyright 2023 Google LLC
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

#ifndef STRESSAPPTEST_ADLER32MEMCPY_H_
#define STRESSAPPTEST_ADLER32MEMCPY_H_

#include <string>

#include "sattypes.h"

// Encapsulation for Adler checksum. Please see adler32memcpy.cc for more
// detail on the adler checksum algorithm.
class AdlerChecksum {
 public:
  AdlerChecksum() {}
  ~AdlerChecksum() {}
  // Returns true if the checksums are equal.
  bool Equals(const AdlerChecksum &other) const;
  // Returns string representation of the Adler checksum
  string ToHexString() const;
  // Sets components of the Adler checksum.
  void Set(uint64 a1, uint64 a2, uint64 b1, uint64 b2);

 private:
  // Components of Adler checksum.
  uint64 a1_, a2_, b1_, b2_;

  DISALLOW_COPY_AND_ASSIGN(AdlerChecksum);
};

// Calculates Adler checksum for supplied data.
bool CalculateAdlerChecksum(uint64 *data64, unsigned int size_in_bytes,
                            AdlerChecksum *checksum);

// C implementation of Adler memory copy.
bool AdlerMemcpyC(uint64 *dstmem64, uint64 *srcmem64,
                  unsigned int size_in_bytes, AdlerChecksum *checksum);

// C implementation of Adler memory copy with some float point ops,
// attempting to warm up the CPU.
bool AdlerMemcpyWarmC(uint64 *dstmem64, uint64 *srcmem64,
                      unsigned int size_in_bytes, AdlerChecksum *checksum);

// x86_64 SSE2 assembly implementation of fast and stressful Adler memory copy.
bool AdlerMemcpyAsm(uint64 *dstmem64, uint64 *srcmem64,
                    unsigned int size_in_bytes, AdlerChecksum *checksum);

#endif  // STRESSAPPTEST_ADLER32MEMCPY_H_
