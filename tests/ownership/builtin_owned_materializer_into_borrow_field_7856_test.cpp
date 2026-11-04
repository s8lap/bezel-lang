// Regression tests for #7856 -- a BUILTIN owned-materializer result stored
// into a NON-`owned` field was accepted, and the field does not own the value
// so it LEAKED.
//
// THE DEFECT (measured). `public data: Array<byte>;` with a constructor
// `data := g.ToByteArray();` compiled with NO diagnostic, and in the default
// disposal mode the program leaked:
//
//   SUMMARY: AddressSanitizer: 14400 byte(s) leaked in 300 allocation(s).
//
// -- 150 iterations x 2 allocations (the `bz_array_t` header and its backing
// buffer); 20000 iterations read 40000 allocations, so the leak is LINEAR in
// the iteration count. The `Encoding.UTF8.GetBytes` twin is identical (300
// allocations). The `public owned data: Array<byte>;` twin is CLEAN. Under
// `--checked-disposal` the leak is INVISIBLE (the checked mode's backstop
// does not see it), so a checked-mode CI would never see it.
//
// THE RULE (language spec v0.353 -- the numbering did NOT move; verified by
// title as well as number).
//
//   §28.3.4 "Producers" -- the producers table's CALL row (spec line 7127)
//   reads:
//     "Any **call** — instance, static, or extension method, or a
//      function-value invocation | Classified by the announcement table
//      below: the callee's declared, pre-substitution return kind × callee
//      kind decides. `owned T` → **Owned** (row 1)."
//   (that row then continues with the mint-announcing, plain-class,
//   bare-type-parameter, runtime-managed and move-only cases), and the
//   announcement table's row 1 (spec line 7173) reads:
//     "`owned T` over a non-mint-announcing type | **Owned**"
//   §28.6 "Leak prevention" (spec line 7904) -- whose "Who may call
//   `Dispose()`" is a SUB-HEADING inside that section, not the section's
//   title -- together with §28.3's borrow-lifetime rules: an owned value may
//   not be dropped into a borrow slot -- the slot does not own it, so nobody
//   disposes it.
//
// The compiler already implements that rule for a USER-declared
// `owned`-returning call: the lit family
// `tests/lit/ownership/fresh_owned_into_borrow_field_1020__owned_call_into_borrow_field_rejected.bzl`
// pins exactly that rejection, and the diagnostic is the §28.6 fresh-store
// one:
//
//   "a freshly-produced owned value cannot be stored into the non-`owned`
//    field `data` -- the field does not own it, so the value would leak;
//    declare the field `owned` (§28.5), or hand it to a method that takes it
//    `owned` (§28.6)"
//
// THE GAP. A BUILTIN materializer call reaches NO symbol -- the builtin
// member-call typing path in sema lives entirely inside `if (!calleeSym)`
// (stdlib §5 / §4.4 / §9.6), so a builtin callee carries no declaration and
// no `owned`-returning signature for the ownership pass to read. `producerState`
// therefore classifies the call BORROWED, and the §28.6 fresh-store arm --
// which tests `producerState(*as.value) == OwnershipState::Owned` -- never
// fired for it. So the ONE shape the rule could not see was the one the
// standard library declares `owned`-returning:
//   stdlib §2.6 `Guid`      `ToByteArray(): owned Array<byte>`
//   stdlib §3.2 `Encoding`  `GetBytes(s: string): owned Array<byte>`
//
// THE SECOND GAP -- THE FIELD-INITIALIZER POSITION. §28.6's fresh-store
// bullet states the rule with NO positional restriction, and the rule has
// TWO store arms: the ASSIGNMENT-STATEMENT one (`handleAssignment`'s
// `targetIsNonOwnedField` branch) and the FIELD-INITIALIZER one
// (`checkFieldInitializer`). They were separate tests, and only the first
// was widened -- so the field-initializer spelling
// (`public data: Array<byte> := Guid.NewGuid().ToByteArray();`), which is
// the ISSUE'S OWN TITLE SHAPE, was still ACCEPTED and still LEAKED (14400
// bytes in 300 allocations, measured on the pre-fix tree). Both arms now ask
// the SAME named predicate.
//
// THE FIX. The non-`owned`-field store arm -- in BOTH positions -- now also
// fires for a BUILTIN owned-materializer call, recognised by
// `common::isBuiltinOwnedMaterializerCall` (the SAME authority the ownership
// pass already uses to classify these calls as owned productions --
// `isMaterializerCarveOutBorrow`, #4042 / #4057 / #5284).
//
// IT IS DIAGNOSTIC-ONLY BY CONSTRUCTION. `producerState` still classifies
// these calls BORROWED, and MUST keep doing so:
// `isBuiltinOwnedMaterializerCall`'s own header requires it ("codegen already
// reclaims these values on its own recognition ... Reclassifying them Owned
// would register a SECOND free on top of codegen's -- the double-free
// direction"). The arm only REPORTS; no disposal decision changes. The
// program is rejected at compile time, so the leak is no longer reachable --
// at BOTH store positions.
//
// THE ORACLE. The reject rows assert the diagnostic's MESSAGE by SUBSTRING
// -- the §28.6 fresh-store sentence that names the field, the "the field does
// not own it, so the value would leak" clause, the two remedies and their
// §-citations -- and the diagnostic's LOCATION: the exact LINE it lands on,
// and a NON-ZERO column (the file does not pin an exact column). The
// field-initializer reject rows ALSO observe the COMPILE-TIME verdict
// through the real driver: the program is REJECTED (not crashed, not timed
// out) and the rejection carries a real source location. The control
// rows assert the shapes that must NOT change: the `owned` field twin (the
// legal ownership transfer), the user-declared `owned`-returning call (the
// rule's existing coverage), the `new` twin (already rejected before this
// change), and the owned RETURN of the same builtin materializer (the legal
// transfer out).
//
// The last row is a MEASUREMENT: the fix's own remedy -- the `owned` field
// spelling -- is compiled and run under the generated-code ASan/LSan leg and
// its LEAK COUNT is read out of LSan's own "SUMMARY: AddressSanitizer:
// <bytes> byte(s) leaked in <allocations> allocation(s)." line via the
// harness's parser (`bezel::test::aot_detail::parseLeakSummary`). The
// expected count is ZERO. (The pre-fix program's own leak -- 14400 bytes in
// 300 allocations -- was measured directly on the pre-fix tree; that program
// no longer compiles after this change, which is precisely the point, so its
// leak count is not expressible as a row here.)
//
// Spec: §28.6 "Leak prevention" (the fresh-store bullet -- "A
// **freshly-produced owned value** -- a `new` expression, a call returning
// `owned T`, a `move` expression, or an owned operator result -- stored into
// a non-`owned` (borrow) field is an **error**"), §28.3.4 "Producers" (the
// producers table's call row and the announcement table's row 1: `owned T` ->
// Owned), §28.5 "Owned fields" (the remedy); stdlib §2.6 (`Guid`), §3.2
// (`Encoding`), §5 (`System.Collections.Linq`), §4.4 (`Array<T>`), §9.6
// (`String`), §10.1 (`CString`) -- v0.353.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// THE §28.6 FRESH-STORE MESSAGE, verbatim.
const char* kFreshOwnedIntoBorrowField =
    "a freshly-produced owned value cannot be stored into the non-`owned` "
    "field";

