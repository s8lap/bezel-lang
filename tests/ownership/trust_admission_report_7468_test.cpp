// Ownership — issue #7468 (§28.3 *Diagnostics* "The trust-admission report";
// §28.11 *Borrowing collections* → *Storing a borrow*, v0.357): the report
// mode OVER-REPORTED. A line is emitted "at every site where the compiler
// makes an admission that rests on a [K4.3]-recorded trust clause" (§28.3,
// v0.349 and still v0.357), and seven of the fifty lines the mode drew over
// the rule corpus named no obligation at all.
//
// THE DEFECT (MEASURED on the staging tip `r30-c1` @ a44c7bec8, `bezelc
// 0.0.0-a44c7bec8`, `--checked-disposal --report-trust-admissions`):
//
//   * `s10_5_0012` ×3 — `s.Add(E.A);` — an ENUM CONSTANT. A value carries no
//     borrow at all;
//   * `s27_7_1_0004`, `s3_6_0007` — `fs.Add(Demo.Twice);` — a STATIC METHOD
//     REFERENCE. A method reference forms a FUNCTION VALUE (§27.6): a
//     capture-free one carries the empty source set (§28.3 *Provenance*
//     clause 5) and a capture-bearing one is a set-erasing position. Neither
//     is a borrow of the member;
//   * `s28_3_0117` — `b.V := Client.Two;` — the same, into a field;
//   * `s28_3_0110` — `_g := s.Cb;` — a FUNCTION-TYPED PROPERTY READ, which
//     §28.3's provenance clause 5 gives the empty set;
//   * `s7_4_0075` — `d.Add(g.Key, g.Count());` — a VALUE-typed grouping key;
//   * `s28_3_0153` — `_m := Other.Shared;` — a STATIC FIELD READ, whose
//     source is [K2]'s always-live source and so is statically bounded;
//   * `s28_3_0151` — `Gate.M := this._mine;` — a store into a STATIC PROPERTY.
//     [K4.3] clause 3: "No clause covers a **static-held** borrow, and none
//     is needed";
//   * probe `b07_local_collection_quiet.bzl` — `tmp.Add(item);` into a PURELY
//     LOCAL collection. §28.11 (v0.357): "A store into a collection that is
//     a local of the body and does not escape it, of a parameter that is not
//     `owned` or of `this` — the binding itself, not a value read through it
//     — is not reported: the scope comparison bounds the element (K2.3), and
//     the body holds no obligation for the object such a binding names, so no
//     kill in it can transfer that object."
//
// THE ROOT CAUSE. `MethodOwnership::trustFieldStoreCarrier` — the predicate
// every one of the mode's four report sites consults — admitted the
// member-access arm on the AST SHAPE alone ("any `MemberAccessExpr`"), while
// its identifier arm was type-tested throughout. So every member access was
// reported as a borrow carrier. The identifier arm's own comment said why the
// type test is load-bearing: "The spec's rule is about BORROWED references."
//
// WHAT THIS TEST PINS. Every row is RED on the base `r30-c1` (which draws a
// line at each of these stores) and GREEN with the fix — except the TWO
// named control rows at the end, which the owner rule of 2026-11-12 allows:
// each is green on the base and RED under a plausible wrong fix that the
// commit body names (both wrong fixes were shipped by earlier attempts and
// refuted in review). Programs that are green with and without the fix but
// whose verdict a spec sentence decides (the borrow carriers the narrowing
// must keep, the escaping local collections, the alias receivers) are
// conformance-corpus candidates, not gtest rows.
//
// The NARROWING the fix makes, and nothing more:
//   * a member read whose type is POSITIVELY a value — an enum constant, a
//     function value (a method reference, a function-typed property), a
//     primitive or `string`, `Type`, a declaration-less value builtin such
//     as `DateTime` — carries no borrow. Every OTHER declaration-less builtin
//     (`Mutex`, `Stream`, `Encoding`, `OwningSlotMap<T>`, …) and every bare
//     type parameter stays a carrier: the default is CARRIER;
//   * a static field read (the always-live source) and a store into a static
//     holder ([K4.3] clause 3) rest on no clause;
//   * a store of a plain parameter into a collection that is a LOCAL of the
//     body — its binding owns a collection the body created — and does not
//     escape it is not reported (§28.11). A borrow ALIAS of a field's, a
//     parameter's or a callee's collection is not such a collection, and a
//     local that leaves the body in any way the escape scan cannot follow
//     (an alias, a rebinding, a `move`, a return, a store, a capture by a
//     closure that leaves) keeps its line.

