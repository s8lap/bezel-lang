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
// WHAT THIS TEST PINS. Nine fix rows (no trust line), TWO control rows (the
// trust line IS drawn — the owner rule of 2026-11-12 caps the base-green
// controls at two, each named in the commit body with the wrong fix it
// catches), and SIXTEEN fix-forward rows (the trust line IS drawn): the
// borrow carriers the narrowing of commit 176097b47 DELETED (the
// declaration-less builtin CLASS heads and the bare type parameter), and the
// local collections that ESCAPE the body, which §28.11's exemption does not
// cover.
//
// The control rows are the admitting twins: the same shapes with a legal
// borrow source, which §28.3/§28.11 DO admit on a recorded trust clause.
// They are green on the pre-fix code and stay green — that is what shows the
// narrowing did not silence the mode.
//
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

// ── The fix-forward rows: the narrowing must NOT have deleted a line ──────
//
// fix-forward #7468 (review r42-rev-m32-7468, findings 1 and 2). The
// narrowing that commit 176097b47 shipped DELETED real report lines:
//
//   (1) the member-access arm's type test was `isBorrowableOwnedSinkType`,
//       which reaches a user class or interface and the declaration-less
//       builtin class heads `isBorrowBearingBuiltinClassHead` lists
//       (`Array<T>`, `StringBuilder`, `Random`, the owning collections) — and
//       NOTHING else. Every OTHER declaration-less builtin CLASS (`Mutex`,
//       `Semaphore`, `Atomic<T>`, `Channel<T>`, `CancellationTokenSource`,
//       `Lock`, `Shared<T>`, `Weak<T>`, `Task<T>`, `IEnumerable<T>`,
//       `FrozenArray<T>`) and every BARE TYPE PARAMETER fell out of it, so
//       the report stopped naming genuine field-held carriers: §28.3's
//       *Borrow lifetime rules* clause 3 ("The residual") says "the
//       trust-admission report (§28.3 *Diagnostics*) names every such store",
//       and §28.3's closing sentence — "A compilation unit that emits no
//       report under the mode carries no recorded trust clause" — no longer
//       held for those programs;
//   (2) the §28.11 local-collection exemption read "a local of the body" as
//       "and does not escape it", which is FALSE for a collection that
//       leaves the body. Three ACCEPTED escape routes (a `move` into a
//       storing callee, a `return move tmp`, and a stored lambda capture)
//       each drew exactly one report line on the base and none on the
//       narrowed tip.
//
// The rows below are RED on 176097b47 and GREEN on the base `r30-c1` @
// a44c7bec8 (which already reported them) and on this tip.