// The one ERROR-severity diagnostic whose message carries `needle`.
const bezel::Diagnostic* theErrorWith(
    const OwnershipResult& r, const std::string& needle) {
    const bezel::Diagnostic* found = nullptr;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) == std::string::npos) continue;
        if (found) return nullptr;  // more than one -- not a single verdict
        found = &d;
    }
    return found;
}

// The program under test: a class with a NON-`owned` field of the builtin
// collection type, and a constructor that stores a BUILTIN materializer
// result into it.
//
// `fieldDecl` is the field's declaration ("public data: Array<byte>;" or
// "public owned data: Array<byte>;") and `init` the constructor's initializer
// statement ("data := g.ToByteArray();" / "data := Encoding.UTF8.GetBytes(s);").
std::string prog(const std::string& fieldDecl, const std::string& init,
                 const std::string& ctorParams) {
    return
        "module M;\n"
        "class H\n"
        "  " + fieldDecl + "\n"
        "  public constructor(" + ctorParams + ") " + init + " end constructor;\n"
        "end H;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var g := Guid.NewGuid();\n"
        "    using var h := new H(g);\n"
        "    Console.WriteLine(h.data.Count);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// The FIELD-INITIALIZER variant -- the SAME slot, but the materializer
// result is written by the field's OWN INITIALIZER instead of by a
// constructor body.
//
// §28.6's fresh-store bullet states the rule with NO positional restriction
// ("a freshly-produced owned value ... stored into a non-`owned` (borrow)
// field is an error"), so the field-initializer spelling is the same rule at
// a different position -- and it is the spelling the ISSUE'S OWN TITLE
// names ("an owned materializer result stored into a NON-`owned` field
// `public data: Array<byte>`").
//
// `fieldDecl` is the field's declaration ("public data: Array<byte>" or
// "public owned data: Array<byte>") and `initExpr` the initializer
// expression.
std::string fieldInitProg(const std::string& fieldDecl,
                           const std::string& initExpr) {
    return
        "module M;\n"
        "class H\n"
        "  " + fieldDecl + " := " + initExpr + ";\n"
        "end H;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var h := new H();\n"
        "    Console.WriteLine(h.data.Count);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// The COMPILE-TIME verdict, observed through the REAL driver: the program is
// REJECTED at compile time (not crashed, not timed out), and the rejection
// carries a real source location. This is the exit-code observable -- a
// program the compiler rejects never reaches `Main`, so the AOT harness
// reports `ran == false` together with the driver's own
// `<path>:<line>:<col>: <message>` stderr.
void expectCompileRejected(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

}  // namespace

// =========================================================================
// The REJECT rows -- the issue's own repro and its GetBytes twin.
// =========================================================================

// THE ISSUE'S OWN REPRO: `Guid.ToByteArray()` (stdlib §2.6, declared
// `owned Array<byte>`) stored into a non-`owned` field. Before this change
// the program compiled clean and the value leaked (14400 bytes in 300
// allocations, measured on the pre-fix tree).
BEZEL_TEST(builtin_owned_materializer_into_borrow_field_7856,
           guid_to_byte_array_into_non_owned_field_rejected_spec_28_6) {
    OwnershipResult r = parseAndCheckOwnership(
        prog("public data: Array<byte>;", "data := g.ToByteArray();", "g: Guid"));
    const bezel::Diagnostic* d = theErrorWith(r, kFreshOwnedIntoBorrowField);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (!d) return;
    // The message names the field and the two remedies.
    BEZEL_EXPECT_TRUE(d->message.find("`data`") != std::string::npos);
    BEZEL_EXPECT_TRUE(d->message.find("the field does not own it, so the "
                                      "value would leak") != std::string::npos);
    BEZEL_EXPECT_TRUE(d->message.find("declare the field `owned`") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(d->message.find("§28.5") != std::string::npos);
    BEZEL_EXPECT_TRUE(d->message.find("§28.6") != std::string::npos);
    // A LOCATED error: the diagnostic lands on the constructor's initializer
    // line, at a real column.
    BEZEL_EXPECT_EQ(d->location.line, 4u);
    BEZEL_EXPECT_TRUE(d->location.column > 0u);
}

// The `Encoding.UTF8.GetBytes` twin (stdlib §3.2, declared
// `owned Array<byte>`) -- the same shape, the same rule, the same leak.
BEZEL_TEST(builtin_owned_materializer_into_borrow_field_7856,
           encoding_get_bytes_into_non_owned_field_rejected_spec_28_6) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "public data: Array<byte>;",
        "data := Encoding.UTF8.GetBytes(\"abcdefghijklmnop\");", "g: Guid"));
    const bezel::Diagnostic* d = theErrorWith(r, kFreshOwnedIntoBorrowField);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (!d) return;
    BEZEL_EXPECT_TRUE(d->message.find("`data`") != std::string::npos);
    BEZEL_EXPECT_TRUE(d->message.find("§28.6") != std::string::npos);
    BEZEL_EXPECT_EQ(d->location.line, 4u);
    BEZEL_EXPECT_TRUE(d->location.column > 0u);
}

