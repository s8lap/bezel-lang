// fix #5659 — spec §9.7 *borrow-erasing position* / §8.6 (v0.330).
//
// ONE spec bullet, the twin of §28.3's *Set-erasing positions* for a function
// value (spec line 6754). §9.7 line 1455, verbatim:
//
//   "A value that holds borrows may not reach a **borrow-erasing position**: a
//    position declared `Object`, or an interface type other than
//    `IReadOnlySpan<TElement>` — a parameter, plain or `scoped`, a local, a
//    field, a record component, a choice payload, an inline-array slot, a
//    collection slot or collection-literal element, a `ref` or `out`
//    location, a return slot — or a position declared at a bare type parameter
//    that the instantiation substitutes with such a type. The conversion —
//    §8.6's box, or the reference conversion of an `IReadOnlySpan<TElement>`
//    value to `Object` or another interface type, implicit or by an `as`
//    (§17.4) or a typed-pattern binding (§19.3) at such a type — is a **compile
//    error at the conversion site**. … The receiver of an extension call whose
//    head is `Object` or another interface type is borrow-erasing: the
//    extension body reads `this` at the head type and may pass it on to a
//    parameter that stores it."
//
// The bullet names FOUR routes and one position no conversion-site check
// reaches. MEASURED on 35759e44 (staging), the compiler enforced only the
// first, and only where a `checkAssignable` happened to run:
//
//   route                                    shape                35759e44
//   ---------------------------------------  -------------------  ---------
//   implicit, at a parameter                 `Sink.K(s)`          REJECTED
//   `as` (§17.4)                             `s as Object`        COMPILED
//   typed-pattern binding (§19.3)            `case s of var o: Object` COMPILED
//   `Console.Write(span)` at `Object`        `Console.WriteLine(s)`    COMPILED
//   `StringBuilder.Append(span)` at `Object` `sb.Append(s)`           IR CRASH
//   extension receiver, head an interface    `t.Leak(k)`          COMPILED
//
// The `as` and typed-pattern arms ran their own §17.4 / §19.3 checks and never
// read the conversion's SOURCE type; the two builtin calls carry no
// MethodSymbol at all, so no overload signature is reconstructed and the
// imported per-argument `checkAssignable` never runs for them; and an
// extension's `this` is not in `md->params`, so `checkArgsAgainstParams` never
// sees it. `sb.Append(s)` was worse than admitted: it reached codegen and
// aborted the compiler with "SExt only operates on integer … IR verification
// failed".
//
// All six now report ONE diagnostic from ONE predicate
// (ExprTC::checkBorrowErasingPosition), so there is a single needle to pin.
//
// THE ACCEPT-CONTROLS ARE HALF THIS FILE, and they are the more important
// half. §28.3 states the polarity of the twin rule — "a function value with a
// non-empty source set is rejected, and a capture-free one is admitted" — and
// §9.7's own is the same shape: a value that does NOT hold borrows reaching
// `Object` stays ADMITTED, and §9.7 names exactly TWO admitted positions ("the
// receiver of a member call, or of an extension call whose head is
// `IReadOnlySpan<TElement>`, and a position declared `IReadOnlySpan<TElement>`").
// A check that rejected every `Object`/interface conversion would pass a suite
// of reject-rows alone. Each admitted position below is pinned here, together
// with §9.7's own printed remedies.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// §9.7's single diagnostic, as it reads from EVERY route. The needle is the
// RULE, not a route: `checkBorrowErasingPosition` is one function called from
// all six sites, so one phrase pins the rule and the `§9.7` citation pins the
// section it comes from.
bool rejectedBorrowErasing(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(
                "a value that holds borrows may not reach a "
                "borrow-erasing position") != std::string::npos &&
            d.message.find("§9.7") != std::string::npos &&
            d.message.find("borrow-bearing type") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// =====================================================================
// ROUTE 1 — the implicit conversion at a parameter. This one already
// worked on 35759e44; it is pinned as the baseline the other five are
// measured against (the `#5659` block's pre-existing home is
// `ExprTC::checkAssignable`).
// =====================================================================

BEZEL_TEST(sema_borrow_erasing_position_5659,
           span_at_object_parameter_rejected_spec_9_7) {
    JitResult r = runAot(
        "module M; "
        "class Sink public static method K(x: Object?) end K; end Sink; "
        "class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(4); "
        "    var s := buf.AsSpan(); "
        "    Sink.K(s); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
}

// The `s8_6_0007` program: a SPAN (a class-backed builtin view), not an
// `IReadOnlySpan`. §9.7 reads a value declared at `IReadOnlySpan<TElement>` as
// holding the span's borrows, and `isBorrowBearingRecordOrSpan` answers the
// same for the concrete views — the two legs of one predicate.
BEZEL_TEST(sema_borrow_erasing_position_5659,
           concrete_span_at_object_parameter_rejected_spec_9_7) {
    JitResult r = runAot(
        "module M; "
        "class Sink public static method K(x: Object?) end K; end Sink; "
        "class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(4); "
        "    var s := buf.AsSpan(); "
        "    Sink.K(s); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
}

// =====================================================================
// ROUTE 2 — `as` (§17.4). The `s8_6_0009` program.
// =====================================================================

BEZEL_TEST(sema_borrow_erasing_position_5659,
           as_to_object_rejected_spec_9_7_17_4) {
    JitResult r = runAot(
        "module M; "
        "class Sink public static method K(x: Object?) end K; end Sink; "
        "class C "
        "  public static method Main(s: IReadOnlySpan<int>): int "
        "    Sink.K(s as Object); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
}

// §9.7 names `as` at "an interface type other than `IReadOnlySpan<TElement>`"
// as squarely as at `Object`. Same route, different target kind — pinned so a
// fix that only recognized the `Object` primitive cannot pass.
BEZEL_TEST(sema_borrow_erasing_position_5659,
           as_to_non_span_interface_rejected_spec_9_7_17_4) {
    JitResult r = runAot(
        "module M; "
        "public interface IShow "
        "  method Name(): string; "
        "end IShow; "
        "class C "
        "  public static method Main(s: IReadOnlySpan<int>): int "
        "    var i := s as IShow; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
}

// =====================================================================
// ROUTE 3 — the typed-pattern binding (§19.3). Two copies of the
// scope-building block exist — the CaseExpr arm and the CaseStmt arm — and
// BOTH needed the call; fixing only the first leaves the second admitted.
// These two rows are that twin, pinned separately for exactly that reason.
// =====================================================================

BEZEL_TEST(sema_borrow_erasing_position_5659,
           typed_pattern_binding_at_object_rejected_spec_9_7_19_3) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Main(s: IReadOnlySpan<int>): int "
        "    case s of "
        "      var o: Object => Console.WriteLine(\"hit\"), "
        "      _ => Console.WriteLine(\"miss\") "
        "    end case; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
}

// The CaseStmt twin of the row above — a `case` STATEMENT (`end;`, comma-
// separated arms per §20), not a `case … end case` expression. Same binding,
// same declared type, same §9.7 violation, and it was a SEPARATE copy of the
// block in the compiler.
BEZEL_TEST(sema_borrow_erasing_position_5659,
           typed_pattern_binding_at_object_in_case_stmt_rejected_spec_9_7_19_3) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Main(s: IReadOnlySpan<int>): int "
        "    case s of "
        "      var o: Object => Console.WriteLine(\"hit\"), "
        "      _ => Console.WriteLine(\"miss\") "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
}

// =====================================================================
// ROUTE 4 — `Console.Write` / `Console.WriteLine` at their `Object`
// parameter (stdlib §2.1). The `s8_6_0007` program.
//
// §9.7 names the short list outright: "rejects the same short list of
// non-storing calls: `Console.WriteLine(span)` and `StringBuilder.Append(span)`
// at their `Object` parameters".
// =====================================================================

BEZEL_TEST(sema_borrow_erasing_position_5659,
           span_at_consol_writeline_object_parameter_rejected_spec_9_7_8_6) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(4); "
        "    var s := buf.AsSpan(); "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
}

// The SIBLING of the row above. `Console.Write` is declared at the identical
// `Object` parameter (stdlib §2.1) and reaches it through the identical
// synthetic no-MethodSymbol path, so §9.7's general rule covers it; the two
// are pinned together because a fix that named only `WriteLine` would leave
// `Write` open.
BEZEL_TEST(sema_borrow_erasing_position_5659,
           span_at_console_write_object_parameter_rejected_spec_9_7_8_6) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(4); "
        "    var s := buf.AsSpan(); "
        "    Console.Write(s); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
}

// =====================================================================
// ROUTE 5 — `StringBuilder.Append` at its `Object` parameter (stdlib §3.1).
//
// This one did not merely compile on 35759e44: it reached codegen and killed
// the compiler. MEASURED there:
//
//   SExt only operates on integer
//     %14 = sext %bz.arrayspan %13 to i64
//   bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// §9.7 requires "a compile error at the conversion site", so a sema rejection
// is also the fix for the malformed IR: the program never reaches the backend.
// =====================================================================

BEZEL_TEST(sema_borrow_erasing_position_5659,
           span_at_stringbuilder_append_object_parameter_rejected_spec_9_7_8_6) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(4); "
        "    var s := buf.AsSpan(); "
        "    var sb := new StringBuilder(); "
        "    sb.Append(s); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
}

