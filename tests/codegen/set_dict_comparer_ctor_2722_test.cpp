// Spec-derived regression tests for fix #2722 — the hash-collection
// `IEqualityComparer` constructors are DIAGNOSED instead of silently ignored.
//
// AUTHORITY. stdlib §4.10 (v0.240) declares on `Set<T>`, verbatim:
//
//     public constructor();
//     public constructor(capacity: int);
//     public constructor(comparer: IEqualityComparer<T>);
//     public constructor(capacity: int, comparer: IEqualityComparer<T>);   // v0.195: the §4.8 combined form
//
// and the identical four on `OwningSet<T>`; §4.8 and §4.9 declare the same
// comparer pair on `Dictionary<K, V>` and `OwningDictionary<K, V>`:
//
//     public constructor(comparer: IEqualityComparer<K>);
//     public constructor(capacity: int, comparer: IEqualityComparer<K>);
//
// with `IEqualityComparer<T>` itself (§4.8) being
// `method Equals(a: T, b: T): bool; method GetHashCode(value: T): int;`.
// §4.8's "Stability while stored" paragraph names "the collection's
// `IEqualityComparer` (hash forms)" as what governs lookup, so the comparer is
// NORMATIVE, not advisory.
//
// THE DEFECT. Codegen's `lowerNew` Set and Dictionary arms lowered the first
// argument only to emit the negative-capacity guard when it was an `int`, and
// dropped it otherwise; the table was then always built from the ELEMENT TYPE's
// default trampolines, so a supplied comparer never participated. Measured on
// 07cc2e0: a comparer making 3, 13 and 23 equal still gave `Set<int>.Count == 3`
// where §4.10 requires 1 — no diagnostic, no crash, just a wrong answer.
//
// WHY THIS FIXES IT WITH A DIAGNOSTIC RATHER THAN AN IMPLEMENTATION. Unlike
// #2693's `OwningLinkedList<T>(source)`, the form IS specified, so "reject as
// unspecified" would be wrong. It is deferred anyway because implementing it is
// not a codegen change, on two MECHANICAL grounds: `bz_dict_alloc_generic` takes
// bare hash/eq function pointers with NO user-data parameter and `bz_dict_t` has
// no context field, so the comparer instance has nowhere to live; and the
// existing trampoline builders do not reset `propagateBB_`/`unwindTarget_`, so
// an interface call emitted inside one would splice the enclosing function's
// unwind edges in. Sema already records the same deferral in prose ("the
// Dictionary/Set comparer-constructor overloads that would *consume* one are a
// separate, still-deferred feature"); what was missing is that the deferral was
// SILENT. Per the #511 `GetType` precedent, a loud "not yet implemented"
// replaces a silent wrong value. See the follow-up issue for the implementation.
//
// NO DESIGN QUESTION IS OPEN. An earlier draft of this header also claimed the
// comparer's LIFETIME was unspecified and needed a ruling. That is withdrawn:
// the stdlib's own reference implementation of `OwningDictionary<K, V>` declares
// `private _comparer: IEqualityComparer<K>;` — an unmarked, i.e. BORROWING,
// field held for the collection's whole life — and calls `_comparer.Equals(...)`
// inside `Add` (stdlib §4.9), and spec §9.7
// confirms a direct class-typed borrow field is supported and statically
// checked. The borrow reading is already modelled normatively.
//
// A rejection is asserted on the diagnostic TEXT, never via a coarse
// `rejects()`: `runAot`'s WIFSIGNALED path synthesizes an `Error` diagnostic for
// a runtime SIGSEGV too, so a bare "did not run" cannot tell a genuine rejection
// from a crash.
//
// The second half of this file is the FALSE-REJECT guard, which matters more
// than the rejections: every constructor form that is not comparer-bearing must
// still compile AND still produce the right answer.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// A genuine compile-time rejection carrying `needle` in its message.
bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// A user comparer under which 3, 13 and 23 are all equal — the filed repro's.
const char* CMP =
    "module M; import System.Collections.Generic; "
    "class ModCmp: IEqualityComparer<int> "
    "  public method Equals(a: int, b: int): bool => a mod 10 = b mod 10; "
    "  public method GetHashCode(value: int): int => value mod 10; "
    "end ModCmp; ";