// ORACLE. The mode is observed through the OWNERSHIP PASS's diagnostics (the
// same `DiagnosticEngine` the driver hands the pass), not through `runAot`:
// §28.3's report is a compile-time warning, and `runAot` fills
// `JitResult::diagnostics` only on a REJECT — so a `runAot` row could not
// see the warning at all. The pass is driven in-process, in the driver's own
// pass order, with `setReportTrustAdmissions(true)` set before any pass runs
// (the driver's own ordering — `src/driver/Driver.cpp`).

#include <memory>
#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;

namespace {

// The driver's own pass order for a source file (src/driver/Driver.cpp):
// collect symbols → resolve types → typecheck expressions → pure bodies →
// definite assignment → ownership. The report mode's flag is set BEFORE any
// pass runs, so the ownership pass sees it (Driver.cpp: "set before any pass
// runs so every admission site in the ownership pass can consult it").
OwnershipResult checkWithTrustReport(std::string_view source) {
    OwnershipResult r;
    bezel::Lexer lexer(source, r.diagnostics);
    bezel::Parser parser(lexer, r.diagnostics);
    r.sourceFile = parser.parseCompilationUnit();
    r.ctx = std::make_unique<bezel::sema::SemaContext>(r.diagnostics);
    if (r.sourceFile) {
        bezel::sema::collectSymbols(*r.sourceFile, *r.ctx);
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
        r.ctx->setReportTrustAdmissions(true);
        bezel::sema::checkDefiniteAssignment(*r.sourceFile, *r.ctx);
        bezel::ownership::checkOwnership(*r.sourceFile, *r.ctx);
    }
    return r;
}

// The mode's line. §28.3 *Diagnostics*: the warning "names the rule and its
// clause, the borrow and its source, the holder, the obligation …"; the
// single emission point (`MethodOwnership::reportTrustAdmission`) builds one
// sentence for every site, so the needle is stable.
bool hasTrustAdmission(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("trust admission recorded") != std::string::npos)
            return true;
    }
    return false;
}

// The count, so a row can pin "no line" without hiding a second one.
std::size_t trustAdmissionCount(const OwnershipResult& r) {
    std::size_t n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("trust admission recorded") != std::string::npos)
            ++n;
    }
    return n;
}

// "the program compiles" — with the diagnostics PRINTED when it does not, so a
// failure names the rule that fired rather than only the fact that one did
// (the #2988 lesson, `hasErrorMessage`'s own comment: a weak
// `EXPECT_FALSE(hasErrors())` cannot distinguish WHICH rule fired).
#define EXPECT_COMPILES(r)                                                    \
    do {                                                                       \
        if ((r).diagnostics.hasErrors()) {                                     \
            ::bezel::test::currentTestFailed() = true;                         \
            std::cout << "    the program was rejected:\n";                    \
            for (const auto& d : (r).diagnostics.diagnostics())                 \
                if (d.severity == bezel::Severity::Error)                      \
                    std::cout << "      " << d.message << "\n";                \
        }                                                                      \
    } while (false)