// A borrow-bearing RECORD at the same `Object` parameter — §9.7's rule is
// about "a value that holds borrows", and a record whose field carries a
// class-typed borrow is the case the rule's own rationale (§9.7's instance-
// field bullet) is written for.
BEZEL_TEST(sema_borrow_erasing_position_5659,
           borrow_bearing_record_at_console_writeline_rejected_spec_9_7_8_6) {
    JitResult r = runAot(
        "module M; "
        "class Item public constructor() end constructor; end Item; "
        "record Tagged(Src: Item); "
        "class C "
        "  public static method Main(): int "
        "    using var it := new Item(); "
        "    var t := new Tagged(it); "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
}

// =====================================================================
// POSITION — the receiver of an extension call whose head is `Object` or
// another interface type. The `s8_6_0010` program.
//
// This is the one position §9.7 calls out that no conversion-site check
// reaches at all: an extension's `this` is not in `md->params`, so
// `checkArgsAgainstParams` never sees it, and the extension body is checked
// once at the DECLARATION against the parameter type — never re-checked at the
// call site, which is where §9.7 puts the check ("The check is the caller's,
// over the callee's declared signature, per instantiation (§3.6 rule 2)").
// =====================================================================

BEZEL_TEST(sema_borrow_erasing_position_5659,
           borrow_bearing_record_as_extension_receiver_rejected_spec_9_7_26_5) {
    JitResult r = runAot(
        "module M; "
        "class Item public constructor() end constructor; "
        "  public method Size(): int => 1; "
        "end Item; "
        "public interface IShow method Name(): string; end IShow; "
        "class Keeper "
        "  private _kept: IShow?; "
        "  public constructor() end constructor; "
        "  public method Put(s: IShow) _kept := s; end Put; "
        "end Keeper; "
        "public extension IShow "
        "  public method Leak(k: Keeper) => k.Put(this); "
        "end extension; "
        "public record Tagged(Src: Item): IShow "
        "  public method Name(): string => \"tagged\"; "
        "end Tagged; "
        "class C "
        "  public static method Main(k: Keeper): int "
        "    using var it := new Item(); "
        "    var t := new Tagged(it); "
        "    t.Leak(k); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
}

// The same position, with an extension body that does NOT store `this` — it
// only calls a member on it. §9.7 rejects on the declared SHAPE, not on what
// the body happens to do with the box: "the extension body reads `this` at the
// head type and MAY pass it on to a parameter that stores it", and "The check
// is the caller's, over the callee's declared signature". A check that
// analysed bodies would admit this program; the spec does not, and this row is
// what pins the difference.
BEZEL_TEST(sema_borrow_erasing_position_5659,
           extension_receiver_rejected_even_when_body_does_not_store_spec_9_7_26_5) {
    JitResult r = runAot(
        "module M; "
        "class Item public constructor() end constructor; end Item; "
        "public interface IShow "
        "  method Name(): string; "
        "end IShow; "
        "public extension IShow "
        "  public method Describe(): string => this.Name(); "
        "end extension; "
        "public record Tagged(Src: Item): IShow "
        "  public method Name(): string => \"tagged\"; "
        "end Tagged; "
        "class C "
        "  public static method Main(): int "
        "    using var it := new Item(); "
        "    var t := new Tagged(it); "
        "    var d := t.Describe(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
}

// =====================================================================
// ACCEPT-CONTROLS — §9.7's two admitted positions, its printed remedies,
// and the polarity §28.3 states for the twin rule. A rejection too wide is a
// worse defect than the one being fixed, and every row below compiled on
// 35759e44.
// =====================================================================

// A `string` at an `Object` parameter. §9.7 reads only a value that HOLDS
// BORROWS; a `string` is runtime-managed (§28.9) and has unbounded lifetime by
// construction, so it is admitted. This is the row that catches a check which
// rejects every `Object` conversion — the failure mode §28.3's "a capture-free
// one is admitted" warns about for the twin rule.
BEZEL_TEST(sema_borrow_erasing_position_5659,
           string_at_object_parameter_accepted_spec_9_7_28_9) {
    JitResult r = runAot(
        "module M; "
        "class Sink public static method K(x: Object?): string "
        "  if x = null then return \"null\"; end; "
        "  return x.ToString(); "
        "end K; end Sink; "
        "class C "
        "  public static method Main(): int "
        "    if Sink.K(\"hello\") <> \"hello\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A NON-borrow-bearing record at an `Object` parameter: §9.7's field-level
// predicate reads DECLARED field types, and `string`/runtime-managed fields do
// not make a record borrow-bearing. `Plain(N: int)` holds nothing.
BEZEL_TEST(sema_borrow_erasing_position_5659,
           non_borrow_bearing_record_at_object_parameter_accepted_spec_9_7) {
    JitResult r = runAot(
        "module M; "
        "record Plain(N: int); "
        "class Sink public static method K(x: Object?): int "
        "  if x = null then return -1; end; "
        "  return 1; "
        "end K; end Sink; "
        "class C "
        "  public static method Main(): int "
        "    var p := new Plain(7); "
        "    return Sink.K(p); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// A CLASS at an `Object` parameter. A class reference is not a value type and
// carries no holds-borrows property (§3.6 rule 1) — the borrow rules for class
// references are §28.3's own, unchanged.
BEZEL_TEST(sema_borrow_erasing_position_5659,
           class_reference_at_object_parameter_accepted_spec_9_7_3_6) {
    JitResult r = runAot(
        "module M; "
        "class D public constructor() end constructor; end D; "
        "class Sink public static method K(x: Object?): int "
        "  if x = null then return -1; end; "
        "  return 1; "
        "end K; end Sink; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    return Sink.K(d); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §9.7's FIRST admitted position: "a position declared
// `IReadOnlySpan<TElement>`". A span at an `IReadOnlySpan<int>` parameter is
// the whole point of the interface — §9.7 reads the value there "as the span it
// boxes", so the callee's parameter is bounded exactly as the span would be.
//
// All THREE routes §9.7 names are exercised against this one admitted
// position (implicit, `as`, typed-pattern binding), because they are the same
// conversion spelled three ways and a fix that exempted only the implicit one
// would still reject the other two.
//
// This row asserts COMPILATION and the ABSENCE of the diagnostic, not a
// runtime value for the `as` and pattern spellings. What those two evaluate to
// at run time over a span is a separate, PRE-EXISTING matter this fix does not
// touch (MEASURED on 35759e44's own codegen: `r as IReadOnlySpan<int>` yields
// `null` over a non-null `r`, and the typed pattern does not match it), and
// pinning that here would assert a bug as correct. What this rule owes — the
// conversion is admitted — is what is asserted. The implicit arm's value IS
// sound and IS asserted: `Sink.K(r)` returns a constant 4.
BEZEL_TEST(sema_borrow_erasing_position_5659,
           span_at_ireadonlyspan_position_accepted_spec_9_7_4_16) {
    JitResult r = runAot(
        "module M; "
        "class Sink "
        "  public static method K(x: IReadOnlySpan<int>): int => 4; "
        "  public static method L(x: IReadOnlySpan<int>?): int "
        "    if x = null then return 0; end; "
        "    return 4; "
        "  end L; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(4); "
        "    var r: IReadOnlySpan<int> := buf.AsSpan().AsReadOnly(); "
        "    var code := Sink.K(r); "
        "    code := code + Sink.L(r as IReadOnlySpan<int>); "
        "    case r of "
        "      var v: IReadOnlySpan<int> => code := code + 4, "
        "      _ => code := code + 100 "
        "    end case; "
        "    return code; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBorrowErasing(r));
}

// §9.7's SECOND admitted position: "the receiver of a member call". Every
// §8.5 rendering of a span goes through one — `ToString()`, `GetType()`, an
// interpolation hole. This row is what makes the remedy the rejection NAMES
// real: a diagnostic pointing at a spelling that does not compile is worse
// than one naming none.
BEZEL_TEST(sema_borrow_erasing_position_5659,
           span_as_member_call_receiver_accepted_spec_9_7_8_5) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(4); "
        "    var s := buf.AsSpan(); "
        "    var code := 0; "
        "    if s.ToString() <> \"\" then code := code + 1; end; "
        "    Console.WriteLine(s.ToString()); "
        "    Console.WriteLine($\"{s}\"); "
        "    return code; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §9.7's THIRD admitted position: "an extension call whose head is
// `IReadOnlySpan<TElement>`". A GENERIC extension head, so the head type is a
// parameter the call binds — and it binds to `IReadOnlySpan<int>`, exactly the
// admitted position. This is the row that separates the fix's extension-
// receiver arm from a blanket "reject every extension receiver": the arm reads
// the HEAD TYPE, and this head is the one §9.7 exempts.
//
// A borrow-bearing span is the receiver here, so this row is simultaneously
// the sharpest admit and the sharpest reject in the file — the same value and
// the same call shape as the reject-rows above, differing only in the head.
//
// The receiver must be DECLARED at the interface: extension members are not
// looked up on a concrete builtin span receiver (`Span<T>`, `ReadOnlySpan<T>`).
// That is a PRE-EXISTING limitation, identical on 35759e44 (MEASURED — the
// lookup reports "no member `Twice` on `Span<T>`" there too) and out of scope
// here; declaring the local at `IReadOnlySpan<int>` reaches the extension and
// is what this rule is about.
BEZEL_TEST(sema_borrow_erasing_position_5659,
           span_as_extension_receiver_over_ireadonlyspan_head_accepted_spec_9_7_26_5) {
    JitResult r = runAot(
        "module M; "
        "public extension IReadOnlySpan<T> "
        "  public method Marker(): int => 7; "
        "end extension; "
        "class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(4); "
        "    var r: IReadOnlySpan<int> := buf.AsSpan().AsReadOnly(); "
        "    return r.Marker(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
    BEZEL_EXPECT_FALSE(rejectedBorrowErasing(r));
}

// A non-borrow-bearing receiver over the SAME interface head the reject-rows
// use. Same extension shape, same head, receiver that holds nothing — admitted.
// This is the row that separates the fix's extension arm from "reject every
// record receiver on an extension".
BEZEL_TEST(sema_borrow_erasing_position_5659,
           non_borrow_bearing_record_as_extension_receiver_accepted_spec_9_7_26_5) {
    JitResult r = runAot(
        "module M; "
        "public interface IShow "
        "  method Name(): string; "
        "end IShow; "
        "class Keeper "
        "  private _kept: IShow?; "
        "  public constructor() end constructor; "
        "  public method Put(s: IShow) _kept := s; end Put; "
        "  public method Count(): int => (if _kept <> null then 1 else 0); "
        "end Keeper; "
        "public extension IShow "
        "  public method Leak(k: Keeper) => k.Put(this); "
        "end extension; "
        "public record Plain(N: int): IShow "
        "  public method Name(): string => \"plain\"; "
        "end Plain; "
        "class C "
        "  public static method Main(): int "
        "    using var k := new Keeper(); "
        "    var t := new Plain(7); "
        "    t.Leak(k); "
        "    return k.Count(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_FALSE(rejectedBorrowErasing(r));
}

// §9.7's FIRST printed remedy, at the receiver position: "a parameter or local
// declared at the value's OWN TYPE". An extension headed at the record's own
// type declares `this` at `Tagged`, which announces the borrows, so the
// receiver position erases nothing. Same borrow-bearing value as the reject-row
// above, same extension call shape, differing only in the head — and a head
// that is a RECORD, which §9.7's sentence does not list (it names `Object` and
// "another interface type").
BEZEL_TEST(sema_borrow_erasing_position_5659,
           borrow_bearing_record_as_receiver_of_own_type_extension_accepted_spec_9_7) {
    JitResult r = runAot(
        "module M; "
        "class Item public constructor() end constructor; end Item; "
        "public record Tagged(Src: Item); "
        "public extension Tagged "
        "  public method Peek(): int => 42; "
        "end extension; "
        "class C "
        "  public static method Main(): int "
        "    using var it := new Item(); "
        "    var t := new Tagged(it); "
        "    return t.Peek(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
    BEZEL_EXPECT_FALSE(rejectedBorrowErasing(r));
}

// An `Object`-HEADED extension over a CLASS receiver. §9.7 makes the receiver
// of an `Object`-headed extension borrow-erasing, but only for a value that
// HOLDS BORROWS — and a class reference carries no holds-borrows property
// (§3.6 rule 1). This is the row that pins the two halves of the sentence
// together: the HEAD decides whether the position is borrow-erasing, and the
// SOURCE decides whether there is anything to erase.
BEZEL_TEST(sema_borrow_erasing_position_5659,
           class_as_receiver_of_object_headed_extension_accepted_spec_9_7_26_5) {
    JitResult r = runAot(
        "module M; "
        "public extension Object "
        "  public method Marker(): int => 9; "
        "end extension; "
        "class D public constructor() end constructor; end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    return d.Marker(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
    BEZEL_EXPECT_FALSE(rejectedBorrowErasing(r));
}

// §9.7 prints two remedies for a rejected conversion — "a parameter or local
// declared at the value's own type or at `IReadOnlySpan<TElement>`, a class
// implementing the interface, or — for rendering — `x.ToString()` or an
// interpolation hole". The first and the last are pinned above; this row pins
// the middle one, `a class implementing the interface`, which reaches the
// interface WITHOUT erasing anything.
BEZEL_TEST(sema_borrow_erasing_position_5659,
           class_implementing_interface_reaches_it_accepted_spec_9_7_8_6) {
    JitResult r = runAot(
        "module M; "
        "public interface IShow "
        "  method Name(): string; "
        "end IShow; "
        "public class Impl: IShow "
        "  public constructor() end constructor; "
        "  public method Name(): string => \"impl\"; "
        "end Impl; "
        "class Sink public static method K(x: Object?): int "
        "  if x = null then return -1; end; "
        "  return 1; "
        "end K; end Sink; "
        "class C "
        "  public static method Main(): int "
        "    using var i := new Impl(); "
        "    var s: IShow := i; "
        "    return Sink.K(s); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_FALSE(rejectedBorrowErasing(r));
}