std::string withCmp(const std::string& body) {
    return std::string(CMP) +
           "class C public static method Main(): int " + body + " end Main; end C;";
}

std::string plain(const std::string& body) {
    return "module M; import System.Collections.Generic; "
           "class C public static method Main(): int " + body + " end Main; end C;";
}

}  // namespace

// ── The four comparer-bearing forms are diagnosed (§4.8 / §4.10) ─────────────

// per stdlib §4.10 (v0.240), #2722: the filed repro. Pre-fix this compiled and
// returned Count 3 where the comparer requires 1.
BEZEL_TEST(set_dict_comparer_ctor_2722, set_comparer_ctor_diagnosed) {
    BEZEL_EXPECT_TRUE(rejectsWith(withCmp(
        "  using var cmp: ModCmp := new ModCmp(); "
        "  using var s: Set<int> := new Set<int>(cmp); "
        "  s.Add(3); s.Add(13); s.Add(23); "
        "  return s.Count; "),
        "the 'Set' IEqualityComparer constructor is not yet implemented"));
}

// per stdlib §4.10 (v0.240), #2722: the v0.195 combined `(capacity, comparer)`
// form. The filer inferred this from code shape but did not test it.
BEZEL_TEST(set_dict_comparer_ctor_2722, set_capacity_comparer_ctor_diagnosed) {
    BEZEL_EXPECT_TRUE(rejectsWith(withCmp(
        "  using var cmp: ModCmp := new ModCmp(); "
        "  using var s: Set<int> := new Set<int>(8, cmp); "
        "  s.Add(3); "
        "  return s.Count; "),
        "the 'Set' IEqualityComparer constructor is not yet implemented"));
}

// per stdlib §4.8 (v0.240), #2722: the Dictionary arm drops the comparer through
// the same code shape. Also inferred-not-proven in the filed issue.
BEZEL_TEST(set_dict_comparer_ctor_2722, dictionary_comparer_ctor_diagnosed) {
    BEZEL_EXPECT_TRUE(rejectsWith(withCmp(
        "  using var cmp: ModCmp := new ModCmp(); "
        "  using var d: Dictionary<int, int> := new Dictionary<int, int>(cmp); "
        "  d.Add(1, 1); "
        "  return d.Count; "),
        "the 'Dictionary' IEqualityComparer constructor is not yet implemented"));
}

// per stdlib §4.8 (v0.240), #2722: and its combined form.
BEZEL_TEST(set_dict_comparer_ctor_2722, dictionary_capacity_comparer_ctor_diagnosed) {
    BEZEL_EXPECT_TRUE(rejectsWith(withCmp(
        "  using var cmp: ModCmp := new ModCmp(); "
        "  using var d: Dictionary<int, int> := new Dictionary<int, int>(8, cmp); "
        "  d.Add(1, 1); "
        "  return d.Count; "),
        "the 'Dictionary' IEqualityComparer constructor is not yet implemented"));
}

// The message must say WHY, so the deferral is actionable rather than a bare
// refusal — this is the whole point of choosing a loud failure over a silent one.
BEZEL_TEST(set_dict_comparer_ctor_2722, diagnostic_explains_the_silent_default) {
    BEZEL_EXPECT_TRUE(rejectsWith(withCmp(
        "  using var cmp: ModCmp := new ModCmp(); "
        "  using var s: Set<int> := new Set<int>(cmp); "
        "  return s.Count; "),
        "the comparer would be silently ignored and default equality used instead"));
}

// ── FALSE-REJECT GUARD: every non-comparer form still compiles and is correct ─

// per stdlib §4.10 (v0.240), #2722: the no-arg and capacity forms are untouched.
BEZEL_TEST(set_dict_comparer_ctor_2722, set_default_and_capacity_ctors_still_work) {
    BEZEL_EXPECT_EQ(run(plain(
        "  using var s0: Set<int> := new Set<int>(); "
        "  s0.Add(1); s0.Add(1); "
        "  using var s1: Set<int> := new Set<int>(16); "
        "  s1.Add(2); "
        "  if s0.Count <> 1 then return 90; end; "     // dedup by §8.3 equality
        "  if s1.Count <> 1 then return 91; end; "
        "  return 42; ")), 42);
}

