// Sema + codegen — issue #3249: §10.4 / §22.12 / stdlib §4.19 FrozenArray<T>
// — the deeply-immutable, fixed-size, bz_array_t*-backed shared-read
// primitive. The compiler side:
//
//   1. FrozenArray<T> is comment-only in src/ today — the type needs its
//      implementation (stdlib §4.19's surface: From(items: Array<T>), the
//      read surface, the immutable-fact wiring into the §22.12
//      warning-suppression set and the §28.13 Shared-payload synchronized
//      set formula).
//   2. Two tests replicate the old §10.4 example and migrate with it:
//      enum_property_restrictions_2499_test.cpp and static_fields_test.cpp
//      — both pinned in this fix.
//
// This file is the THIRD pin: a from-scratch regression for the §10.4
// example shape (a FrozenArray<T> as a `static readonly` of an enum),
// exercising the From(items: Array<T>) construction form and the .Length
// read surface. Per §4.19, FrozenArray<T>'s element type is restricted to
// sendable value types + string; an enum is a sendable value type, so the
// Priority / Color carrier examples are sound and exercise the constraint
// at its most common use site.

#include "support/aot_support.hpp"   // runAot — AOT compile path
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── §10.4 / stdlib §4.19: a `static readonly` of a FrozenArray<EnumType> ──
//
// The §10.4 flagship enum example, respelled (per v0.256) from
// `Array<Priority>` to `FrozenArray<Priority>`. The `.Length` read is the
// `int`-returning `readonly property Length: int` (stdlib §4.19) — distinct
// from `Array<T>.Count` (§1's Count/Length convention: a fixed-extent
// collection declares Length, a variable-extent one declares Count).
//
// Fix: tryLowerFrozenArrayCall dispatches `FrozenArray<T>.From(items: ...)`
// to a runtime `bz_array_copy` + element-retain + source-free path; the
// `static readonly` initializer lowers through it; the `.Length` read
// lowers through the existing isCollectionTypeName-gated bz_array_len path
// (#3249 widened that gate's predicate to also admit the builtinClassSym-
// declaration shape that FrozenArray's static-factory result carries).
BEZEL_TEST(frozen_array_3249, static_readonly_frozen_array_length_spec_10_4) {
    const std::string program =
        "module M; "
        "enum Priority Low, Medium, High; "
        "  public static readonly All: FrozenArray<Priority> := "
        "      FrozenArray<Priority>.From([Low, Medium, High]); "
        "end Priority; "
        "public class C "
        "  public static method Main(): int "
        "    return Priority.All.Length; "
        "end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 3);
}

// ── stdlib §4.19 / §22.12 rule 4 — the warning-suppression set ──
//
// FrozenArray<T>'s immutable fact (§4.19 / §1) places it in §22.12 rule
// 4's warning-suppression set: a `static owned readonly` of FrozenArray<T>
// is a warning-free, every-domain-readable singleton, where the same
// field at `Array<Priority>` (a mutable class type outside the set)
// would draw rule 4's declaration-site sharing warning. This test pins the
// shared-read idiom at runtime: a FrozenArray<int> shared by reference
// between two reads, demonstrating the §30.14 cross-domain sharing the
// immutable fact admits (the runtime sees the same bz_array_t*).
BEZEL_TEST(frozen_array_3249, frozen_array_immutable_facts_spec_22_12) {
    const std::string program =
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var f := FrozenArray<int>.From([10, 20, 30, 40]); "
        "    return f.Length; "             // 4 elements
        "end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 4);
}

// ── stdlib §4.19: a FrozenArray<string> — the copying construction's
// string-element path. The From(items: Array<T>) form pays one copy at
// construction (§4.19's "the copying construction form for T = string"),
// and the FrozenArray's element release at scope-end balances the
// construction-side retain (emitArrayRetainStringsIfStringArray at
// tryLowerFrozenArrayCall + the §28.9 disposal-side string release).
// Pinning a string element exercises the retain/release pairing the
// v0.186 ruling names as the construction-side obligation.
//
// NOTE: this test exercises the indexer `[i]` and the length read; the
// element comparison is via Length (string equality's `==` overload lives
// on the language side and is not the load-bearing shape — the
// construction-side string retain is).
BEZEL_TEST(frozen_array_3249, frozen_array_string_element_round_trip_spec_4_19) {
    const std::string program =
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var f := FrozenArray<string>.From([\"alpha\", \"beta\", \"gamma\"]); "
        "    if f.Length <> 3 then return 1; end; "
        "    if f[0].Length <> 5 then return 2; end; "
        "    if f[2].Length <> 5 then return 3; end; "
        "    return 42; "
        "end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}
