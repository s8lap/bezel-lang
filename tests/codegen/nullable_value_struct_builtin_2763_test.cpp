// fix #2763 / spec §7.2 / §2.5–§2.6 / stdlib §4.14–§4.15 / §2.15 / §9.8 / §31.4:
// a NULLABLE VALUE-STRUCT BUILTIN (`Span<T>?`, `ReadOnlySpan<T>?`, `ArraySpan<T>?`,
// `ReadOnlyArraySpan<T>?`, `ReadOnlyStringSpan?`, `JsonOptions?`, `ExceptionInfo?`,
// `Range<T>?`) had no TAGGED nullable encoding — isNullableValueTy →
// isNullableValueBuiltinTy → valueBuiltinName → valueBuiltinBoxNameStr recognises
// only five names (DateTime/TimeSpan/DateOnly/TimeOnly/Guid), so every other
// value-struct builtin fell through `nullableValueStructTy` to the placeholder
// `nullablePrimStructTy(rt->primitive)` path (a primitive whose rt->primitive is
// the default — Int — for a Named kind), and was lowered as its bare struct with
// no hasValue tag at all.
//
// The shape consequences (measured on HEAD):
//  * `Range<int>? := 1..5; if rn = null then …` — the assignment OVER-WROTE a
//    24-byte Range payload into a 9-byte `{i8, i32}` slot (buffer overflow) AND
//    the read of `rn = null` extracted the wrong element (a struct field, not
//    a tag) — `bezelc` aborted with a SIGSEGV during codegen (segfault on
//    `Range<int>? := 1..5; if rn = null then acc := 1; end;`, measured). For the
//    `null`-literal initializer the runtime returned 0 because the all-zero
//    `{i8, i32}` slot happened to compare bitwise equal to itself in `= null`,
//    but a non-trivial initializer crashed the compiler.
//  * `Span<int>?` over an `Array<int>` — same shape defect: a 16-byte array-span
//    payload was truncated into a 9-byte slot, and a present-value `= null`
//    check crashed the compiler (segfault, measured).
//  * `JsonOptions?`, `ExceptionInfo?`, `ReadOnlyStringSpan?` — same defect.
//
// Fix: extend the value-struct recognition set to include the seven affected
// names. They are NOT added to `valueBuiltinBoxNameStr` (the BOX-key set,
// used for boxing into Object) — they have their own box paths
// (`isSpanLikeTypeName` for spans, `rangeBoxable` for Range, `JsonOptions` has
// no box, `ExceptionInfo` is a private causechain element). Instead a new
// `isNullableValueStructBuiltinTy` predicate handles the NULLABLE-encoding
// path; `nullableValueStructTy` lowers a hit to
// `{ i8 hasValue; <lowerTypeRef(non-nullable)> }` — the same tagged shape the
// five already-recognised names get.
//
// Observable is Main's returned int.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §9.8 / spec §7.2 (#2763): a present `Range<int>? := 1..5; if rn = null then …`
// — the bare Range lowering overwrote a 24-byte payload into a 9-byte slot AND
// the `= null` read extracted a struct field rather than the (non-existent)
// tag, so `bezelc` crashed with SIGSEGV during codegen on HEAD. With the fix the
// has-value case is distinguished from null, and the present Range value
// survives the assignment → 7.
BEZEL_TEST(nullable_value_struct_builtin_2763, range_nullable_present_no_crash) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var rn: Range<int>? := 1..5; "
        "    if rn = null then return 11; end; "       // present → must not fire
        "    return 7; "
        "  end Main; end Program;"), 7);
}

// §9.8 / spec §7.2 (#2763): a `Range<int>?` round-trips its value AND a real
// null `Range<int>?` compares equal to null. Encodes: present(+1) &
// value-preserved(+10) & real-null-is-null(+100) → 111. On HEAD the present
// arm crashed the compiler (-999); on the fix it returns 111.
BEZEL_TEST(nullable_value_struct_builtin_2763, range_nullable_round_trip) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var rn: Range<int>? := 1..5; "
        "    var acc: int := 0; "
        "    if rn <> null then acc := acc + 1; end; "    // present
        "    if rn = (1..5) then acc := acc + 10; end; "  // value preserved
        "    var nn: Range<int>? := null; "
        "    if nn = null then acc := acc + 100; end; "   // real null
        "    if nn <> null then acc := acc + 1000; end; " // must not fire
        "    return acc; "
        "  end Main; end Program;"), 111);
}