// per stdlib §4.8 (v0.240), #2722: string keys (the dedicated `bz_dict_alloc`
// fast path) and non-string keys (the `bz_dict_alloc_generic` trampoline path)
// are BOTH untouched — the two arms the fix inserts its guard into.
BEZEL_TEST(set_dict_comparer_ctor_2722, dictionary_default_and_capacity_ctors_still_work) {
    BEZEL_EXPECT_EQ(run(plain(
        "  using var d0: Dictionary<string, int> := new Dictionary<string, int>(); "
        "  d0.Add(\"a\", 1); "
        "  using var d1: Dictionary<string, int> := new Dictionary<string, int>(8); "
        "  d1.Add(\"b\", 2); "
        "  using var d2: Dictionary<int, int> := new Dictionary<int, int>(4); "
        "  d2.Add(5, 6); "
        "  if d0.Count <> 1 then return 90; end; "
        "  if d1.Count <> 1 then return 91; end; "
        "  if d2[5] <> 6 then return 92; end; "
        "  return 42; ")), 42);
}

// per stdlib §4.10 (v0.240), #2722: a Set LITERAL materializes through
// `materializeHeapSetLiteral`, a different lowering that must not be caught by
// the new guard.
BEZEL_TEST(set_dict_comparer_ctor_2722, set_literal_unaffected) {
    BEZEL_EXPECT_EQ(run(plain(
        "  const lit: Set<int> := [1, 2, 3]; "
        "  return lit.Count; ")), 3);
}

// per stdlib §4.17 (`SortedDictionary<K, V>`) / §4.18 (`SortedSet<T>`), #2722:
// the SORTED family is deliberately out of scope — §4.17 says ordering comes
// from `IComparable<K>.CompareTo` and that there is "no separate comparer
// parameter", so what a comparer means
// there is a separate question. This pins that the fix did not disturb them.
BEZEL_TEST(set_dict_comparer_ctor_2722, sorted_family_unaffected) {
    BEZEL_EXPECT_EQ(run(plain(
        "  using var ss: SortedSet<int> := new SortedSet<int>(); "
        "  ss.Add(3); ss.Add(1); "
        "  return ss.Count; ")), 2);
}

// ── The OWNING types: in the predicate, so they must be tested (§4.9/§4.10) ──

// per stdlib §4.10 (v0.240), #2722: `OwningSet<T>` declares the same four
// constructors as `Set<T>`, and shares the arm.
BEZEL_TEST(set_dict_comparer_ctor_2722, owning_set_comparer_ctor_diagnosed) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        std::string(CMP) +
        "class Box public V: int; public constructor(v: int) V := v; end constructor; "
        "  destructor end destructor; end Box; "   // §A.4: a destructor has no visibility
        "class BoxCmp: IEqualityComparer<Box> "
        "  public method Equals(a: Box, b: Box): bool => a.V = b.V; "
        "  public method GetHashCode(value: Box): int => value.V; "
        "end BoxCmp; "
        "class C public static method Main(): int "
        "  using var cmp: BoxCmp := new BoxCmp(); "
        "  using var s: OwningSet<Box> := new OwningSet<Box>(cmp); "
        "  return s.Count; "
        "end Main; end C;",
        "the 'OwningSet' IEqualityComparer constructor is not yet implemented"));
}

// per stdlib §4.9 (v0.240), #2722. §4.9 — NOT §4.8 — is where
// `OwningDictionary<K, V>`'s comparer constructors are declared. An earlier
// draft passed one section for both dictionary names and so mis-cited this one;
// the citation is asserted here precisely because that shipped untested.
BEZEL_TEST(set_dict_comparer_ctor_2722, owning_dictionary_comparer_ctor_cites_4_9) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        std::string(CMP) +
        "class Box public V: int; public constructor(v: int) V := v; end constructor; "
        "  destructor end destructor; end Box; "   // §A.4: a destructor has no visibility
        "class C public static method Main(): int "
        "  using var cmp: ModCmp := new ModCmp(); "
        "  using var d: OwningDictionary<int, Box> := new OwningDictionary<int, Box>(cmp); "
        "  return d.Count; "
        "end Main; end C;",
        "stdlib §4.9: the 'OwningDictionary' IEqualityComparer constructor"));
}