// A member read of a CLASS-KINDED declaration-less builtin — and of a bare
// type parameter — is a borrow carrier: storing it into a non-owned field is
// the field-held [K4.3] clause, and §28.3's report names every such store.
// The class-ness is the stdlib's own (stdlib §6.2 `public class Mutex`,
// `public class Semaphore`, `public class Lock`, `public class Atomic<T>`,
// `public class CancellationTokenSource`; stdlib §2.16 `public class
// Shared<T>`, `public class Weak<T>`; stdlib §4.19 `public class
// FrozenArray<T>`; stdlib §5 `public class Task`; stdlib §4 `public interface
// IEnumerable<out T>`; stdlib §6.9 `public class Channel<T>`).
//
// The program is the same shape for every head: a `Holder` with a nullable
// field of that type, a `Sink` with a nullable field of that type, and a
// read of the holder's field stored into the sink's.
std::string classKindedBuiltinFieldRead(std::string_view ty) {
    std::string s =
        "module Conf;\n"
        "public class Item\n"
        "  public constructor() end constructor;\n"
        "end Item;\n"
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

// fix-forward #7468 (§28.3 *Borrow lifetime rules* clause 3; §28.3
// *Diagnostics*, v0.357): a `Mutex` field read stored into a field is the
// field-held [K4.3] clause and is named.
BEZEL_TEST(trust_admission_report_7468,
           class_kinded_builtin_mutex_field_read_still_reports) {
    auto r = checkWithTrustReport(classKindedBuiltinFieldRead("Mutex"));
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}

// fix-forward #7468 (§28.3 *Borrow lifetime rules* clause 3; §28.3
// *Diagnostics*, v0.357): the same for `Semaphore`.
BEZEL_TEST(trust_admission_report_7468,
           class_kinded_builtin_semaphore_field_read_still_reports) {
    auto r = checkWithTrustReport(classKindedBuiltinFieldRead("Semaphore"));
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}

// fix-forward #7468 (§28.3 *Borrow lifetime rules* clause 3; §28.3
// *Diagnostics*, v0.357): the same for `Atomic<int>`.
BEZEL_TEST(trust_admission_report_7468,
           class_kinded_builtin_atomic_field_read_still_reports) {
    auto r = checkWithTrustReport(classKindedBuiltinFieldRead("Atomic<int>"));
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}

// fix-forward #7468 (§28.3 *Borrow lifetime rules* clause 3; §28.3
// *Diagnostics*, v0.357): the same for `Channel<int>`.
BEZEL_TEST(trust_admission_report_7468,
           class_kinded_builtin_channel_field_read_still_reports) {
    auto r = checkWithTrustReport(classKindedBuiltinFieldRead("Channel<int>"));
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}

// fix-forward #7468 (§28.3 *Borrow lifetime rules* clause 3; §28.3
// *Diagnostics*, v0.357): the same for `CancellationTokenSource`.
BEZEL_TEST(trust_admission_report_7468,
           class_kinded_builtin_cts_field_read_still_reports) {
    auto r =
        checkWithTrustReport(classKindedBuiltinFieldRead("CancellationTokenSource"));
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}

// fix-forward #7468 (§28.3 *Borrow lifetime rules* clause 3; §28.3
// *Diagnostics*, v0.357): the same for `Lock`.
BEZEL_TEST(trust_admission_report_7468,
           class_kinded_builtin_lock_field_read_still_reports) {
    auto r = checkWithTrustReport(classKindedBuiltinFieldRead("Lock"));
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}

// fix-forward #7468 (§28.3 *Borrow lifetime rules* clause 3; §28.3
// *Diagnostics*, v0.357): the same for `Shared<Item>` — §28.3's *Borrow
// lifetime rules* own return bullet names `Shared<T>` and `Weak<T>` as
// following "the same owned-vs-borrow rule as any other reference return".
BEZEL_TEST(trust_admission_report_7468,
           class_kinded_builtin_shared_field_read_still_reports) {
    auto r = checkWithTrustReport(classKindedBuiltinFieldRead("Shared<Item>"));
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}

// fix-forward #7468 (§28.3 *Borrow lifetime rules* clause 3; §28.3
// *Diagnostics*, v0.357): the same for `Weak<Item>`.
BEZEL_TEST(trust_admission_report_7468,
           class_kinded_builtin_weak_field_read_still_reports) {
    auto r = checkWithTrustReport(classKindedBuiltinFieldRead("Weak<Item>"));
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}

// fix-forward #7468 (§28.3 *Borrow lifetime rules* clause 3; §28.3
// *Diagnostics*, v0.357): the same for `Task<int>`.
BEZEL_TEST(trust_admission_report_7468,
           class_kinded_builtin_task_field_read_still_reports) {
    auto r = checkWithTrustReport(classKindedBuiltinFieldRead("Task<int>"));
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}

// fix-forward #7468 (§28.3 *Borrow lifetime rules* clause 3; §28.3
// *Diagnostics*, v0.357): the same for `IEnumerable<int>` — an INTERFACE
// head, a class-kinded reference like any other.
BEZEL_TEST(trust_admission_report_7468,
           class_kinded_builtin_ienumerable_field_read_still_reports) {
    auto r = checkWithTrustReport(classKindedBuiltinFieldRead("IEnumerable<int>"));
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}

// fix-forward #7468 (§28.3 *Borrow lifetime rules* clause 3; §28.3
// *Diagnostics*, v0.357): the same for `FrozenArray<int>`.
BEZEL_TEST(trust_admission_report_7468,
           class_kinded_builtin_frozenarray_field_read_still_reports) {
    auto r =
        checkWithTrustReport(classKindedBuiltinFieldRead("FrozenArray<int>"));
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}

// fix-forward #7468 (§28.3 *Borrow lifetime rules* clause 3; §28.3
// *Diagnostics*, v0.357): a BARE TYPE PARAMETER is a borrow carrier too. The
// compiler's own classification of a declaration-less Named type reference is
// "a bare type parameter OR a class/interface builtin" (the same shape
// `ExprTypecheck_Calls.cpp`'s owned-marker erasure test reads), so a `T?`
// field read stored into a `T?` field is the field-held [K4.3] clause and is
// named. §14.1's *Bare-type-parameter returns* is the section that classifies
// the bare-parameter shape.
BEZEL_TEST(trust_admission_report_7468,
           bare_type_parameter_field_read_still_reports) {
    auto r = checkWithTrustReport(
        "module Conf;\n"
        "public class Item\n"
        "  public constructor() end constructor;\n"
        "end Item;\n"
        "public class Box<T>\n"
        "  public _v: T?;\n"
        "end Box<T>;\n"
        "public class Sink<T>\n"
        "  private _k: T?;\n"
        "  public method Take(b: Box<T>)\n"
        "    _k := b._v;\n"
        "  end Take;\n"
        "end Sink<T>;\n");
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}

// fix-forward #7468 (§28.3 *Borrow lifetime rules* clause 3; §28.11, v0.357):
// the RECEIVER-SOURCED shape of the same carrier — `this._m` read into a
// field AND into a collection — draws TWO lines (the field-held clause and
// the collection-held clause). The corpus calls this shape "a receiver-sourced
// borrow" (s28_3_0151).
BEZEL_TEST(trust_admission_report_7468,
           receiver_sourced_mutex_field_read_still_reports) {
    auto r = checkWithTrustReport(
        "module Conf;\n"
        "public class Sink\n"
        "  private owned _m: Mutex;\n"
        "  private _kept: Mutex?;\n"
        "  private owned _ms: Array<Mutex>;\n"
        "  public constructor()\n"
        "    _m := new Mutex();\n"
        "    _ms := new Array<Mutex>();\n"
        "  end constructor;\n"
        "  public method Take()\n"
        "    _kept := this._m;\n"
        "    _ms.Add(this._m);\n"
        "  end Take;\n"
        "end Sink;\n");
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 2u);
}

