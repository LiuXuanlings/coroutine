#ifndef MINICYBER_COMMON_TYPES_H_
#define MINICYBER_COMMON_TYPES_H_

#include <cstdint>

namespace minicyber {

class NullType {};

// Return code definition for minicyber internal function return.
enum ReturnCode {
  SUCC = 0,
  FAIL = 1,
};

/**
 * @brief Describe relation between nodes, writers/readers...
 */
enum Relation : std::uint8_t {
  NO_RELATION = 0,
  DIFF_HOST,  // different host
  DIFF_PROC,  // same host, but different process
  SAME_PROC,  // same process
};

}  // namespace minicyber

#endif  // MINICYBER_COMMON_TYPES_H_