// The set family's section is shared, so pin that one too — §4.10 covers
// "Set<T> and OwningSet<T>" together and must NOT be split.
BEZEL_TEST(set_dict_comparer_ctor_2722, set_diagnostic_cites_4_10) {
    BEZEL_EXPECT_TRUE(rejectsWith(withCmp(
        "  using var cmp: ModCmp := new ModCmp(); "
        "  using var s: Set<int> := new Set<int>(cmp); "
        "  return s.Count; "),
        "stdlib §4.10: the 'Set' IEqualityComparer constructor"));
}

// per stdlib §4.9/§4.10 (v0.240), #2722: the owning types' non-comparer forms
// must still compile — the false-reject guard for the arm above.
BEZEL_TEST(set_dict_comparer_ctor_2722, owning_types_default_ctors_still_work) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class Box public V: int; public constructor(v: int) V := v; end constructor; "
        "  destructor end destructor; end Box; "   // §A.4: a destructor has no visibility
        "class C public static method Main(): int "
        "  using var s: OwningSet<Box> := new OwningSet<Box>(); "
        "  s.Add(new Box(1)); "
        "  using var d: OwningDictionary<int, Box> := new OwningDictionary<int, Box>(); "
        "  d.Add(1, new Box(2)); "
        "  using var d2: OwningDictionary<int, Box> := new OwningDictionary<int, Box>(4); "
        "  d2.Add(3, new Box(4)); "
        "  if s.Count <> 1 then return 90; end; "
        "  if d.Count <> 1 then return 91; end; "
        "  if d2.Count <> 1 then return 92; end; "
        "  return 42; "
        "end Main; end C;"), 42);
}

// ── The identified false-reject vector: a NON-LITERAL capacity ──────────────

// per stdlib §4.8/§4.10 (v0.240), #2722. The capacity discriminator reads
// `exprType`, so a computed capacity is the argument shape most likely to be
// mis-classified as a comparer. No test in the repo covered a non-literal
// capacity before this one.
BEZEL_TEST(set_dict_comparer_ctor_2722, non_literal_capacity_is_not_a_comparer) {
    BEZEL_EXPECT_EQ(run(plain(
        "  var n: int := 4; "
        "  using var s: Set<int> := new Set<int>(n); "
        "  s.Add(1); "
        "  using var xs: Array<int> := new Array<int>(); "
        "  xs.Add(1); xs.Add(2); "
        // a capacity that is a property read, not a local
        "  using var d: Dictionary<string, int> := new Dictionary<string, int>(xs.Count); "
        "  d.Add(\"k\", 1); "
        // and an arithmetic expression
        "  using var s2: Set<int> := new Set<int>(n * 2 + 1); "
        "  s2.Add(9); "
        "  if s.Count <> 1 then return 90; end; "
        "  if d.Count <> 1 then return 91; end; "
        "  if s2.Count <> 1 then return 92; end; "
        "  return 42; ")), 42);
}

// ── §5.1 WIDENING capacities: the false reject this guard almost shipped ────