// The same shape stored into ANOTHER object's field (not the constructor's
// own `this`): the rule is about the SLOT, not about the receiver.
BEZEL_TEST(builtin_owned_materializer_into_borrow_field_7856,
           builtin_materializer_into_another_objects_field_rejected_spec_28_6) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M;\n"
        "class H\n"
        "  public data: Array<byte>?;\n"
        "end H;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var g := Guid.NewGuid();\n"
        "    var h := new H();\n"
        "    h.data := g.ToByteArray();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theErrorWith(r, kFreshOwnedIntoBorrowField);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_EQ(d->location.line, 9u);
}

// =========================================================================
// The CONTROL rows -- the shapes that must NOT change.
// =========================================================================

// The `owned` field twin: the legal ownership transfer (§28.5). It was
// CLEAN before this change and must stay clean -- this is the row that
// would go RED on a false rejection.
BEZEL_TEST(builtin_owned_materializer_into_borrow_field_7856,
           owned_field_twin_accepted_spec_28_5) {
    OwnershipResult r = parseAndCheckOwnership(
        prog("public owned data: Array<byte>;", "data := g.ToByteArray();",
             "g: Guid"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A USER-declared `owned`-returning call into a non-`owned` field -- the
// rule's EXISTING coverage (the #1020 lit family's
// `owned_call_into_borrow_field_rejected`). It was rejected before this
// change and must stay rejected, with the same message.
BEZEL_TEST(builtin_owned_materializer_into_borrow_field_7856,
           user_owned_call_into_non_owned_field_still_rejected_spec_28_6) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Inner end Inner; "
        "class Wrap "
        "  private _b: Inner?; "
        "  public static method MakeOwned(): owned Inner return new Inner(); "
        "    end MakeOwned; "
        "  public method Set() this._b := MakeOwned(); end Set; "
        "end Wrap; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Wrap(); "
        "    w.Set(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    const bezel::Diagnostic* d = theErrorWith(r, kFreshOwnedIntoBorrowField);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->message.find("`_b`") != std::string::npos);
}

// The `new` twin -- a `new Array<byte>()` IS visible to `producerState`, so
// this shape was ALREADY rejected before this change, with the same message.
// The row pins that the rule's wording is the same for both producers.
BEZEL_TEST(builtin_owned_materializer_into_borrow_field_7856,
           new_array_into_non_owned_field_still_rejected_spec_28_6) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class H "
        "  public data: Array<byte>; "
        "  public constructor() data := new Array<byte>(); end constructor; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    using var h := new H(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    const bezel::Diagnostic* d = theErrorWith(r, kFreshOwnedIntoBorrowField);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->message.find("`data`") != std::string::npos);
}

// The legal transfer OUT: the same builtin materializer result RETURNED
// under an `owned` return type (§28.8 / K3.2) must still compile -- the fix
// widens a STORE rule only.
BEZEL_TEST(builtin_owned_materializer_into_borrow_field_7856,
           owned_return_of_builtin_materializer_still_accepted_spec_28_8) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class H "
        "  public static method Make(g: Guid): owned Array<byte> "
        "    return g.ToByteArray(); "
        "  end Make; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var g := Guid.NewGuid(); "
        "    var a := H.Make(g); "
        "    Console.WriteLine(a.Count); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =========================================================================
// The MEASUREMENT -- the fix's own remedy (the `owned` field spelling) is
// compiled and run, and its LEAK COUNT is measured under the generated-code
// ASan/LSan leg. The expected count is ZERO.
// =========================================================================
BEZEL_TEST(builtin_owned_materializer_into_borrow_field_7856,
           owned_field_remedy_measures_zero_leaked_allocations_spec_28_5) {
    JitResult r = runAotChecked(
        "module M; "
        "class H "
        "  public owned data: Array<byte>; "
        "  public constructor(g: Guid) data := g.ToByteArray(); end constructor; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var tot := 0; "
        "    var i := 0; "
        "    while i < 150 do "
        "      var g := Guid.NewGuid(); "
        "      using var h := new H(g); "
        "      tot := tot + h.data.Count; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(tot > 0); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
    std::string text;
    for (const auto& d : r.diagnostics.diagnostics()) {
        text += d.message;
        text += '\n';
    }
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    // No sanitizer report of any kind (a double free would land here).
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

// =========================================================================
// The FIELD-INITIALIZER store arm -- the SAME §28.6 fresh-store rule at the
// position the ISSUE'S OWN TITLE names.
//
// The assignment-statement store arm (`handleAssignment`'s
// `targetIsNonOwnedField` branch) and the FIELD-INITIALIZER store
// (`checkFieldInitializer`) are TWO store arms of ONE rule. §28.6's
// fresh-store bullet states the rule without any positional restriction, so
// the field-initializer spelling is the same rule at a different position.
//
// Before this change only the assignment-statement arm saw a BUILTIN
// owned-materializer call; the field-initializer arm still tested
// `producerState(*fd.initializer) == OwnershipState::Owned` alone, and
// `producerState` classifies a builtin materializer BORROWED (a builtin
// callee resolves NO symbol, so the call carries no `owned`-returning
// declaration to read). So the field-initializer spelling was ACCEPTED and
// LEAKED -- MEASURED at the pre-fix tree: 14400 bytes in 300 allocations for
// the `Guid.NewGuid().ToByteArray()` program (150 constructions x 2
// allocations), the same for the `Encoding.UTF8.GetBytes` twin.
//
// Both arms now ask the SAME named predicate, so the two positions cannot
// drift apart again.
// =========================================================================

// THE ISSUE'S OWN TITLE SHAPE, as a FIELD INITIALIZER --
// `public data: Array<byte> := Guid.NewGuid().ToByteArray();`. Before this
// change the program compiled clean and leaked 14400 bytes in 300
// allocations (measured on the pre-fix tree).
BEZEL_TEST(builtin_owned_materializer_into_borrow_field_7856,
           field_initializer_guid_to_byte_array_into_non_owned_field_rejected_spec_28_6) {
    const std::string src =
        fieldInitProg("public data: Array<byte>",
                       "Guid.NewGuid().ToByteArray()");
    OwnershipResult r = parseAndCheckOwnership(src);
    const bezel::Diagnostic* d = theErrorWith(r, kFreshOwnedIntoBorrowField);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(d->message.find("`data`") != std::string::npos);
        BEZEL_EXPECT_TRUE(d->message.find("the field does not own it, so the "
                                          "value would leak") != std::string::npos);
        BEZEL_EXPECT_TRUE(d->message.find("declare the field `owned`") !=
                          std::string::npos);
        BEZEL_EXPECT_TRUE(d->message.find("§28.5") != std::string::npos);
        BEZEL_EXPECT_TRUE(d->message.find("§28.6") != std::string::npos);
        // The diagnostic lands on the FIELD DECLARATION's line (line 3), at a
        // real column.
        BEZEL_EXPECT_EQ(d->location.line, 3u);
        BEZEL_EXPECT_TRUE(d->location.column > 0u);
    }
    // The COMPILE-TIME verdict through the real driver: the program is
    // REJECTED, so it never reaches `Main`.
    JitResult jr = runAotChecked(src);
    expectCompileRejected(jr);
}

// The `Encoding.UTF8.GetBytes` twin (stdlib §3.2, declared
// `owned Array<byte>`) as a FIELD INITIALIZER -- the same rule, the same
// message, a different materializer.
BEZEL_TEST(builtin_owned_materializer_into_borrow_field_7856,
           field_initializer_encoding_get_bytes_into_non_owned_field_rejected_spec_28_6) {
    const std::string src =
        fieldInitProg("public data: Array<byte>",
                       "Encoding.UTF8.GetBytes(\"abcdefghijklmnop\")");
    OwnershipResult r = parseAndCheckOwnership(src);
    const bezel::Diagnostic* d = theErrorWith(r, kFreshOwnedIntoBorrowField);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(d->message.find("`data`") != std::string::npos);
        BEZEL_EXPECT_TRUE(d->message.find("§28.6") != std::string::npos);
        BEZEL_EXPECT_EQ(d->location.line, 3u);
        BEZEL_EXPECT_TRUE(d->location.column > 0u);
    }
    JitResult jr = runAotChecked(src);
    expectCompileRejected(jr);
}

// The LINQ `ToArray` twin (stdlib §5, declared `owned Array<T>`) as a FIELD
// INITIALIZER -- the rule must not be special-cased to one call.
BEZEL_TEST(builtin_owned_materializer_into_borrow_field_7856,
           field_initializer_to_array_into_non_owned_field_rejected_spec_28_6) {
    const std::string src =
        fieldInitProg("public data: Array<int>", "(1..3).ToArray()");
    OwnershipResult r = parseAndCheckOwnership(src);
    const bezel::Diagnostic* d = theErrorWith(r, kFreshOwnedIntoBorrowField);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(d->message.find("`data`") != std::string::npos);
        BEZEL_EXPECT_TRUE(d->message.find("§28.6") != std::string::npos);
        BEZEL_EXPECT_EQ(d->location.line, 3u);
        BEZEL_EXPECT_TRUE(d->location.column > 0u);
    }
    JitResult jr = runAotChecked(src);
    expectCompileRejected(jr);
}

