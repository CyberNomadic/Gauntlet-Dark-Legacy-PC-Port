#pragma once

#include <string>

/** The arithmetic as it behaves in this process: the x86-64 profile, the rounding,
 * denormal and exception modes, and the bits of a fixed corpus of the operations the
 * simulation uses (gdl::portable's functions, square roots, remainders, the basic
 * operations and glm's algebra). Two processes whose witnesses agree compute the
 * simulation alike; the corpus is bounded evidence, not a proof for every input. */
namespace gdl::game::numeric_witness {

/** Throws std::runtime_error unless the modes the witness assumes are in force: round
 * to nearest, gradual underflow, every exception masked and, on Windows, the UCRT's
 * FMA3 math path disabled. */
void requireModes();

/** Once per process, before any worker thread: on Windows disables the UCRT's
 * CPU-dependent FMA3 math path; throws std::runtime_error when that fails. */
void prepareProcess();

/** SHA-256 hex of the witness; throws std::runtime_error when the modes are wrong or
 * the corpus produces a nonfinite result. The same on every supported platform. */
std::string digest();

} // namespace gdl::game::numeric_witness
