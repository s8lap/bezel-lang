// Spec-derived regression tests for issue #6023 — the `Object`-source half.
//
// THE RULE. Spec §5.1 "Reference conversions" (v0.338) is a closed list:
//
//   "The following conversions are implicit …: identity …; class to base
//    class, transitively, up to `Object`; any type to `Object` …; a type to an
//    interface it implements or conforms to …; interface to extended interface
//    …; a type parameter to an interface or class its constraints guarantee
//    conformance to …; the variance-induced conversions of §12.9"
//
// and it characterises every one of them as widening: "A reference conversion
// … is representation-preserving — the same reference viewed at a wider
// static type". No row has `Object` as its SOURCE except identity, so a value
// of static type `Object` converts implicitly to no class, interface, type
// parameter or — §4.6, "The compiler does not implicitly convert other types
// to `string`" — `string`. §5.1 "Nullable conversions" adds only "`S?`
// implicitly converts to `T?` whenever `S` implicitly converts to `T`", so
// `Object` → `B?` and `Object?` → `B?` are out as well. The checked spellings
// are §12.7's `is` (which flow-narrows the carrier) and `as` ("returns the
// value cast to the target type, or `null` if the cast fails. Its result is
// always nullable").
//
// WHAT WAS BROKEN. `ExprTC::checkAssignable` — the one §5.1 conversion-site
// predicate — returned "accepted" for an `Object` source into ANY reference
// target (the #3115 carve-out, kept "out of scope" by #5023). The issue's
// repro
//
//     var o: Object := new A();
//     var b: B := o;              // B: A, B adds field `y`
//     Console.WriteLine($"x30 y={b.Y()}");
//
// compiled and printed `x30 y=0`: an `A` read as a `B`, one field past the
// object. MEASURED on 6d8c82eb with the real `bezelc`: every reject row below
// compiled clean before the fix.
//
// TWO SHAPES KEPT PENDING #6083. The spec does not settle whether the code
// after an `if` on whose every surviving path a type test of `o` held
// (`if o is not B then return; end;`, the same with `elif`/`else` branches
// that leave, the positive `if o is B then … else return; end;`), or the body
// of a type-pattern arm `case o of B => …`, sees `o` narrowed. Both compiled and
// ran correctly before this fix (through the conversion it removes), so for an
// `Object` carrier they are kept accepted until #6083 is ruled — the rows named
// `pending_ruling_6083_…` pin exactly that, and the reject rows after them pin
// how far it does NOT reach.
//
// SHAPE OF THE FILE. One reject row per conversion POSITION (they all reach
// the same predicate, so each row is a call site that could drift), then one
// per TARGET KIND at a fixed position, then accept-and-RUN controls whose
// stdout distinguishes "the conversion happened and the right object came
// out" from "it compiled". Every row is on `runAot` — the real driver — and a
// reject row asserts three things: the program did not run, the rejection
// carries a real source location (so a crash or a timeout cannot satisfy it),
// and the diagnostic is THIS rule's, naming both types.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// `B: A` adds a field and a method `A` does not have — the layout difference
// that makes a wrong accept observable.
constexpr const char* kPrelude =
    "module M6023; "
    "public interface IFoo method F(): int; end IFoo; "
    "public class A: IFoo "
    "  public constructor() end constructor; "
    "  public x: int := 1; "
    "  public method F(): int => x; "
    "end A; "
    "public class B: A "
    "  public constructor() end constructor; "
    "  public y: int := 9; "
    "  public method Y(): int => y; "
    "end B; "
    "public class C: A "
    "  public constructor() end constructor; "
    "  public z: int := 7; "
    "end C; ";

// Closes `class P` with a `Main` that would print if the program ever ran.
constexpr const char* kMainTail =
    " public static method Main(): int "
    "   Console.WriteLine(\"RAN\"); return 0; "
    " end Main; end P;";

void expectDowncastRejected(const JitResult& r, const char* pair) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type (§5.1): `Object` "
           "converts implicitly to no other type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, pair));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "which is never implicit"));
}

}  // namespace

// REJECT — THE ISSUE'S REPRO (`x30`), verbatim in shape: an `A` behind
// `Object`, initialising a `B`, then a call that reads `B`'s own field.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           issue_repro_object_holding_base_initialises_derived_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class Program "
        "  public static method Main() "
        "    var o: Object := new A(); "
        "    var b: B := o; "
        "    Console.WriteLine($\"x30 y={b.Y()}\"); "
        "  end Main; "
        "end Program;");
    expectDowncastRejected(r, "`Object` to `B` is a downcast");
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

// ── One row per conversion POSITION, `Object` → `B` ───────────────────────