// The `string.Split` twin (stdlib §9.6, declared `owned Array<string>`) as a
// FIELD INITIALIZER -- the rule must not be special-cased to one call.
BEZEL_TEST(builtin_owned_materializer_into_borrow_field_7856,
           field_initializer_split_into_non_owned_field_rejected_spec_28_6) {
    const std::string src =
        fieldInitProg("public data: Array<string>", "\"a,b,c\".Split(\",\")");
    OwnershipResult r = parseAndCheckOwnership(src);
    const bezel::Diagnostic* d = theErrorWith(r, kFreshOwnedIntoBorrowField);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(d->message.find("`data`") != std::string::npos);
        BEZEL_EXPECT_TRUE(d->message.find("§28.6") != std::string::npos);
        BEZEL_EXPECT_EQ(d->location.line, 3u);
        BEZEL_EXPECT_TRUE(d->location.column > 0u);
    }
    JitResult jr = runAotChecked(src);
    expectCompileRejected(jr);
}

// The `new` twin as a FIELD INITIALIZER -- a `new Array<byte>()` IS visible to
// `producerState`, so this shape was ALREADY rejected before this change,
// with the same message. The row pins that the rule's wording is the same
// for both producers at the field-initializer position too.
BEZEL_TEST(builtin_owned_materializer_into_borrow_field_7856,
           field_initializer_new_array_into_non_owned_field_still_rejected_spec_28_6) {
    OwnershipResult r = parseAndCheckOwnership(
        fieldInitProg("public data: Array<byte>", "new Array<byte>()"));
    const bezel::Diagnostic* d = theErrorWith(r, kFreshOwnedIntoBorrowField);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(d->message.find("`data`") != std::string::npos);
        BEZEL_EXPECT_EQ(d->location.line, 3u);
        BEZEL_EXPECT_TRUE(d->location.column > 0u);
    }
}