// §4.14 / spec §7.2 (#2763): a present `Span<int>?` over an `Array<int>` — the
// bare array-span lowering truncated a 16-byte payload into a 9-byte slot AND
// the `= null` read extracted a struct field rather than the (non-existent)
// tag, so `bezelc` crashed with SIGSEGV during codegen on HEAD. With the fix
// the has-value case is distinguished from null → 7.
BEZEL_TEST(nullable_value_struct_builtin_2763, span_nullable_present_no_crash) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(10); "
        "    a.Add(20); "
        "    var sn: Span<int>? := a.AsSpan(); "
        "    if sn = null then return 11; end; "       // present → must not fire
        "    return 7; "
        "  end Main; end Program;"), 7);
}

// §4.14 / spec §7.2 (#2763): a `Span<int>?` over an `Array<int>` round-trips
// its value AND a real null `Span<int>?` compares equal to null. Encodes:
// present(+1) & length-preserved(+10) & real-null-is-null(+100) → 111. On
// HEAD the present arm crashed the compiler (-999); on the fix it returns 111.
BEZEL_TEST(nullable_value_struct_builtin_2763, span_nullable_round_trip) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(10); "
        "    a.Add(20); "
        "    var sn: Span<int>? := a.AsSpan(); "
        "    var acc: int := 0; "
        "    if sn <> null then acc := acc + 1; end; "    // present
        "    var live := sn!.Length; "                    // unwrap → payload
        "    if live = 2 then acc := acc + 10; end; "     // value preserved
        "    var nn: Span<int>? := null; "
        "    if nn = null then acc := acc + 100; end; "   // real null
        "    if nn <> null then acc := acc + 1000; end; " // must not fire
        "    return acc; "
        "  end Main; end Program;"), 111);
}

// §2.15 / spec §7.2 (#2763): a `JsonOptions?` round-trips its value AND a
// real null `JsonOptions?` compares equal to null. Encodes: present(+1) &
// real-null-is-null(+100) → 101. On HEAD the assignment of the present value
// overwrites a JsonOptions struct into a `{i8, i32}` placeholder slot, and
// the `= null` read crashes the compiler (-999); on the fix it returns 101.
BEZEL_TEST(nullable_value_struct_builtin_2763, jsonoptions_nullable_round_trip) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Serialization; "
        "class Program "
        "  public static method Main(): int "
        "    var j := new JsonOptions(); "
        "    var jn: JsonOptions? := j; "
        "    var acc: int := 0; "
        "    if jn <> null then acc := acc + 1; end; "  // present
        "    var nn: JsonOptions? := null; "
        "    if nn = null then acc := acc + 100; end; " // real null
        "    if nn <> null then acc := acc + 1000; end; "
        "    return acc; "
        "  end Main; end Program;"), 101);
}

// §31.4 / spec §7.2 (#2763): a present `ExceptionInfo?` — the
// `{ptr,ptr,ptr}` causechain element — must have a tagged nullable encoding.
// On HEAD the bare struct is lowered as `{ptr,ptr,ptr}` with no hasValue tag,
// so `if ei = null then …` reads a struct field rather than a tag (a wrong
// answer); with the fix the has-value case is distinguished from null → 7.
BEZEL_TEST(nullable_value_struct_builtin_2763, exceptioninfo_nullable_present) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var ei := new ExceptionInfo(\"T\", \"M\", \"S\"); "
        "    var ein: ExceptionInfo? := ei; "
        "    if ein = null then return 11; end; "       // present → must not fire
        "    return 7; "
        "  end Main; end Program;"), 7);
}

// §3.3 / spec §7.2 (#2763): a `ReadOnlyStringSpan?` over a string — the bare
// array-span lowering (string ptr + start + length) must be wrapped in a
// hasValue tag. On HEAD the bare struct is lowered with no tag and the
// `= null` read returns a wrong answer; with the fix it returns the
// spec-stated answer → 7.
BEZEL_TEST(nullable_value_struct_builtin_2763, readonlystringspan_nullable_present) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var rs := \"hello\".AsSpan(); "
        "    var rsn: ReadOnlyStringSpan? := rs; "
        "    if rsn = null then return 11; end; "       // present → must not fire
        "    return 7; "
        "  end Main; end Program;"), 7);
}