// REJECT — a local initializer.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_local_initializer_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P "
        "public method M(o: Object): int var b: B := o; "
        "return b.Y(); "
        "end M;"
        + kMainTail);
    expectDowncastRejected(r, "`Object` to `B` is a downcast");
}

// REJECT — an assignment statement.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_assignment_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P "
        "public method M(o: Object, d: B): int var b: B := d; "
        "b := o; "
        "return b.Y(); "
        "end M;"
        + kMainTail);
    expectDowncastRejected(r, "`Object` to `B` is a downcast");
}

// REJECT — a call argument — `Take(o)` for `Take(b: B)`, the issue's second shape.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_call_argument_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P "
        "public method Take(b: B): int => b.Y(); "
        "public method M(o: Object): int => Take(o);"
        + kMainTail);
    expectDowncastRejected(r, "`Object` to `B` is a downcast");
}

// REJECT — a `return`.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_return_statement_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P "
        "public method M(o: Object): string return o; "
        "end M;"
        + kMainTail);
    expectDowncastRejected(r, "`Object` to `string` is a downcast");
}

// REJECT — a field store.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_field_store_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P "
        "public f: B; "
        "public constructor(d: B) f := d; "
        "end constructor; "
        "public method M(o: Object) f := o; "
        "end M;"
        + kMainTail);
    expectDowncastRejected(r, "`Object` to `B` is a downcast");
}

// REJECT — a property store.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_property_store_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P "
        "public property Pr: B; "
        "public constructor(d: B) Pr := d; "
        "end constructor; "
        "public method M(o: Object) Pr := o; "
        "end M;"
        + kMainTail);
    expectDowncastRejected(r, "`Object` to `B` is a downcast");
}

// REJECT — a collection-literal element.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_collection_literal_element_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P "
        "public method M(o: Object): int var xs: Array<B> := [o]; "
        "return xs.Count; "
        "end M;"
        + kMainTail);
    expectDowncastRejected(r, "`Object` to `B` is a downcast");
}

// REJECT — an `if`-expression branch (the join is `Object`; the `Object` branch is the one rejected).
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_if_expression_branch_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P "
        "public method M(o: Object, d: B, c: bool): int var b: B := (if c then o else d); "
        "return b.Y(); "
        "end M;"
        + kMainTail);
    expectDowncastRejected(r, "`Object` to `B` is a downcast");
}

// REJECT — a `case`-expression arm beside a class arm and a bare `null` arm.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_case_expression_arm_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P "
        "public method M(o: Object, d: B, k: int): int var b: B? := (case k of 0 => o, 1 => d, _ => null); "
        "return 0; "
        "end M;"
        + kMainTail);
    expectDowncastRejected(r, "`Object` to `B?` is a downcast");
}

// REJECT — a constructor argument.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_constructor_argument_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class H public constructor(b: B) end constructor; end H; "
        "public class P "
        "public method M(o: Object): int var h := new H(o); "
        "return 0; "
        "end M;"
        + kMainTail);
    expectDowncastRejected(r, "`Object` to `B` is a downcast");
}

// REJECT — a built-in collection method argument (`Stack<B>.Push`).
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_stack_push_argument_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P "
        "public method M(o: Object, st: Stack<B>) st.Push(o); "
        "end M;"
        + kMainTail);
    expectDowncastRejected(r, "`Object` to `B` is a downcast");
}

// REJECT — a dictionary indexer store.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_dictionary_indexer_store_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P "
        "public method M(o: Object, dd: Dictionary<string, B>) dd[\"k\"] := o; "
        "end M;"
        + kMainTail);
    expectDowncastRejected(r, "`Object` to `B` is a downcast");
}

// ── One row per TARGET KIND, at the local-initializer position ────────────

// REJECT — `Object` into `B?` — §5.1 "Nullable conversions" needs `Object` → `B` first.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_nullable_class_target_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P "
        "public method M(o: Object): int var b: B? := o; "
        "return 0; "
        "end M;"
        + kMainTail);
    expectDowncastRejected(r, "`Object` to `B?` is a downcast");
}

// REJECT — `Object?` into `B?` — "`S?` implicitly converts to `T?` whenever `S` implicitly converts to `T`", and `Object` does not convert to `B`.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_nullable_object_into_nullable_class_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P "
        "public method M(o: Object?): int var b: B? := o; "
        "return 0; "
        "end M;"
        + kMainTail);
    expectDowncastRejected(r, "`Object?` to `B?` is a downcast");
}

// REJECT — `Object` into a class that is itself a base (`A`) — still a downcast from `Object`.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_base_class_target_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P "
        "public method M(o: Object): int var a: A := o; "
        "return a.F(); "
        "end M;"
        + kMainTail);
    expectDowncastRejected(r, "`Object` to `A` is a downcast");
}