// The USER-declared `owned`-returning call as a FIELD INITIALIZER -- the
// rule's EXISTING coverage at this position. It was rejected before this
// change and must stay rejected, with the same message.
BEZEL_TEST(builtin_owned_materializer_into_borrow_field_7856,
           field_initializer_user_owned_call_into_non_owned_field_still_rejected_spec_28_6) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M;\n"
        "class Inner end Inner;\n"
        "class Maker\n"
        "  public static method MakeOwned(): owned Inner\n"
        "    return new Inner();\n"
        "  end MakeOwned;\n"
        "end Maker;\n"
        "class H\n"
        "  public data: Inner := Maker.MakeOwned();\n"
        "end H;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var h := new H();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theErrorWith(r, kFreshOwnedIntoBorrowField);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(d->message.find("`data`") != std::string::npos);
        BEZEL_EXPECT_EQ(d->location.line, 9u);
        BEZEL_EXPECT_TRUE(d->location.column > 0u);
    }
}

// The `owned` field twin as a FIELD INITIALIZER -- the legal §28.5 storage
// path. It was CLEAN before this change and must stay clean: this is the
// row that would go RED on a false rejection.
BEZEL_TEST(builtin_owned_materializer_into_borrow_field_7856,
           field_initializer_owned_field_twin_accepted_spec_28_5) {
    OwnershipResult r = parseAndCheckOwnership(
        fieldInitProg("public owned data: Array<byte>",
                       "Guid.NewGuid().ToByteArray()"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The `owned` field twin as a FIELD INITIALIZER, MEASURED: 150 constructions
// of the class, and the LEAK COUNT read out of LSan's own "SUMMARY:
// AddressSanitizer: <bytes> byte(s) leaked in <allocations> allocation(s)."
// line via the harness's parser. The expected count is ZERO.
BEZEL_TEST(builtin_owned_materializer_into_borrow_field_7856,
           field_initializer_owned_field_remedy_measures_zero_leaked_allocations_spec_28_5) {
    JitResult r = runAotChecked(
        "module M;\n"
        "class H\n"
        "  public owned data: Array<byte> := Guid.NewGuid().ToByteArray();\n"
        "end H;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var tot := 0;\n"
        "    var i := 0;\n"
        "    while i < 150 do\n"
        "      using var h := new H();\n"
        "      tot := tot + h.data.Count;\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(tot > 0);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
    std::string text;
    for (const auto& d : r.diagnostics.diagnostics()) {
        text += d.message;
        text += '\n';
    }
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}