// per spec §5.1 (v0.240) + stdlib §4.8/§4.10, #2722. §5.1 makes `sbyte`, `byte`,
// `short`, `ushort` and `char` implicitly convertible to `int`, so each of these
// is a well-typed call against the declared `constructor(capacity: int)` and
// COMPILES TODAY. A discriminator matching `Int` alone classified them as the
// comparer form and hard-rejected them — a false reject carrying a WRONG
// explanation ("the IEqualityComparer constructor is not yet implemented").
//
// Nothing upstream catches this: the builtin collections have
// `declaration == nullptr` so sema's `checkNewExprArgs` never runs, and
// `exprType` reports the argument's NATURAL type, never a target-converted one.
BEZEL_TEST(set_dict_comparer_ctor_2722, widening_integral_capacity_is_not_a_comparer) {
    BEZEL_EXPECT_EQ(run(plain(
        "  var b: byte := 8; "
        "  using var s: Set<int> := new Set<int>(b); "
        "  s.Add(1); "
        "  var sh: short := 4; "
        "  using var d: Dictionary<string, int> := new Dictionary<string, int>(sh); "
        "  d.Add(\"k\", 1); "
        "  var c: char := 'A'; "
        "  using var s2: Set<int> := new Set<int>(c); "
        "  s2.Add(2); "
        "  if s.Count <> 1 then return 90; end; "
        "  if d.Count <> 1 then return 91; end; "
        "  if s2.Count <> 1 then return 92; end; "
        "  return 42; ")), 42);
}

// per spec §7.4/§12.7 (v0.240, #914) + stdlib §4.8/§4.10, #2722. A nullable
// integral proved non-null by FLOW NARROWING is non-nullable in value position,
// so this is a well-typed capacity call that compiles and runs correctly.
//
// It is the subtlest false-reject vector in this guard, because the narrowing is
// invisible to `exprType`: sema's implementation states verbatim that it treats
// the narrowed source as non-nullable "WITHOUT rewriting its exprType", so a
// `!nullable` conjunct in the discriminator rejected it with the comparer
// message. Dropping that conjunct cannot re-open the silent drop, because
// `IEqualityComparer<T>` is an interface (`Kind::Named`) and every capacity
// primitive is `Kind::Primitive` — disjoint whatever the nullability.
BEZEL_TEST(set_dict_comparer_ctor_2722, narrowed_nullable_capacity_is_not_a_comparer) {
    BEZEL_EXPECT_EQ(run(plain(
        "  var n: int? := 8; "
        "  if n is not null then "
        "    using var s: Set<int> := new Set<int>(n); "
        "    s.Add(1); "
        "    using var d: Dictionary<string, int> := new Dictionary<string, int>(n); "
        "    d.Add(\"k\", 1); "
        "    if s.Count <> 1 then return 90; end; "
        "    if d.Count <> 1 then return 91; end; "
        "    return 42; "
        "  end; "
        "  return 99; ")), 42);
}

// per spec §5.1 (v0.240), #2722: the OTHER half of the width rule. §5.1's
// never-implicit list covers signed/unsigned of the same or narrower width and
// every narrowing, so `long` does NOT convert to `int` — `new Set<int>(n)` for
// `n: long` is not a well-typed call to `constructor(capacity: int)` and must
// stay on the comparer side rather than being silently widened into a capacity.
BEZEL_TEST(set_dict_comparer_ctor_2722, long_capacity_is_not_widened_into_a_capacity) {
    BEZEL_EXPECT_TRUE(rejectsWith(plain(
        "  var n: long := 8; "
        "  using var s: Set<int> := new Set<int>(n); "
        "  return s.Count; "),
        "the 'Set' IEqualityComparer constructor is not yet implemented"));
}

// per stdlib §1/§4.8 (v0.183, #1396), #2722: the negative-capacity fault is
// emitted from the same block the new guard was inserted ahead of, so prove the
// guard did not displace it — AND that `-1` is not swallowed as a comparer.
//
// A bare `run(...) != 0` would be VACUOUS here: the local `run()` returns -999
// on a COMPILE failure, so it would pass whether `-1` faults at runtime (right)
// or is falsely rejected at compile time (wrong). Separate the two explicitly.
BEZEL_TEST(set_dict_comparer_ctor_2722, negative_capacity_faults_and_is_not_rejected) {
    auto r = runAot(plain(
        "  using var s: Set<int> := new Set<int>(-1); "
        "  return 0; "));
    // (1) NOT rejected by the new comparer guard — `-1` is the capacity form.
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_TRUE(
            d.message.find("IEqualityComparer constructor is not yet implemented")
            == std::string::npos);
    // (2) and the §1396 ArgumentOutOfRangeException still stops the program
    //     before it can reach `return 0`.
    BEZEL_EXPECT_TRUE(!(r.ran && r.exitCode == 0));
}