// REJECT — `Object` into a user interface.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_interface_target_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P "
        "public method M(o: Object): int var f: IFoo := o; "
        "return f.F(); "
        "end M;"
        + kMainTail);
    expectDowncastRejected(r, "`Object` to `IFoo` is a downcast");
}

// REJECT — `Object` into `string` — the issue's `v05` (§4.6: "The compiler does not implicitly convert other types to `string`").
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_string_target_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P "
        "public method M(o: Object): int var s: string := o; "
        "return s.Length; "
        "end M;"
        + kMainTail);
    expectDowncastRejected(r, "`Object` to `string` is a downcast");
}

// REJECT — `Object` into `string?`.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_nullable_string_target_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P "
        "public method M(o: Object): int var s: string? := o; "
        "return 0; "
        "end M;"
        + kMainTail);
    expectDowncastRejected(r, "`Object` to `string?` is a downcast");
}

// REJECT — `Object` into the built-in `Exception`.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           object_source_rejected_at_builtin_exception_target_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P "
        "public method M(o: Object): string var e: Exception := o; "
        "return e.Message; "
        "end M;"
        + kMainTail);
    expectDowncastRejected(r, "`Object` to `Exception` is a downcast");
}

// ── Accept-and-RUN controls: what §5.1 DOES list keeps working ────────────

// ACCEPT — every widening row of §5.1 that involves `Object` or the class
// hierarchy: class → `Object`, class → base, class → interface, boxing an
// `int` and a `string` into `Object` (§8.6), `null` into `B?` and `Object?`,
// and the nullable liftings `B` → `B?` / `B` → `Object?`.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           widening_conversions_still_accepted_and_run_spec_5_1) {
    auto r = runAot(std::string(kPrelude) +
        "public class P public static method Main(): int var b := new B(); "
        "var o: Object := b; "
        "var a: A := b; "
        "var f: IFoo := b; "
        "var fa: IFoo := a; "
        "var i: Object := 42; "
        "var s: Object := \"str\"; "
        "var n: B? := null; "
        "var on: Object? := null; "
        "var nb: B? := b; "
        "var oq: Object? := b; "
        "Console.WriteLine($\"{a.F()} {f.F()} {fa.F()} {i} {s} {n = null} {on = null} {nb <> null} {oq <> null} {o is B}\"); "
        "return 0; "
        "end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1 1 1 42 str True True True True True\n"));
}

// ACCEPT — §12.7: `is T` flow-narrows the carrier, so inside the branch the
// SAME spelling the reject rows use (`var b: B := o`) is an identity
// conversion. A fix that keyed on the DECLARED type of `o` would reject this.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           is_narrowed_carrier_initialises_the_narrowed_type_spec_12_7) {
    auto r = runAot(std::string(kPrelude) +
        "public class P public static method Main(): int var o: Object := new B(); "
        "if o is B then var b: B := o; "
        "Console.WriteLine($\"B {b.Y()}\"); "
        "end if; "
        "if o is A then var a: A := o; "
        "Console.WriteLine($\"A {a.F()}\"); "
        "end if; "
        "var t: Object := \"str\"; "
        "if t is string then var s: string := t; "
        "Console.WriteLine($\"S {s.Length}\"); "
        "end if; "
        "if t is B then Console.WriteLine(\"WRONG\"); "
        "else Console.WriteLine(\"not B\"); "
        "end if; "
        "return 0; "
        "end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("B 9\nA 1\nS 3\nnot B\n"));
}

// ACCEPT — §12.7 over a BUILT-IN EXCEPTION test type, from an `Object`
// carrier. The built-in exception classes carry no TypeSymbol and used to
// narrow nothing; `Object` → `IOException` is rejected by this fix, so the
// narrowing has to exist for §12.7's spelling to keep compiling. The message
// is read THROUGH the narrowed `IOException` local.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           is_narrows_object_carrier_to_builtin_exception_spec_12_7) {
    auto r = runAot(
        "module M6023N; "
        "public class P "
        "  public static method Probe(o: Object) "
        "    if o is IOException then "
        "      var io: IOException := o; "
        "      var ex: Exception := o; "
        "      Console.WriteLine(\"io \" + io.Message + \" \" + ex.Message); "
        "    else "
        "      Console.WriteLine(\"not io\"); "
        "    end if; "
        "  end Probe; "
        "  public static method Main(): int "
        "    try throw new IOException(\"disk\"); "
        "    catch var e: Exception => Probe(e), end try; "
        "    try throw new Exception(\"plain\"); "
        "    catch var e: Exception => Probe(e), end try; "
        "    Probe(\"a string\"); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("io disk disk\nnot io\nnot io\n"));
}

