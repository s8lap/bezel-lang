// Spec-derived tests for fix #1375 — `Array<T>(size: int, fill: T)` constructor.
//
// Authority: stdlib §4.4 v0.165 declares a two-argument constructor
//   public constructor(size: int, fill: T);   // Count = size, every slot = fill
// distinct from the one-argument `constructor(capacity: int)` which reserves
// storage WITHOUT populating elements (Count = 0). The sized-fill form is what
// makes the canonical read-into-preallocated-buffer loop of §7.1 writable
// (`new Array<byte>(64 * 1024, 0)` then `Read(buf, 0, buf.Count)`).
//
// Bezel syntax notes (this file uses valid Bezel throughout):
//   * inequality is `<>`, not `!=` (§17.3);
//   * there is no `Nb` byte-literal suffix — a byte value is written `byte(N)`
//     or, in the typed fill context, a bare in-range int literal (§4.2).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// A source that must be rejected at compile time (front-end error, no run).
bool rejects(const std::string& src) {
    auto r = runJit(src);
    return !r.ran && r.diagnostics.hasErrors();
}

}  // namespace

// ── Count = size (distinct from the capacity ctor's Count = 0) ────────────────

BEZEL_TEST(array_sized_fill_1375, int_count_equals_size) {
    // per stdlib §4.4 v0.165 (#1375)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<int>(4, 42); "
        "  return arr.Count; "
        "end Main; end C;"), 4);
}

BEZEL_TEST(array_sized_fill_1375, capacity_equals_size) {
    // per stdlib §4.4 v0.165 (#1375): storage for `size` slots is reserved too.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<int>(5, 1); "
        "  return arr.Capacity; "
        "end Main; end C;"), 5);
}

// The one-argument CAPACITY ctor stays distinct: Count = 0, Capacity = arg.
BEZEL_TEST(array_sized_fill_1375, capacity_ctor_still_empty) {
    // per stdlib §4.4 v0.99+ (capacity ctor) vs v0.165 (sized-fill ctor)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<int>(8); "
        "  return arr.Count * 100 + arr.Capacity; "  // 0*100 + 8
        "end Main; end C;"), 8);
}

// ── Every slot carries the fill value ─────────────────────────────────────────

BEZEL_TEST(array_sized_fill_1375, int_every_slot_filled) {
    // per stdlib §4.4 v0.165 (#1375)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<int>(4, 42); "
        "  if arr[0] <> 42 then return 1; end; "
        "  if arr[1] <> 42 then return 2; end; "
        "  if arr[2] <> 42 then return 3; end; "
        "  if arr[3] <> 42 then return 4; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

BEZEL_TEST(array_sized_fill_1375, int_negative_fill) {
    // per stdlib §4.4 v0.165 (#1375)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<int>(1, -7); "
        "  if arr.Count <> 1 then return 1; end; "
        "  if arr[0] <> -7 then return 2; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

BEZEL_TEST(array_sized_fill_1375, long_fill) {
    // per stdlib §4.4 v0.165 (#1375): value-typed T stored by copy (long).
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<long>(2, 5L); "
        "  return int(arr[0]) + int(arr[1]); "
        "end Main; end C;"), 10);
}

// ── byte fill: `byte(N)` cast form ────────────────────────────────────────────

BEZEL_TEST(array_sized_fill_1375, byte_fill_nonzero_via_cast) {
    // per stdlib §4.4 v0.165 (#1375)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<byte>(3, byte(255)); "
        "  if int(arr[0]) <> 255 then return 1; end; "
        "  if int(arr[1]) <> 255 then return 2; end; "
        "  if int(arr[2]) <> 255 then return 3; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// ── byte fill: bare in-range int literal (§4.2 typed-context narrowing) ────────
// This is the exact form of the §7.1 canonical read-loop `new Array<byte>(N, 0)`.

BEZEL_TEST(array_sized_fill_1375, byte_fill_bare_int_literal) {
    // per stdlib §4.4 / §7.1 v0.165 (#1375)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<byte>(4, 0); "        // canonical read-buffer form
        "  if int(arr[0]) <> 0 then return 1; end; "
        "  if int(arr[3]) <> 0 then return 2; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

BEZEL_TEST(array_sized_fill_1375, byte_fill_bare_int_literal_200) {
    // per stdlib §4.4 v0.165 (#1375): 200 fits byte, admitted at byte.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<byte>(2, 200); "
        "  return int(arr[0]) + int(arr[1]); "
        "end Main; end C;"), 400);
}