// fix-forward #7468 (§28.11 *Borrowing collections* → *Storing a borrow*,
// v0.357): §28.11's exemption is conditioned on the collection being "a
// local of the body AND DOES NOT ESCAPE IT". A collection that is moved into
// a callee that stores it leaves the body, so the borrow it holds of the
// plain parameter outlives the body, and the collection-held [K4.3] clause
// holds the obligation — the mode names it.
//
// The program is ACCEPTED (zero errors): the callee takes the array as an
// `owned` parameter and stores it in an owned field. (The callee's store is
// written `_held := a;` — the `owned` field slot announces the transfer, so
// a `move` there would be the redundant marker §17.8 rejects.)
BEZEL_TEST(trust_admission_report_7468,
           local_collection_moved_into_a_storing_callee_still_reports) {
    auto r = checkWithTrustReport(
        "module Conf;\n"
        "public class Item\n"
        "  public constructor() end constructor;\n"
        "end Item;\n"
        "public class Keeper\n"
        "  private owned _held: Array<Item>?;\n"
        "  public method Keep(owned a: Array<Item>)\n"
        "    _held := a;\n"
        "  end Keep;\n"
        "end Keeper;\n"
        "public class Prog\n"
        "  public method Take(k: Keeper, item: Item)\n"
        "    var tmp: Array<Item> := new Array<Item>();\n"
        "    tmp.Add(item);\n"
        "    k.Keep(move tmp);\n"
        "  end Take;\n"
        "end Prog;\n");
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}

// fix-forward #7468 (§28.11, v0.357): the same for a collection RETURNED
// out of the body — the returned `owned Array<Item>` outlives the body and
// holds the borrow of the plain parameter. (The `owned` return type
// announces the transfer, so the operand is written without a `move` —
// §17.8's marker economy.)
BEZEL_TEST(trust_admission_report_7468,
           local_collection_returned_out_of_the_body_still_reports) {
    auto r = checkWithTrustReport(
        "module Conf;\n"
        "public class Item\n"
        "  public constructor() end constructor;\n"
        "end Item;\n"
        "public class Prog\n"
        "  public method Make(item: Item): owned Array<Item>\n"
        "    var tmp: Array<Item> := new Array<Item>();\n"
        "    tmp.Add(item);\n"
        "    return tmp;\n"
        "  end Make;\n"
        "end Prog;\n");
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}