// ACCEPT — the issue's two programs respelled with `as` (§12.7 / §17.4). The
// `A` behind `o` is NOT a `B`, so `o as B` yields null and the program says so
// instead of printing `x30 y=0`; `o as A` and `t as string` succeed and yield
// the object.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           as_cast_is_the_checked_spelling_and_yields_null_on_mismatch_spec_12_7) {
    auto r = runAot(std::string(kPrelude) +
        "public class P public static method Main(): int var o: Object := new A(); "
        "var b := o as B; "
        "if b = null then Console.WriteLine(\"x30 not a B\"); "
        "else Console.WriteLine($\"x30 y={b.Y()}\"); "
        "end if; "
        "var a := o as A; "
        "if a = null then Console.WriteLine(\"WRONG\"); "
        "else Console.WriteLine($\"A {a.F()}\"); "
        "end if; "
        "var t: Object := \"str\"; "
        "var s := t as string; "
        "if s = null then Console.WriteLine(\"WRONG\"); "
        "else Console.WriteLine($\"S {s}\"); "
        "end if; "
        "var u := o as string; "
        "if u = null then Console.WriteLine(\"v05 not a string\"); "
        "else Console.WriteLine(\"WRONG\"); "
        "end if; "
        "return 0; "
        "end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("x30 not a B\nA 1\nS str\nv05 not a string\n"));
}

// ACCEPT — §20: "A bare `null` branch … target-types to the nullable form of
// the other branches' common type". With class arms the join is typed `B?`
// (#6086, tests/sema/case_null_arm_join_6086_test.cpp) and never reaches the
// `Object`-source rejection; this row keeps the conversion-site view of it
// next to that rejection. `M(0)`/`M(1)`/`M(2)` take a different arm each and
// the sum encodes which results were non-null.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           null_arm_join_of_class_arms_still_reaches_nullable_class_spec_20) {
    auto r = runAot(std::string(kPrelude) +
        "public class P public bb: B; "
        "public aa: A; "
        "public constructor(b: B, a: A) bb := b; "
        "aa := a; "
        "end constructor; "
        "public method Take(q: B?): int => (if q = null then -1 else q.Y()); "
        "public method M(k: int): int   var r1: B? := (case k of 0 => bb, _ => null); "
        "  var r2: A? := (case k of 0 => bb, 1 => aa, _ => null); "
        "  var r3: B? := (if k = 0 then bb else null); "
        "  var r4: IFoo? := (case k of 0 => bb, _ => null); "
        "  var n := Take((case k of 0 => bb, _ => null)); "
        "  if r1 <> null then n := n + 10; "
        "end if; "
        "  if r2 <> null then n := n + 100; "
        "end if; "
        "  if r3 <> null then n := n + 1000; "
        "end if; "
        "  if r4 <> null then n := n + 10000; "
        "end if; "
        "  return n; "
        "end M; "
        "public static method Main(): int   var b := new B(); "
        "var a := new A(); "
        "var p := new P(b, a); "
        "  Console.WriteLine($\"{p.M(0)} {p.M(1)} {p.M(2)}\"); "
        "return 0; "
        "end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("11119 99 -1\n"));
}

// ACCEPT — §22.6 ranking is untouched (the fix is in the conversion-site
// predicate, not in overload resolution): an `Object` argument selects the
// `Object` overload and never the `A`/`B` ones, a `B` selects `B`, an `A`
// selects `A`, and an interface / `string` / boxed `int` argument selects
// `Object`.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           overload_ranking_with_object_argument_unchanged_spec_22_6) {
    auto r = runAot(std::string(kPrelude) +
        "public class P public static method Pick(o: Object): string => \"object\"; "
        "public static method Pick(a: A): string => \"A\"; "
        "public static method Pick(b: B): string => \"B\"; "
        "public static method Main(): int   var b := new B(); "
        "var a := new A(); "
        "var o: Object := b; "
        "var f: IFoo := b; "
        "  Console.WriteLine($\"{Pick(o)} {Pick(a)} {Pick(b)} {Pick(f)} {Pick(\"s\")} {Pick(7)}\"); "
        "return 0; "
        "end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("object A B object object object\n"));
}

// ACCEPT — two expressions recorded `Object?` only because every value they
// can produce is the bare `null`: a `case` whose arms are all `null`, and a
// §17.15 binding atom whose body is `null`. Both reach the `Object`-source arm
// with a nullable target and are judged by their leaves — each leaf is the
// `null` literal — not rejected as `Object?` → `B?` / `string?`. (MEASURED:
// with the leaf walk disabled this row fails.)
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           all_null_case_and_null_binding_atom_still_accepted_spec_5_1) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int): string
    var c: B? := (case k of 0 => null, _ => null);
    var cs: string? := (case k of 0 => null, _ => null);
    var n: B? := (var t := 1; null);
    var s: string? := (var t := 1; null);
    return $"{c = null} {cs = null} {n = null} {s = null}";
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(0)}|{T(1)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("True True True True|True True True True\n"));
}