// ── The issue's own repro ────────────────────────────────────────────────
// Issue #7468's body, verbatim (`b07_local_collection_quiet.bzl`).
constexpr std::string_view kPurelyLocalCollection =
    "module Conf;\n"
    "public class Item  public constructor() end constructor;  end Item;\n"
    "public class Program\n"
    "  public static method Use(xs: Array<Item>): int => xs.Count;\n"
    "  public static method Register(item: Item): int\n"
    "    using var tmp := new Array<Item>();\n"
    "    tmp.Add(item);        // r35: 'trust admission recorded (§28.11 "
    "borrowing-collection store)' — statically bounded, no clause\n"
    "    return Use(tmp);\n"
    "  end Register;\n"
    "end Program;\n";

} // namespace

// ── The fix rows: no trust admission ─────────────────────────────────────

// fix #7468 (§28.3 *Diagnostics*, v0.357): an ENUM CONSTANT carries no
// borrow, so `s.Add(E.A)` rests on no trust clause. The corpus's
// `s10_5_0012` drew three such lines.
BEZEL_TEST(trust_admission_report_7468, enum_constant_into_collection_is_quiet) {
    auto r = checkWithTrustReport(
        "module Conf;\n"
        "enum E\n"
        "  A, B;\n"
        "end E;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var s := new Set<E>();\n"
        "    s.Add(E.A);\n"
        "    return s.Count;\n"
        "  end Main;\n"
        "end Program;\n");
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 0u);
}

// fix #7468 (§28.3 *Diagnostics*; §27.6 *Method references*, v0.357): a
// STATIC METHOD REFERENCE forms a capture-free function value, which carries
// the empty source set (§28.3 *Provenance* clause 5) — no borrow, no clause.
// The corpus's `s27_7_1_0004` and `s3_6_0007` drew such lines.
BEZEL_TEST(trust_admission_report_7468,
           static_method_reference_into_collection_is_quiet) {
    auto r = checkWithTrustReport(
        "module Conf;\n"
        "public class Demo\n"
        "  public static method Twice(x: int): int => x * 2;\n"
        "  public static method Main(): int\n"
        "    var fs := new Array<(int) -> int>();\n"
        "    fs.Add(Demo.Twice);\n"
        "    return fs.Count;\n"
        "  end Main;\n"
        "end Demo;\n");
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 0u);
}

// fix #7468 (§28.3 *Diagnostics*, v0.357): the same method reference stored
// into a FIELD. The corpus's `s28_3_0117` drew such a line.
BEZEL_TEST(trust_admission_report_7468,
           static_method_reference_into_field_is_quiet) {
    auto r = checkWithTrustReport(
        "module Conf;\n"
        "public class Box<T>\n"
        "  public V: T;\n"
        "  public constructor(v: T)\n"
        "    V := v;\n"
        "  end constructor;\n"
        "end Box;\n"
        "public class Client\n"
        "  public static method One(): int => 1;\n"
        "  public static method Two(): int => 2;\n"
        "  public static method Main(): int\n"
        "    using var b := new Box<() -> int>(Client.One);\n"
        "    b.V := Client.Two;\n"
        "    var g := b.V;\n"
        "    return g();\n"
        "  end Main;\n"
        "end Client;\n");
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 0u);
}

// fix #7468 (§28.3 *Diagnostics*; §28.3 *Provenance* clause 5, v0.357): a
// FUNCTION VALUE read from a property carries the empty set. The corpus's
// `s28_3_0110` drew such a line.
BEZEL_TEST(trust_admission_report_7468,
           function_typed_property_read_into_field_is_quiet) {
    auto r = checkWithTrustReport(
        "module Conf;\n"
        "public class Source\n"
        "  public property Cb: () -> int;\n"
        "  public constructor()\n"
        "    Cb := lambda() => 1;\n"
        "  end constructor;\n"
        "end Source;\n"
        "public class Sink\n"
        "  private _g: () -> int;\n"
        "  public constructor()\n"
        "    _g := lambda() => 0;\n"
        "  end constructor;\n"
        "  public method Take(s: Source): int\n"
        "    _g := s.Cb;\n"
        "    var h := _g;\n"
        "    return h();\n"
        "  end Take;\n"
        "end Sink;\n");
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 0u);
}

