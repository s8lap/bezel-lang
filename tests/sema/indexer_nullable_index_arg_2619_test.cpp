// Spec-derived regression test for issue #2619.
//
// §7.1 (Non-nullable and nullable types) / §7.4 (Member access on nullable
// values) / §23.4 (Indexers), v0.240.
//
// §23.4: "An indexer is a property whose name is replaced by `[parameters]`."
// An index argument is therefore a PARAMETER PASS, and §7.1 states: "The
// compiler enforces the distinction at every assignment, parameter pass, and
// return."
//
// EVERY indexer path in sema skipped that check. #2613 closed it for the single
// `ReadOnlyStringSpan` receiver and attributed the hole to builtin members
// having no MethodSymbol — but that explanation does NOT cover the
// USER-declared indexer, which does resolve a `PropertySymbol` and was equally
// unchecked. An un-narrowed `int?` index was silently accepted by sema on
// `Array<T>`, `string`, `Dictionary<K,V>`, the span/buffer families, inline
// arrays AND user-declared indexers (class, record and interface receivers,
// local and imported), and reached codegen, which emitted malformed IR:
//
//     SExt only operates on integer      %7 = sext { i8, i32 } %4 to i64
//     GEP indexes must be integers       ... getelementptr ..., { i8, i32 } %8
//     Trunc only operates on integer     %11 = trunc { i8, i32 } %5 to i32
//     Call parameter type does not match function signature!
//
// i.e. an internal compiler error where §7 asks for a clean diagnostic. The fix
// runs the check ONCE per IndexExpr, for every receiver family, in
// `ExprTC::checkIndexArgNullability` (src/sema/ExprTypecheck_Exprs.cpp).
//
// This is a REJECTION fix, so every negative case asserts on diagnostic TEXT —
// NOT the coarse rejects() helper, which cannot tell a genuine rejection from a
// crash (runAot's WIFSIGNALED path synthesizes an Error diagnostic for a
// runtime SIGSEGV too). The positive controls are the load-bearing half: a
// widened rejection that breaks working code is worse than the bug it fixes, so
// every index spelling the compiler accepts today is pinned here — including
// the §7.4-NARROWED forms, which are exactly what a naive "is the argument type
// nullable" rejection would break (§7.4 narrowing deliberately does not rewrite
// `exprType`).

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"  // runAot — end-to-end accept control
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

using bezel::test::runAot;

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic contains `sub`.
bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The §7 "nullable -> non-nullable" assignability diagnostic fragment, shared
// with the user-method path and every sibling builtin gate (#2315/#2582/#2613).
constexpr const char* kNullableDiag =
    "is nullable but the target type is non-nullable";

// The §5.1 "not implicitly convertible" fragment — the SHARED needle every
// §5.1 site reports (`checkAssignable`'s one message). #7064 (v0.343) reports
// it at the INDEX position, where the index argument's type is checked
// against the indexer's declared parameter type.
constexpr const char* kNotConvertible =
    "has a type that is not implicitly convertible to the target type (§5.1)";

// The type declarations the cases below index into. Kept in one place so a
// negative case and its positive control index the SAME declaration.
constexpr const char* kDecls =
    "enum E A, B; end E; "
    "interface IIdx readonly property [i: int]: int; end IIdx; "
    "class Idx: IIdx public property [i: int]: int => i + 1; end Idx; "
    "class Grid public property [r: int, c: int]: int => r + c; end Grid; "
    "class EnumIdx public property [e: E]: int => 5; end EnumIdx; "
    "class NullIdx public property [i: int?]: int => 9; end NullIdx; "
    "class Box<T> public property [k: T]: int => 3; end Box; "
    "record Rec public x: int; public property [i: int]: int => i * 2; "
    "end Rec; ";