// fix-forward #7468 (§28.11, v0.357): the same for a collection CAPTURED by
// a closure that is stored in a field — the closure holds the collection, and
// the field outlives the body.
BEZEL_TEST(trust_admission_report_7468,
           local_collection_captured_by_a_stored_lambda_still_reports) {
    auto r = checkWithTrustReport(
        "module Conf;\n"
        "public class Item\n"
        "  public constructor() end constructor;\n"
        "end Item;\n"
        "public class Prog\n"
        "  private _cb: (() -> int)?;\n"
        "  public method Take(item: Item)\n"
        "    var tmp: Array<Item> := new Array<Item>();\n"
        "    tmp.Add(item);\n"
        "    var f := lambda() => tmp.Count;\n"
        "    _cb := f;\n"
        "  end Take;\n"
        "end Prog;\n");
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}

// ── The control rows: the trust line IS still drawn ──────────────────────

// fix-forward #7468 (owner rule of 2026-11-12): this row and its three
// siblings — a plain parameter's borrow into a field-held collection, a plain
// parameter's borrow into an instance field, a field-path read into an
// instance field, and a receiver-sourced element into a field-held
// collection — are GREEN with and without the fix, so they moved to the
// conformance corpus instead of staying gtest rows (the rule: "A program that
// is green with and without the fix, but whose verdict a spec sentence fixes,
// goes to the conformance corpus instead"). Their programs and their deciding
// spec sentences are in
// `/var/tmp/bezel-scratch/r42-m32-7468/corpus-candidates/` (the harvest lane
// writes the corpus cases from them).

// The admitting twin of the purely-local row: an element read through a FIELD
// PATH into a purely local collection STAYS reported — "the body's own
// replacing `move` can take the field's value out and transfer it" (§28.3.9
// *Scope* clause 1, quoted by §28.11).
//
// THE WRONG FIX THIS ROW CATCHES (owner rule of 2026-11-12 — a control names
// the plausible wrong fix it catches): a narrowing that applies §28.11's
// exemption to EVERY store into a purely local collection, dropping the
// "the binding itself, not a value read through it" half. That wrong fix
// silences this row, and §28.11's last sentence says it must not.
BEZEL_TEST(trust_admission_report_7468,
           field_path_into_purely_local_collection_still_reports) {
    auto r = checkWithTrustReport(
        "module Conf;\n"
        "public class Item\n"
        "  public constructor() end constructor;\n"
        "end Item;\n"
        "public class Holder\n"
        "  public owned _child: Item;\n"
        "  public constructor()\n"
        "    _child := new Item();\n"
        "  end constructor;\n"
        "end Holder;\n"
        "public class Program\n"
        "  public static method Register(h: Holder)\n"
        "    using var tmp := new Array<Item>();\n"
        "    tmp.Add(h._child);\n"
        "  end Register;\n"
        "end Program;\n");
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}

// The receiver-sourced twin: a field-path read stored into an instance field
// is a field-held trust clause too. (moved to the corpus — see the note
// above).

// The receiver-sourced twin of the collection row: a collection held by a
// FIELD outlives the body, so a plain parameter's borrow stored into it rests
// on the collection-held trust clause (§28.11 clause 1). (moved to the corpus
// — see the note above).

// The static-holder test's own twin: the SAME receiver-sourced class borrow,
// stored into an INSTANCE field instead of a static property, IS the
// field-held trust clause (§28.3 *Borrow lifetime rules*), and the mode names
// it. It is what shows the static-holder exclusion is about the HOLDER and
// not about the operand.
//
// THE WRONG FIX THIS ROW CATCHES (owner rule of 2026-11-12 — a control names
// the plausible wrong fix it catches): a narrowing that drops the member-
// access arm of the report altogether (or one that type-tests it so tightly
// that a user class read falls out of it). That wrong fix silences this row,
// and §28.3's *Borrow lifetime rules* clause 3 says the report "names every
// such store".
BEZEL_TEST(trust_admission_report_7468,
           receiver_sourced_class_borrow_into_field_still_reports) {
    auto r = checkWithTrustReport(
        "module Conf;\n"
        "public class Res\n"
        "  public constructor() end constructor;\n"
        "end Res;\n"
        "public class Owner\n"
        "  private owned _mine: Res;\n"
        "  private _kept: Res? := null;\n"
        "  public constructor()\n"
        "    _mine := new Res();\n"
        "  end constructor;\n"
        "  public method Publish()\n"
        "    _kept := this._mine;\n"
        "  end Publish;\n"
        "end Owner;\n");
    EXPECT_COMPILES(r);
    BEZEL_EXPECT_EQ(trustAdmissionCount(r), 1u);
}