// fix #7468 (§28.3 *Diagnostics*, v0.357): a VALUE-typed member read (a
// `bool`) carries no borrow. The corpus's `s7_4_0075` (a grouping key) and
// the issue's own `p22_ctor_extra_argument` probe (`_loud := o.Verbose`)
// drew such lines.
BEZEL_TEST(trust_admission_report_7468,
           value_typed_member_read_into_field_is_quiet) {
    auto r = checkWithTrustReport(
        "module Conf;\n"
        "public class Customer\n"
        "  private _name: string;\n"
        "  public constructor(name: string)\n"
        "    _name := name;\n"
        "  end constructor;\n"
        "  public property Name: string => _name;\n"
        "end Customer;\n"
        "public class Options\n"
        "  private _verbose: bool;\n"
        "  public constructor(verbose: bool)\n"
        "    _verbose := verbose;\n"
        "  end constructor;\n"
        "  public property Verbose: bool => _verbose;\n"
        "end Options;\n"
        "public class Account\n"
        "  private _owner: Customer;\n"
        "  private _loud: bool;\n"
        "  public constructor(owner: Customer, o: Options)\n"
        "    _owner := owner;\n"
        "    _loud := o.Verbose;\n"
        "  end constructor;\n"
        "end Account;\n");
    EXPECT_COMPILES(r);
    // Exactly ONE line is drawn — the `_owner := owner` store, which IS a
    // trust admission (a plain parameter's borrow into an instance field).
    // The `_loud := o.Verbose` store is not.
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}

// fix #7468 (§28.3 *Diagnostics*; [K2]'s always-live source, v0.357): a
// STATIC FIELD READ has the always-live source, so it is statically bounded
// and rests on no clause. The corpus's `s28_3_0153` drew such a line.
BEZEL_TEST(trust_admission_report_7468, static_field_read_into_field_is_quiet) {
    auto r = checkWithTrustReport(
        "module Conf;\n"
        "public class Res\n"
        "  public constructor() end constructor;\n"
        "end Res;\n"
        "public class Other\n"
        "  public static readonly Shared: Res := new Res();\n"
        "end Other;\n"
        "public class Gate\n"
        "  private _m: Res? := null;\n"
        "  public method FromStatic()\n"
        "    _m := Other.Shared;\n"
        "  end FromStatic;\n"
        "end Gate;\n");
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 0u);
}

// fix #7468 (§28.3 *Diagnostics*; [K4.3] clause 3, v0.357): a store into a
// STATIC PROPERTY records no clause — "No clause covers a **static-held**
// borrow, and none is needed". The corpus's `s28_3_0151` (`Gate.M := this._mine;`
// into a `Mutex?` static property) drew such a line.
//
// `Mutex` is spelled here WITHOUT `import System.Threading;` on purpose: the
// harness drives the sema passes in-process and loads no module, so an import
// would be unresolvable — and §22.12 rule 2's form-(i) set makes `Mutex` a
// compiler-known builtin class head anyway.
BEZEL_TEST(trust_admission_report_7468, static_property_store_is_quiet) {
    auto r = checkWithTrustReport(
        "module Conf;\n"
        "public class Gate\n"
        "  public static property M: Mutex? := null;\n"
        "end Gate;\n"
        "public class Owner\n"
        "  private owned _mine: Mutex;\n"
        "  public constructor()\n"
        "    _mine := new Mutex();\n"
        "  end constructor;\n"
        "  public method Publish()\n"
        "    Gate.M := this._mine;\n"
        "  end Publish;\n"
        "end Owner;\n");
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 0u);
}