// A compilation unit made of kDecls + a class C wrapping `body`. `NI()` is the
// canonical un-narrowed `int?` source; `NS()` / `NE()` its string / enum twins.
std::string prog(const std::string& body) {
    return std::string("module M; ") + kDecls +
           "class C "
           "  private static method NI(): int? => null; "
           "  private static method NS(): string? => null; "
           "  private static method NE(): E? => null; " +
           body + " end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// REJECT — one case per receiver family the issue enumerates. Each asserts the
// §7 text AND the receiver-specific subject, so a case cannot pass on some
// OTHER site's diagnostic.
// ---------------------------------------------------------------------------

// #2619 site 1 / §7.1 + stdlib §4.4 (`Array<T>`: `public property [i: int]: T`)
// — the most common indexer in the language. Pre-fix: no sema diagnostic, then
// `SExt only operates on integer` at codegen.
BEZEL_TEST(sema_indexer_nullable_index_arg_2619,
           array_indexer_rejects_unnarrowed_nullable_index_7_1) {
    auto r = parseSemaAll(prog(
        "public static method T(a: Array<int>): int return a[NI()]; end T;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "index argument to `Array[i]`"));
    BEZEL_EXPECT_TRUE(hasError(r, "(\xC2\xA7""7)"));
}

// #2619 site 2 / §4.6 + stdlib §4.6 (`string`: `public pure readonly property
// [i: int]: char`). Pre-fix: `%str.idx64 = sext { i8, i32 } ... to i64`.
BEZEL_TEST(sema_indexer_nullable_index_arg_2619,
           string_indexer_rejects_unnarrowed_nullable_index) {
    auto r = parseSemaAll(prog(
        "public static method T(s: string): char return s[NI()]; end T;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "index argument to `string[i]`"));
}

// #2619 site 3 / stdlib §4.8 (`Dictionary<K,V>`: `public property [key: K]:
// V`). The KEY type is `K`, not `int` — the check must be keyed to
// genericArgs[0], exactly as checkBuiltinCollectionCallArgs is for
// ContainsKey/Remove. Both a `K = int` and a `K = string` dictionary are pinned.
BEZEL_TEST(sema_indexer_nullable_index_arg_2619,
           dictionary_key_indexer_rejects_unnarrowed_nullable_key_4_8) {
    auto r = parseSemaAll(prog(
        "public static method T(d: Dictionary<int, int>): int "
        "  return d[NI()]; end T;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "index argument to `Dictionary[key]`"));

    auto r2 = parseSemaAll(prog(
        "public static method T(d: Dictionary<string, int>): int "
        "  return d[NS()]; end T;"));
    BEZEL_EXPECT_TRUE(hasError(r2, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r2, "index argument to `Dictionary[key]`"));
}

// #2619 sites 4/7/9/10 — the span and buffer families (stdlib §4.14 / §4.15).
// `ArraySpan<T>` borrows an `Array<T>`; `Span<T>` / `OwningBuffer<T>` are the
// owning pair. Each declares `[i: int]`.
BEZEL_TEST(sema_indexer_nullable_index_arg_2619,
           span_and_buffer_indexers_reject_unnarrowed_nullable_index) {
    auto r = parseSemaAll(prog(
        "public static method T1(sp: ArraySpan<int>): int "
        "  return sp[NI()]; end T1; "
        "public static method T2(ob: OwningBuffer<int>): int "
        "  return ob[NI()]; end T2; "
        "public static method T3(s: Span<int>): int "
        "  return s[NI()]; end T3;"));
    BEZEL_EXPECT_TRUE(hasError(r, "index argument to `ArraySpan[i]`"));
    BEZEL_EXPECT_TRUE(hasError(r, "index argument to `OwningBuffer[i]`"));
    BEZEL_EXPECT_TRUE(hasError(r, "index argument to `Span[i]`"));
}

// #2619 site 5 — THE case #2613's root-cause story did not cover: a
// USER-DECLARED indexer. It HAS a PropertySymbol, so "builtin members have no
// MethodSymbol" never explained it. Pre-fix: `GEP indexes must be integers` /
// `Call parameter type does not match function signature!`. All three receiver
// categories §23.4 admits are pinned: class, interface (`Indexers may be
// declared in interfaces`), and record (`Records may declare read-only
// indexers`).
BEZEL_TEST(sema_indexer_nullable_index_arg_2619,
           user_declared_indexer_rejects_unnarrowed_nullable_index_23_4) {
    auto r = parseSemaAll(prog(
        "public static method T1(u: Idx): int return u[NI()]; end T1; "
        "public static method T2(i: IIdx): int return i[NI()]; end T2; "
        "public static method T3(v: Rec): int return v[NI()]; end T3;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "index argument to `Idx[i]`"));
    BEZEL_EXPECT_TRUE(hasError(r, "index argument to `IIdx[i]`"));
    BEZEL_EXPECT_TRUE(hasError(r, "index argument to `Rec[i]`"));
}

// #2619 site 6 — the receiver #2613 fixed. Its check moved into the shared
// helper; the diagnostic text is unchanged, so #2613's own regression test
// still pins it. This case is the cross-check that the move did not drop it.
BEZEL_TEST(sema_indexer_nullable_index_arg_2619,
           readonly_string_span_indexer_still_rejects_2613) {
    auto r = parseSemaAll(prog(
        "public static method T(): int "
        "  var sp := \"hello\".AsSpan(); "
        "  var c := sp[NI()]; "
        "  return 0; "
        "end T;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "index argument to `ReadOnlyStringSpan[i]`"));
}

// #2619 site 8 / §15.1 — an INLINE array `[N]T`. Its element access lowers to a
// GEP, so the pre-fix failure was `GEP indexes must be integers`. There is no
// receiver name to quote, so the subject is the bare "index argument".
BEZEL_TEST(sema_indexer_nullable_index_arg_2619,
           inline_array_indexer_rejects_unnarrowed_nullable_index_15_1) {
    auto r = parseSemaAll(prog(
        "public static method T(): int "
        "  var ia: [4]int; "
        "  return ia[NI()]; "
        "end T;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "the index argument is nullable"));
}

// #2619 / §23.4 ("Indexers may be overloaded by parameter type list") — a
// MULTI-parameter indexer must be checked at EVERY index position, not only the
// first, and the diagnostic must say which one.
BEZEL_TEST(sema_indexer_nullable_index_arg_2619,
           two_parameter_indexer_rejects_at_each_position_23_4) {
    auto r1 = parseSemaAll(prog(
        "public static method T(g: Grid): int return g[NI(), 2]; end T;"));
    BEZEL_EXPECT_TRUE(hasError(r1, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r1, "index argument 1 to the `Grid` indexer"));

    auto r2 = parseSemaAll(prog(
        "public static method T(g: Grid): int return g[1, NI()]; end T;"));
    BEZEL_EXPECT_TRUE(hasError(r2, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r2, "index argument 2 to the `Grid` indexer"));
}

// #2619 / §7.1 ("every assignment") — the WRITE forms take different lowering
// paths from the read: a plain `:=` store, a compound `+=` (which reads first),
// and `++`. All three must reject, and exactly once each.
BEZEL_TEST(sema_indexer_nullable_index_arg_2619,
           assign_compound_and_increment_index_forms_reject) {
    for (const char* stmt : {"a[NI()] := 5; ", "a[NI()] += 1; ", "a[NI()]++; "}) {
        auto r = parseSemaAll(prog(
            std::string("public static method T(a: Array<int>): int ") + stmt +
            "  return 0; end T;"));
        BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
        BEZEL_EXPECT_TRUE(hasError(r, "index argument to `Array[i]`"));
        // Exactly one diagnostic — the compound/increment forms visit the
        // IndexExpr through more than one path, and a double report would be a
        // regression in its own right.
        int n = 0;
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find(kNullableDiag) != std::string::npos) ++n;
        BEZEL_EXPECT_EQ(n, 1);
    }
}

// #2619 / §14.1 — a GENERIC user indexer `[k: T]` instantiated at a
// NON-nullable type argument still declares a non-nullable index, so an
// un-narrowed `int?` is rejected. (Its `Box<int?>` twin below is the positive
// control that proves the substitution — not a bare-`T` guess — is what
// decides.)
BEZEL_TEST(sema_indexer_nullable_index_arg_2619,
           generic_indexer_at_non_nullable_argument_rejects_14_1) {
    auto r = parseSemaAll(prog(
        "public static method T(b: Box<int>): int return b[NI()]; end T;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "index argument to `Box[i]`"));
}

// #2619 / §10 — an ENUM-typed index parameter with an un-narrowed `E?`. The
// check is category-blind: it compares the argument against its own type with
// `nullable` cleared, so it covers every index type, not an enumerated list of
// them.
BEZEL_TEST(sema_indexer_nullable_index_arg_2619,
           enum_typed_index_parameter_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(prog(
        "public static method T(e: EnumIdx): int return e[NE()]; end T;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "index argument to `EnumIdx[i]`"));
}

// ---------------------------------------------------------------------------
// ACCEPT — the load-bearing half. A false reject here breaks working code.
// ---------------------------------------------------------------------------

// #2619 / §7.4 — the four NARROWING forms the section names (a null test in an
// `if`, an early exit, `is not null`, and a `const` local whose narrowing "is
// never invalidated"), on both a builtin and a user-declared indexer, plus the
// multi-parameter form. These are exactly what a naive "is the argument type
// nullable" rejection would break: §7.4 narrowing deliberately does NOT rewrite
// `exprType`, so the accept has to come from checkAssignable, which this fix
// reuses rather than reimplementing.
BEZEL_TEST(sema_indexer_nullable_index_arg_2619,
           narrowed_nullable_index_is_accepted_7_4) {
    auto r = parseSemaAll(prog(
        "public static method T1(a: Array<int>): int "
        "  var i := NI(); if i <> null then return a[i]; end; return 0; end T1; "
        "public static method T2(a: Array<int>): int "
        "  var i := NI(); if i = null then return 0; end; return a[i]; end T2; "
        "public static method T3(a: Array<int>): int "
        "  var i := NI(); if i is not null then return a[i]; end; return 0; "
        "end T3; "
        "public static method T4(a: Array<int>): int "
        "  const i := NI(); if i = null then return 0; end; return a[i]; "
        "end T4; "
        "public static method T5(u: Idx): int "
        "  var i := NI(); if i <> null then return u[i]; end; return 0; "
        "end T5; "
        "public static method T6(g: Grid): int "
        "  var r := NI(); var c := NI(); "
        "  if r <> null and c <> null then return g[r, c]; end; return 0; "
        "end T6; "
        "public static method T7(d: Dictionary<string, int>): int "
        "  var k := NS(); if k <> null then return d[k]; end; return 0; "
        "end T7; "
        "public static method T8(s: string): char "
        "  var i := NI(); if i <> null then return s[i]; end; return ' '; "
        "end T8;"));
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(!r.diagnostics.hasErrors());
}

// #2619 / §7.8 ("`x!` asserts non-nullness. … The result is the non-nullable
// form of the expression") and §7.7 (`??`) — the two non-flow ways to reach a
// non-nullable index.
BEZEL_TEST(sema_indexer_nullable_index_arg_2619,
           null_forgiving_and_coalescing_index_are_accepted_7_7_7_8) {
    auto r = parseSemaAll(prog(
        "public static method T1(a: Array<int>): int return a[NI()!]; end T1; "
        "public static method T2(a: Array<int>): int "
        "  return a[NI() ?? 0]; end T2;"));
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(!r.diagnostics.hasErrors());
}

// #2619 — every NON-nullable index spelling. The fix gates on the argument's
// own type being nullable, so none of these reaches the check at all; this case
// is the mechanical proof of that claim across the axes the receiver families
// differ on (literal, local, `const`, enum, bare generic `T`).
//
// BEHAVIOUR FLIP (#7064, §5.1 / §23.4, v0.343). The `long` axis (`T5`, below)
// USED to sit in this accept control — the comment called it a "widening
// `long`", on the premise that `long` -> `int` is a §5.1 conversion. It is
// NOT: §5.1's table row is `int` -> `long`, never the reverse, and §5.1's
// constant-narrowing clause — the only way a wider integer reaches a
// narrower declared type — applies to COMPILE-TIME CONSTANTS only. #7064
// closes the index position (§23.4's parameter pass), so `a[l]` at a `long l`
// is now a §5.1 error. The axis moved to the row below, which still pins
// #2619's own claim — the §7 gate does NOT fire on a `long` — while pinning
// the §5.1 verdict that replaced it.
BEZEL_TEST(sema_indexer_nullable_index_arg_2619,
           non_nullable_index_spellings_are_untouched) {
    auto r = parseSemaAll(prog(
        "public static method T1(a: Array<int>, i: int): int "
        "  var x := a[0] + a[i]; const c := 1; return x + a[c]; end T1; "
        "public static method T2(e: EnumIdx): int return e[E.A]; end T2; "
        "public static method T3(b: Box<int>): int return b[7]; end T3; "
        "public static method T4<T>(b: Box<T>, k: T): int return b[k]; end T4; "
        "public static method T6(g: Grid): int return g[1, 2]; end T6;"));
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(!r.diagnostics.hasErrors());
}

// The `long` axis moved here (#7064, §5.1 / §23.4, v0.343): a `long` index
// at an `int` index parameter is a §5.1 ERROR (the table has no `long` ->
// `int` row), and it is NOT §7's nullable diagnostic — #2619's own claim,
// that the §7 gate fires only on a NULLABLE argument, still holds.
BEZEL_TEST(sema_indexer_nullable_index_arg_2619,
           long_index_is_rejected_by_the_5_1_rule_not_the_7_rule) {
    auto r = parseSemaAll(prog(
        "public static method T5(a: Array<int>, l: long): int "
        "  return a[l]; end T5; "));
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, kNotConvertible));
}

// #2619 / §15.6 + §4.6 — the RANGE index forms (`s[a..b]` slicing and a
// Range-typed VALUE) select a different indexer overload (stdlib §4.6:
// `public pure readonly property [r: Range<int>]: string`). They are
// non-nullable, so the check never fires on them — the spelling #2613 named as
// the thing a general fix must not break.
BEZEL_TEST(sema_indexer_nullable_index_arg_2619,
           range_slicing_index_forms_are_untouched_15_6) {
    auto r = parseSemaAll(prog(
        "public static method T1(s: string): string return s[0..2]; end T1; "
        "public static method T2(s: string): int "
        "  var rg := 0..2; return s[rg].Length; end T2; "
        "public static method T3(a: Array<int>): int "
        "  var rg := 0..1; return a[rg].Count; end T3;"));
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(!r.diagnostics.hasErrors());
}

// #2619 / §7.1 — the one case where a nullable index argument is LEGAL: the
// declared index parameter is itself nullable. Three shapes: a user indexer
// declared `[i: int?]` (the control #2613 shipped, which must keep passing), a
// `Dictionary<K, V>` whose KEY type is nullable, and a generic indexer
// `[k: T]` INSTANTIATED at `int?` — the last proves the declared type is read
// through generic substitution, not as the bare `T` (which would false-reject).
BEZEL_TEST(sema_indexer_nullable_index_arg_2619,
           nullable_declared_index_parameter_still_accepts_nullable) {
    auto r = parseSemaAll(prog(
        "public static method T1(n: NullIdx): int return n[NI()]; end T1; "
        "public static method T2(d: Dictionary<string?, int>): int "
        "  return d[NS()]; end T2; "
        "public static method T3(b: Box<int?>): int return b[NI()]; end T3;"));
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(!r.diagnostics.hasErrors());
}

// #2619 END-TO-END accept control. Sema acceptance is not enough: the program
// must still COMPILE AND RUN with the same values. Every receiver family the
// fix touches appears here with a legal index (literal, `const`, `!`, compound
// `+=`, `++`, range slice, dictionary key, span, 2-parameter indexer, record,
// generic, interface, enum, and a nullable-declared index parameter), and the
// accumulated total pins the lowering: 96. Verified identical before and after
// the fix.
BEZEL_TEST(sema_indexer_nullable_index_arg_2619,
           legal_index_spellings_still_compile_and_run) {
    auto r = runAot(
        "module M; "
        "enum E A, B; end E; "
        "interface IIdx readonly property [i: int]: int; end IIdx; "
        "class Idx: IIdx public property [i: int]: int => i + 1; end Idx; "
        "class Grid "
        "  private _v: int := 0; "
        "  public property [r: int, c: int]: int "
        "    get => _v + r + c; "
        "    set => _v := value - r - c; "
        "  end; "
        "end Grid; "
        "class EnumIdx public property [e: E]: int => 5; end EnumIdx; "
        "class NullIdx public property [i: int?]: int => 9; end NullIdx; "
        "class Box<T> public property [k: T]: int => 3; end Box; "
        "record Rec public x: int; public property [i: int]: int => i * 2; "
        "end Rec; "
        "class C "
        "  private static method NI(): int? => null; "
        "  private static method SI(): int? => 0; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    using var a := new Array<int>(4, 0); "
        "    a[0] := 10; t := t + a[0]; "            // 10
        "    const ci := 0; t := t + a[ci]; "        // 20
        "    var m := SI(); t := t + a[m!]; "        // 30
        "    a[0] += 1; a[0]++; t := t + a[0]; "     // 42
        "    var ia: [4]int; ia[0] := 3; t := t + ia[0]; "   // 45
        "    var s := \"hi\"; t := t + s[0..1].Length; "     // 47
        "    t := t + int(s[0]) - 104; "                     // 47
        "    using var d := new Dictionary<int, int>(); "
        "    d[1] := 4; t := t + d[1]; "             // 51
        "    var sp := a.AsSpan(); t := t + sp[0]; "  // 63
        "    using var g := new Grid(); "
        "    g[1, 2] := 10; t := t + g[1, 2]; "      // 73
        "    var rec := new Rec(1); t := t + rec[2]; "        // 77
        "    using var bx := new Box<int>(); t := t + bx[7]; " // 80
        "    using var ux := new Idx(); "
        "    var iface: IIdx := ux; t := t + iface[1]; "      // 82
        "    using var ei := new EnumIdx(); t := t + ei[E.A]; " // 87
        "    using var nix := new NullIdx(); t := t + nix[NI()]; " // 96
        "    return t; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 96);
}
