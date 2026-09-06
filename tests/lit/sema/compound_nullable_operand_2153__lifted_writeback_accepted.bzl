// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

// CHECK: compound assignment `+=` is not defined when the

// #2153 (§6.6 / §7.11 / §5.1, spec v0.220) — REJECTION override
// (#3298 / v0.267 / proposal 0114): the previously-admitted lifted
// form `int? += long` is rejected at the §6.6 gate (§7.11 lifting
// gone), as are all the other §6.6 compound forms in this file.
// The narrower write-back family (byte? += 100, char? += 1, …) used
// to compile because §7.11 lifted to T? then §6.6 narrowed via the
// checked-narrow-integer write-back; both rules are off now.
//
// fix #3830 (v0.288): every operand below is now a PARAMETER rather than a
// literal-initialized local. §7.4 narrows a local at declaration when its
// initializer provably proves non-null (an int/float/char/decimal literal,
// or a `new`-typed non-nullable expression) — every local this file used to
// declare qualified — and #3830 makes narrowing reach compound assignment.
// As LOCALS, this row's whole premise flips: `int` widens to `long`/
// `decimal` (§5.1), so once narrowing removes the ONLY objection, every one
// of these six statements is an ordinary admitted compound form — `d += i`
// included (`int` widens to `decimal`; only `decimal`/binary-float have no
// conversion either way, and `i` here is `int`, not `double`). Verified
// empirically: with locals in place of the parameters below, the WHOLE
// program compiles clean (no diagnostic at all) and computes `a`=15,
// `b`=25, `m`=8 (12 & 10), `d`=3.5, `c`='B' (66), `y`=101 — this row's
// original name ("accepted") is literally true again. A PARAMETER is never
// narrowed by what the caller passes — narrowing requires an in-body null
// test (§7.4) — so this restores the row's original genuinely-nullable,
// unguarded coverage of the REJECTION side. The narrowed-ACCEPTED side of
// `a`/`b`/`m`/`c`/`y` is pinned with asserted VALUES by
// codegen_nullable_property_compound_2888.local_increment_narrowed_accepted_3830
// and .checked_writeback_narrowed_accepted_3830
// (tests/codegen/nullable_property_compound_2888_test.cpp).

module M;
class C
  public static method Chk(a: int?, b: int?, w: long?, m: int?, n: long?,
                            d: decimal?, i: int?, c: char?, y: byte?): int
    a += 10L;
    b += w;
    m &= n;
    d += i;
    c += 1;
    y += 1;
    return 0;
  end Chk;
  public static method Main(): int
    return Chk(5, 5, 20L, 12, 10L, 1.5m, 2, char(65), 100);
  end Main;
end C;