// fix #7468 (§28.3 *Diagnostics*; [K4.3] clause 3, v0.357): THE STATIC-HOLDER
// HALF OF THE EXCLUSION, on a shape whose operand is unambiguously a
// class-typed borrow. §28.3's *Static stores* admits a class- or
// interface-typed borrow into a static property only when it is
// static-storage-sourced, so a receiver-sourced store rests on no clause at
// all — "No clause covers a **static-held** borrow, and none is needed" — and
// the report must not name it.
//
// The holder is a MANUAL static property (a `get`/`set` pair over no backing
// field of its own), because §22.12 rule 2's verdict table governs "a static
// field or auto-property": an auto-property of a class type is a declaration-
// site error, so the shape would not compile. The manual property is the one
// spelling of a settable static holder that admits a class-typed value.
//
// The operand `this._mine` is a USER class — `isBorrowableOwnedSinkType` sees
// a real class declaration — so the operand is a carrier and the STATIC
// HOLDER is the only thing that silences the line. That is what makes this
// row the holder test's home: the row above (`Mutex`) is silenced by the
// operand test as well, because the compiler models the builtin handle heads
// as declaration-less (see the member-read type test's own comment).
BEZEL_TEST(trust_admission_report_7468,
           static_property_store_of_a_class_borrow_is_quiet) {
    auto r = checkWithTrustReport(
        "module Conf;\n"
        "public class Res\n"
        "  public constructor() end constructor;\n"
        "end Res;\n"
        "public class Gate\n"
        "  public static property M: Res?\n"
        "    get => null;\n"
        "    set => do\n"
        "      _ := value;\n"
        "    end;\n"
        "  end M;\n"
        "end Gate;\n"
        "public class Owner\n"
        "  private owned _mine: Res;\n"
        "  public constructor()\n"
        "    _mine := new Res();\n"
        "  end constructor;\n"
        "  public method Publish()\n"
        "    Gate.M := this._mine;\n"
        "  end Publish;\n"
        "end Owner;\n");
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 0u);
}

// fix #7468 (§28.3 *Diagnostics*; §28.11 *Borrowing collections* → *Storing a
// borrow*, v0.357): a store into a PURELY LOCAL collection of a plain
// parameter is not reported — the scope comparison bounds the element
// (K2.3) and the body holds no obligation for the object the binding names.
// THIS IS THE ISSUE'S OWN REPRO.
BEZEL_TEST(trust_admission_report_7468,
           purely_local_collection_store_is_quiet) {
    auto r = checkWithTrustReport(kPurelyLocalCollection);
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 0u);
}

// fix #7468 (§28.11 *Borrowing collections* → *Storing a borrow*, v0.357):
// the purely local collection is CAPTURED by a closure that is only ever
// CALLED in the body (`f()`), never stored, returned or passed. The closure
// dies with the frame, so the collection does not escape the body and the
// store is not reported — the same verdict as the issue's own repro.
BEZEL_TEST(trust_admission_report_7468,
           local_collection_captured_by_a_closure_called_in_place_is_quiet) {
    auto r = checkWithTrustReport(
        "module Conf;\n"
        "public class Item\n"
        "  public constructor() end constructor;\n"
        "end Item;\n"
        "public class Prog\n"
        "  public method Take(item: Item): int\n"
        "    var tmp: Array<Item> := new Array<Item>();\n"
        "    tmp.Add(item);\n"
        "    var f := lambda() => tmp.Count;\n"
        "    return f();\n"
        "  end Take;\n"
        "end Prog;\n");
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 0u);
}

// A `Holder` with a nullable field of type `ty`, a `Sink` with a nullable
// field of the same type, and a read of the holder's field stored into the
// sink's — the shape the review's `t_*` probes use.
std::string fieldReadIntoField(std::string_view ty) {
    std::string s =
        "module Conf;\n"
        "public class Holder\n"
        "  public _x: ";
    s += std::string(ty);
    s +=
        "?;\n"
        "end Holder;\n"
        "public class Sink\n"
        "  private _kept: ";
    s += std::string(ty);
    s +=
        "?;\n"
        "  public method Take(h: Holder)\n"
        "    _kept := h._x;\n"
        "  end Take;\n"
        "end Sink;\n";
    return s;
}