// ── Pending #6083: the two kept shapes, and their limits ─────────────────

// `if o is not B then return …; end;` — on the surviving path `o` is still
// typed `Object` (no member access: see the reject row below), but `o` → `B` is
// not rejected.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           pending_ruling_6083_exiting_is_not_return_keeps_object_to_tested_class_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: Object): int
    if o is not B then return -1; end if;
    var b: B := o;
    return b.y;
  end T;
  public static method Main(): int
    var b: Object := new B();
    var a: Object := new A();
    Console.WriteLine($"{T(b)} {T(a)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -1\n"));
}

// The same with `continue` as the exit, inside a loop body.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           pending_ruling_6083_exiting_is_not_continue_in_loop_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: Object): int
    var i := 0;
    while i < 3 do
      i := i + 1;
      if o is not B then continue; end if;
      var b: B := o;
      return b.y;
    end while;
    return -1;
  end T;
  public static method Main(): int
    var b: Object := new B();
    var a: Object := new A();
    Console.WriteLine($"{T(b)} {T(a)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -1\n"));
}

// An `or`-joined exit condition (false only when every operand is false), with
// a nullable `Object?` carrier into a nullable target, and a wider target (`A`,
// a base of the tested `B`).
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           pending_ruling_6083_exiting_or_joined_is_not_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: Object, f: bool): int
    if f or o is not B then return -4; end if;
    var a: A := o;
    return a.x;
  end T;
  public static method U(o: Object?): int
    if o is not B then return -5; end if;
    var b: B? := o;
    return (if b = null then -6 else b.y);
  end U;
  public static method Main(): int
    var b: Object := new B();
    var a: Object := new A();
    Console.WriteLine($"{T(b, false)} {T(b, true)} {T(a, false)} {U(b)} {U(a)} {U(null)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1 -4 -4 9 -5 -5\n"));
}

// A bare type-pattern arm `B => …` over an `Object` scrutinee: inside the arm
// `o` → `B` is not rejected.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           pending_ruling_6083_type_pattern_arm_statement_form_spec_19_1) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: Object): int
    case o of
      B => do var b: B := o; return b.y; end,
      _ => do return -9; end
    end case;
  end T;
  public static method Main(): int
    var b: Object := new B();
    var a: Object := new A();
    Console.WriteLine($"{T(b)} {T(a)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -9\n"));
}

// The expression form, with the carrier passed as a `B` argument, and a typed
// binding arm `var q: B` whose body converts the SCRUTINEE.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           pending_ruling_6083_type_pattern_arm_expression_form_spec_19_1) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method TakeB(b: B): int => b.y;
  public static method T(o: Object): int => (case o of B => TakeB(o), _ => -9);
  public static method U(o: Object): int => (case o of var q: B => TakeB(o) + q.y, _ => -9);
  public static method Main(): int
    var b: Object := new B();
    var a: Object := new A();
    Console.WriteLine($"{T(b)} {T(a)} {U(b)} {U(a)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -9 18 -9\n"));
}

// REJECT — NOT pending. §7.4: "`not (x is T)` … narrows neither branch", and a
// type fact is "never" established "by `not`-composition".
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           not_composed_exit_is_rejected_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: Object): int
    if not (o is B) then return -1; end if;
    var b: B := o;
    return b.y;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` converts implicitly to no other type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` to `B` is a downcast, which is never implicit"));
}