// The §7.1 canonical buffer size (64 KiB) fills correctly.
BEZEL_TEST(array_sized_fill_1375, large_buffer_canonical_read_loop_size) {
    // per stdlib §7.1 v0.165 (#1375)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new Array<byte>(64 * 1024, 0); "
        "  if buf.Count <> 65536 then return 1; end; "
        "  if int(buf[0]) <> 0 then return 2; end; "
        "  if int(buf[65535]) <> 0 then return 3; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// ── Edge sizes ────────────────────────────────────────────────────────────────

BEZEL_TEST(array_sized_fill_1375, size_zero_is_empty) {
    // per stdlib §4.4 v0.165 (#1375): size 0 is valid; Count = 0.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<int>(0, 99); "
        "  return arr.Count; "
        "end Main; end C;"), 0);
}

BEZEL_TEST(array_sized_fill_1375, size_one_single_element) {
    // per stdlib §4.4 v0.165 (#1375)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<int>(1, 7); "
        "  return arr.Count * 100 + arr[0]; "
        "end Main; end C;"), 107);
}

// A negative size raises ArgumentOutOfRangeException (stdlib §1/§4.4 v0.183,
// #1396 — the doc now defines the fault; this supersedes the earlier v0.165
// empty-fallback that treated a non-positive size like size 0).
BEZEL_TEST(array_sized_fill_1375, negative_size_raises_aoor_1396) {
    // stdlib §1/§4.4 (v0.183, #1396): a negative size now raises
    // ArgumentOutOfRangeException (supersedes the v0.165 empty-fallback this
    // test formerly asserted, which #1586 changed).
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  try "
        "    using var arr := new Array<int>(-1, 5); "
        "    return arr.Count; "
        "  catch ArgumentOutOfRangeException => return 42; end; "
        "end Main; end C;"), 42);
}

// ── Slots are ordinary array slots afterwards ─────────────────────────────────

BEZEL_TEST(array_sized_fill_1375, overwrite_after_fill) {
    // per stdlib §4.4 v0.165 (#1375)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<int>(3, 1); "
        "  arr[1] := 99; "
        "  if arr[0] <> 1  then return 1; end; "
        "  if arr[1] <> 99 then return 2; end; "
        "  if arr[2] <> 1  then return 3; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// ── Class-typed T: every slot borrows the same instance (§4.4 borrowing) ──────

BEZEL_TEST(array_sized_fill_1375, class_fill_borrows_instance) {
    // per stdlib §4.4 v0.165 (#1375): Array<T> stores borrows; a class fill
    // makes every slot borrow the passed instance.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class W public v: int; public constructor(x: int) v := x; end; end W; "
        "class C public static method Main(): int "
        "  using var w := new W(5); "
        "  var arr := new Array<W>(3, w); "
        "  return arr[0].v + arr[1].v + arr[2].v; "
        "end Main; end C;"), 15);
}

// ── Soundness: the fill / size arguments are type-checked (§4.4, §5.1) ─────────

BEZEL_TEST(array_sized_fill_1375, fill_type_mismatch_rejected) {
    // per stdlib §4.4 v0.165 (#1375): fill must be assignable to T.
    BEZEL_EXPECT_TRUE(rejects(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<int>(4, \"x\"); "  // string fill for int T
        "  return arr.Count; "
        "end Main; end C;"));
}

BEZEL_TEST(array_sized_fill_1375, size_type_mismatch_rejected) {
    // per stdlib §4.4 v0.165 (#1375): size must be int-like.
    BEZEL_EXPECT_TRUE(rejects(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<int>(\"n\", 4); "  // string size
        "  return arr.Count; "
        "end Main; end C;"));
}

BEZEL_TEST(array_sized_fill_1375, byte_fill_out_of_range_literal_rejected) {
    // per stdlib §4.4 / §4.2 (#1375): 300 does not fit byte → rejected.
    BEZEL_EXPECT_TRUE(rejects(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<byte>(4, 300); "  // 300 > 255
        "  return arr.Count; "
        "end Main; end C;"));
}