// fix #7468 (§28.3 *Diagnostics*, v0.357): a `DateTime` is a VALUE type
// (stdlib §2.5 `public record DateTime`), so a member read of one carries no
// borrow and its store into a field rests on no clause.
BEZEL_TEST(trust_admission_report_7468, datetime_field_read_into_field_is_quiet) {
    auto r = checkWithTrustReport(fieldReadIntoField("DateTime"));
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 0u);
}

// fix #7468 (§28.3 *Diagnostics*; §28.9, v0.357): `Type` is runtime-managed —
// "None of them is a class" (§28.9) — and its instance is interned for the
// life of the process, so a member read of one carries no borrow.
BEZEL_TEST(trust_admission_report_7468, type_field_read_into_field_is_quiet) {
    auto r = checkWithTrustReport(fieldReadIntoField("Type"));
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 0u);
}

// fix #7468 (§28.3 *Diagnostics*; §3.4; §28.9, v0.357): `String` is §3.4's
// PascalCase alias of `string`, which is runtime-managed (§28.9), so a member
// read of one carries no borrow.
BEZEL_TEST(trust_admission_report_7468,
           pascalcase_string_field_read_into_field_is_quiet) {
    auto r = checkWithTrustReport(fieldReadIntoField("String"));
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 0u);
}

// ── The two control rows: the trust line IS still drawn ──────────────────
//
// Owner rule of 2026-11-12: at most two base-green controls per suite, each
// RED under a plausible wrong fix that the commit body names. Both wrong fixes
// below were SHIPPED by an earlier attempt at this issue and refuted in
// review; every other base-green program this fix must keep reporting is a
// conformance-corpus candidate.

// CONTROL 1 (§28.11 *Storing a borrow*; K3.13, v0.357). The receiver `tmp` is
// a local of the body, but the COLLECTION is not: `var tmp := _items;` is a
// borrow alias of the field's collection (K3.13: "`var b := a;` is always a
// borrow for class-typed `a`"), so the element goes into a collection that
// outlives the body and the line is drawn exactly as for `_items.Add(item)`.
//
// THE WRONG FIX THIS ROW CATCHES: an exemption keyed on the RECEIVER BINDING
// being a local of this frame (`RecvKind::Local` and a depth in this frame)
// rather than on the collection being one the body created. That wrong fix
// silences this row.
BEZEL_TEST(trust_admission_report_7468,
           store_through_a_local_alias_of_a_field_collection_still_reports) {
    auto r = checkWithTrustReport(
        "module Conf;\n"
        "public class Item\n"
        "  public constructor() end constructor;\n"
        "end Item;\n"
        "public class Prog\n"
        "  private owned _items: Array<Item> := new Array<Item>();\n"
        "  public method Take(item: Item)\n"
        "    var tmp := _items;\n"
        "    tmp.Add(item);\n"
        "  end Take;\n"
        "end Prog;\n");
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}

// CONTROL 2 (§28.3 *Borrow lifetime rules* clause 3; §28.3 *Diagnostics*,
// v0.357). `Stream` is a class (stdlib §7.1 `public abstract class Stream`),
// so a field read of one stored into a non-owned field is the field-held
// [K4.3] clause, and "the trust-admission report … names every such store".
//
// THE WRONG FIX THIS ROW CATCHES: a member-read type test that treats a
// declaration-less builtin as a VALUE unless it is on an allow-list of known
// class heads, so a builtin class the list forgot drops out of the report. That wrong fix silences this row; the fix's default is CARRIER,
// with a closed list of VALUE builtins.
BEZEL_TEST(trust_admission_report_7468, stream_field_read_still_reports) {
    auto r = checkWithTrustReport(fieldReadIntoField("Stream"));
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}
