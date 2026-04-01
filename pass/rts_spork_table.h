/**
 * rts_spork_table.h
 *
 * Public interface for the static table built by RtsSporkPass.
 *
 * After linking any translation unit compiled with the pass, two symbols
 * are available:
 *
 *   __rts_spork_table      – array of RtsSporkSite, one entry per call site
 *   __rts_spork_table_size – number of entries in the array
 *
 * Each entry holds the signed byte offsets of the four pointer arguments of
 * the corresponding __RTS_record_spork call, measured from the frame base
 * (CFA / frame pointer) of the *calling* function.
 *
 * An offset value of INT64_MIN means the pass could not determine the real
 * offset (e.g. the argument was not a plain local variable).
 */

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Number of pointer arguments tracked per call site. */
#define RTS_SPORK_NARGS 4

/**
 * One entry per __RTS_record_spork call site.
 *
 * offsets[0] – stack offset of `promotable_flag`
 * offsets[1] – stack offset of `num_promotions`
 * offsets[2] – stack offset of `prom`
 * offsets[3] – stack offset of `exec_prom`
 *
 * All offsets are in bytes, signed, relative to the frame base of the
 * function that made the call.  A positive offset means the variable lives
 * *above* the frame base (typical on x86-64 when using %rbp).
 */
typedef struct {
    int64_t offsets[RTS_SPORK_NARGS];
} RtsSporkSite;

extern RtsSporkSite __rts_spork_table[];
extern const uint64_t __rts_spork_table_size;

/** Sentinel meaning "could not resolve this argument to a local variable". */
#define RTS_SPORK_UNKNOWN_OFFSET INT64_MIN

#ifdef __cplusplus
}
#endif