// REJECT — the kept shape admits the TESTED type (and its bases) only: after
// `is not B` exits, `o` → `C` is still a downcast nothing has checked.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           exit_test_does_not_cover_a_sibling_class_spec_5_1) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: Object): int
    if o is not B then return -1; end if;
    var c: C := o;
    return c.z;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` to `C` is a downcast, which is never implicit"));
}

// REJECT — an assignment to the carrier ends it. (Before #6023 this compiled
// and read `y` off an `A`.)
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           exit_test_is_forgotten_when_the_carrier_is_reassigned_spec_5_1) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o0: Object): int
    var o: Object := o0;
    if o is not B then return -1; end if;
    o := new A();
    var b: B := o;
    return b.y;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` converts implicitly to no other type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` to `B` is a downcast, which is never implicit"));
}

// ACCEPT + RUN (pending #6083, shape 1). The `else` of an exiting `is not`
// test falls through; the path after the `if` is still only the one the test
// failed on. MEASURED on base: compiles, prints the same line.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           pending_ruling_6083_exit_test_with_non_exiting_else_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: Object): int
    if o is not B then return -1; else Console.Write(""); end if;
    var b: B := o;
    return b.y;
  end T;
  public static method Main(): int
    var t := 0;
    for var i in 0..60 do t := t + T(new B()) + T(new A()); end for;
    Console.WriteLine($"{T(new B())} {T(new A())} {t}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -1 488\n"));
}

// ACCEPT + RUN (pending #6083, shape 1). An `elif` that leaves too: every
// branch exits, so the only path after the `if` is the one both conditions were
// false on.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           pending_ruling_6083_exit_test_followed_by_exiting_elif_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int, o: Object): int
    if o is not B then return -1; elif k = 7 then return -2; end if;
    var b: B := o;
    return b.y;
  end T;
  public static method Main(): int
    var t := 0;
    for var i in 0..60 do t := t + T(0, new B()) + T(7, new B()) + T(0, new A()); end for;
    Console.WriteLine($"{T(0, new B())} {T(7, new B())} {T(0, new A())} {t}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -2 -1 366\n"));
}

// ACCEPT + RUN (pending #6083, shape 1). The test is the `elif`'s.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           pending_ruling_6083_exit_test_in_the_elif_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int, o: Object): int
    if k = 7 then return -2; elif o is not B then return -1; end if;
    var b: B := o;
    return b.y;
  end T;
  public static method Main(): int
    var t := 0;
    for var i in 0..60 do t := t + T(0, new B()) + T(7, new B()) + T(0, new A()); end for;
    Console.WriteLine($"{T(0, new B())} {T(7, new B())} {T(0, new A())} {t}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -2 -1 366\n"));
}

// ACCEPT + RUN (pending #6083, shape 1). The positive spelling: the `else`
// leaves, so the path after the `if` is the one `o is B` held on.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           pending_ruling_6083_positive_test_with_exiting_else_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: Object): int
    if o is B then Console.Write(""); else return -1; end if;
    var b: B := o;
    return b.y;
  end T;
  public static method Main(): int
    var t := 0;
    for var i in 0..60 do t := t + T(new B()) + T(new A()); end for;
    Console.WriteLine($"{T(new B())} {T(new A())} {t}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -1 488\n"));
}

// ACCEPT + RUN (pending #6083, shape 1). `and`: true only when every operand
// is.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           pending_ruling_6083_positive_and_joined_test_with_exiting_else_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int, o: Object): int
    if o is B and k >= 0 then Console.Write(""); else return -1; end if;
    var b: B := o;
    return b.y;
  end T;
  public static method Main(): int
    var t := 0;
    for var i in 0..60 do t := t + T(0, new B()) + T(-1, new B()) + T(0, new A()); end for;
    Console.WriteLine($"{T(0, new B())} {T(-1, new B())} {T(0, new A())} {t}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -1 -1 427\n"));
}

// ACCEPT + RUN (pending #6083, shape 1). Two successive exit tests of one
// carrier: the second does not replace the first — `q` reaches a `B` slot (first
// test) and an `A` slot (either).
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           pending_ruling_6083_two_successive_exit_tests_keep_both_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: Object): int
    var q: Object := o;
    if q is not B then return -1; end if;
    if q is not A then return -2; end if;
    var b: B := q;
    var a: A := q;
    return b.y + a.x;
  end T;
  public static method Main(): int
    var t := 0;
    for var i in 0..60 do t := t + T(new B()) + T(new A()); end for;
    Console.WriteLine($"{T(new B())} {T(new A())} {t}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10 -1 549\n"));
}

// ACCEPT + RUN (pending #6083, shape 1). The test happened on every path that
// reaches the statement after the outer `if`.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           pending_ruling_6083_exit_test_in_every_surviving_branch_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int, o: Object): int
    if k = 1 then
      if o is not B then return -1; end if;
    else
      if o is not B then return -2; end if;
    end if;
    var b: B := o;
    return b.y;
  end T;
  public static method Main(): int
    var t := 0;
    for var i in 0..60 do t := t + T(1, new B()) + T(0, new B()) + T(1, new A()) + T(0, new A()); end for;
    Console.WriteLine($"{T(1, new B())} {T(0, new B())} {T(1, new A())} {T(0, new A())} {t}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 9 -1 -2 915\n"));
}

// REJECT. A reassignment inside an `else`-less `if` ends the record on that
// path, so it is not in force after the join. MEASURED on base: this compiled and
// printed `9 0` for `T(0, b, a)` / `T(1, b, a)` — the second call read `B.y` out
// of an `A` (the hole itself), so the rejection takes nothing that ran correctly.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           exit_test_is_forgotten_when_reassigned_in_a_nested_if_spec_5_1) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int, o: Object, alt: Object): int
    var q: Object := o;
    if q is not B then return -1; end if;
    if k = 1 then q := alt; end if;
    var b: B := q;
    return b.y;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` converts implicitly to no other type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` to `B` is a downcast, which is never implicit"));
}

// REJECT. The same with an explicit `else`.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           exit_test_is_forgotten_when_reassigned_in_one_branch_of_an_if_else_spec_5_1) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int, o: Object, alt: Object): int
    var q: Object := o;
    if q is not B then return -1; end if;
    if k = 1 then q := alt; else Console.Write(""); end if;
    var b: B := q;
    return b.y;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` converts implicitly to no other type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` to `B` is a downcast, which is never implicit"));
}

// REJECT. The same inside a type-pattern arm (shape 2).
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           type_pattern_arm_fact_is_forgotten_when_reassigned_in_a_nested_if_spec_5_1) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int, o: Object, alt: Object): int
    var q: Object := o;
    case q of
      B => do if k = 1 then q := alt; end if; var b: B := q; return b.y; end,
      _ => do return -9; end
    end case;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` converts implicitly to no other type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` to `B` is a downcast, which is never implicit"));
}

// REJECT. The `else` reaches the statement after the `if` untested.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           positive_test_whose_else_falls_through_is_rejected_spec_5_1) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: Object): int
    if o is B then Console.Write(""); else Console.Write(""); end if;
    var b: B := o;
    return b.y;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` converts implicitly to no other type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` to `B` is a downcast, which is never implicit"));
}

// REJECT. The `if` branch falls through without any test of `o`.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           elif_chain_with_an_untested_surviving_branch_is_rejected_spec_5_1) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int, o: Object): int
    if k = 7 then Console.Write(""); elif o is not B then return -1; end if;
    var b: B := o;
    return b.y;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` converts implicitly to no other type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` to `B` is a downcast, which is never implicit"));
}

// REJECT. `or` on the true path proves neither operand.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           positive_or_joined_test_with_exiting_else_is_rejected_spec_5_1) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(k: int, o: Object): int
    if o is B or k = 3 then Console.Write(""); else return -1; end if;
    var b: B := o;
    return b.y;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` converts implicitly to no other type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` to `B` is a downcast, which is never implicit"));
}

// REJECT. §7.4: a type fact is never established "by `not`-composition".
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           not_composed_positive_test_with_exiting_else_is_rejected_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: Object): int
    if not (o is not B) then Console.Write(""); else return -1; end if;
    var b: B := o;
    return b.y;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` converts implicitly to no other type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` to `B` is a downcast, which is never implicit"));
}

// REJECT — the record is block-scoped: an exit test inside an inner block says
// nothing after that block.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           exit_test_inside_a_block_does_not_leave_it_spec_5_1) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: Object, f: bool): int
    if f then
      if o is not B then return -1; end if;
    end if;
    var b: B := o;
    return b.y;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` converts implicitly to no other type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` to `B` is a downcast, which is never implicit"));
}

// REJECT — `break` leaves the loop with `o` NOT a `B`; after the loop the
// conversion is an unchecked downcast.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           break_exit_test_does_not_survive_the_loop_spec_5_1) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: Object): int
    var i := 0;
    while i < 3 do
      i := i + 1;
      if o is not B then break; end if;
    end while;
    var b: B := o;
    return b.y;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` converts implicitly to no other type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` to `B` is a downcast, which is never implicit"));
}

// REJECT — the arm's record ends with the arm: the `_` arm and the code after
// the `case` both see a plain `Object`.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           type_pattern_arm_does_not_cover_the_other_arms_spec_5_1) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: Object): int
    case o of
      B => do Console.WriteLine("b"); end,
      _ => do var b: B := o; return b.y; end
    end case;
    return 0;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` converts implicitly to no other type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` to `B` is a downcast, which is never implicit"));
}

// REJECT — after `end case`.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           type_pattern_arm_does_not_survive_the_case_spec_5_1) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: Object): int
    case o of
      B => do Console.WriteLine("b"); end,
      _ => do Console.WriteLine("n"); end
    end case;
    var b: B := o;
    return b.y;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` converts implicitly to no other type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object` to `B` is a downcast, which is never implicit"));
}

// REJECT — a user-CLASS carrier was rejected here before #6023 and still is:
// the kept shapes are `Object` carriers only.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           class_carrier_exit_test_stays_rejected_spec_5_1) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(a: A): int
    if a is not B then return -1; end if;
    var b: B := a;
    return b.y;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a base-to-derived (downcast) conversion is not implicit"));
}

// ---------------------------------------------------------------------------
// The pending-#6083 record is a flow fact like any other: it ends where a
// narrowing would (§7.4 "How narrowing is invalidated", §12.7, §21.7,
// v0.338). These rows pin the shapes of #6113 / #6114 / #6115 / #6140 for the
// record — each compiled on the first version of this fix and passed an `A`
// where a `B` was required — and the accept rows that sit next to them.

namespace {

JitResult runRecord(const char* body) {
    return runAot(std::string(kPrelude) + R"bzl(public class P
  public static method Take(b: B): int => b.y;
  public static method Rs(ref q: Object, alt: Object): bool
    q := alt;
    return true;
  end Rs;
  public static method T(k: int, o: Object, alt: Object): int
    var q: Object := o; var t := 0; var i := 0;
    )bzl" + body + R"bzl(
  end T;
  public static method Main(): int
    var bb := new B(); var aa := new A();
    Console.WriteLine($"{T(0, bb, aa)} {T(0, aa, bb)}");
    return 0;
  end Main;
end P;
)bzl");
}

void expectRecordLostInLoop(const JitResult& r) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`q` is not known to hold the tested type here: the type test was "
           "made before the loop"));
}

}  // namespace

// #6114 for the record an exit test leaves: the test was made before the
// loop, and the body reassigns the carrier below the use.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           exit_test_before_a_while_that_reassigns_below_the_use_spec_12_7) {
    auto r = runRecord(R"bzl(if q is not B then return -1; end if; while i < 2 do t := t + Take(q); q := alt; i := i + 1; end while; return t;)bzl");
    expectRecordLostInLoop(r);
}

BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           exit_test_before_a_for_that_reassigns_below_the_use_spec_12_7) {
    auto r = runRecord(R"bzl(if q is not B then return -1; end if; for var j in 0..<2 do t := t + Take(q); q := alt; end for; return t;)bzl");
    expectRecordLostInLoop(r);
}

BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           exit_test_before_a_loop_with_a_ref_argument_below_the_use_spec_7_4) {
    auto r = runRecord(R"bzl(if q is not B then return -1; end if; while i < 2 do t := t + Take(q); Rs(ref q, alt); i := i + 1; end while; return t;)bzl");
    expectRecordLostInLoop(r);
}

// The same for the record a type-pattern arm holds.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           type_pattern_arm_with_a_loop_that_reassigns_below_the_use_spec_12_7) {
    auto r = runRecord(R"bzl(case q of B => do while i < 2 do t := t + Take(q); q := alt; i := i + 1; end while; return t; end, _ => do return -1; end end case; return -7;)bzl");
    expectRecordLostInLoop(r);
}

// #6115 for the record: a `ref` argument after the test, in the same condition.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           ref_argument_later_in_the_exit_test_condition_spec_7_4) {
    auto r = runRecord(R"bzl(if q is not B or Rs(ref q, alt) then return -1; end if; return Take(q);)bzl");
    expectDowncastRejected(r, "`Object` to `B` is a downcast");
}

// #6140 for the record: the branch does not exit.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           exit_branch_that_is_a_loop_a_break_leaves_spec_21_7) {
    auto r = runRecord(R"bzl(if q is not B then while true do break; end while; end if; return Take(q);)bzl");
    expectDowncastRejected(r, "`Object` to `B` is a downcast");
}

// #6113 for the record.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           reassignment_under_an_if_after_the_exit_test_spec_12_7) {
    auto r = runRecord(R"bzl(if q is not B then return -1; end if; if k = 0 then q := alt; end if; return Take(q);)bzl");
    expectDowncastRejected(r, "`Object` to `B` is a downcast");
}

// ACCEPT. The test is made on every iteration, before the use.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           exit_test_inside_the_loop_on_every_iteration_spec_12_7) {
    auto r = runRecord(R"bzl(while i < 2 do if q is not B then break; end if; t := t + Take(q); q := alt; i := i + 1; end while; return t;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 0\n"));
}

// ACCEPT.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           continue_test_inside_the_loop_on_every_iteration_spec_12_7) {
    auto r = runRecord(R"bzl(while i < 2 do i := i + 1; if q is not B then continue; end if; t := t + Take(q); q := alt; end while; return t;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 0\n"));
}

// ACCEPT. Nothing in the loop invalidates the record.
BEZEL_TEST(sema_object_downcast_not_implicit_6023,
           exit_test_before_a_loop_that_leaves_the_carrier_alone_spec_12_7) {
    auto r = runRecord(R"bzl(if q is not B then return -1; end if; while i < 2 do t := t + Take(q); i := i + 1; end while; return t;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("18 -1\n"));
}
