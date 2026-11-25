// Regression test for issue #9137 (FOLLOW-UP) — a read of an indexer whose
// parameter is a FUNCTION TYPE THAT RETURNS UNIT, through an INTERFACE-typed
// variable, SEGV'd: the interface dispatch of the indexer getter called a
// NULL itable slot (address 0x0), because the implementing class's indexer
// was never matched against the interface's.
//
// THE DEFECT. `Emitter::findIndexer` (src/codegen/LlvmBackend_Assign.cpp) is
// the codegen mirror of sema's interface-conformance check: it selects the
// class indexer that implements an interface indexer by comparing their
// parameter TYPE LISTS (§23.4: "Indexers may be overloaded by parameter type
// list"), and `declareVtables` Pass D installs the selected accessor's
// function pointer into the class's itable slot for that interface.
//
// The comparison is `indexerParamTypeMatchesUnder`, the codegen twin of
// sema's `Resolver::typeRefEqualImpl` (src/sema/TypeResolution_Checks.cpp).
// A function type is compared SLOT BY SLOT (§27.1: slot types, modifiers and
// return type), so the comparison DESCENDS into the function type's RETURN
// type and into its PARAMETER slots' types. The comparison had arms for the
// FUNCTION kind and (through `indexerArgMatchesUnder`) for PRIMITIVE and
// NAMED — but NOT for UNIT and NOT for INLINE ARRAY, the two kinds sema's
// `typeRefEqualImpl` ACCEPTS. Those two fell through to
// `indexerArgMatchesUnder`, which answers `false` for every kind it does not
// spell out. So:
//
//   * `() -> ()` — a PROCEDURE, the commonest function type there is — has a
//     UNIT return, so the comparison failed;
//   * `() -> [3]int` has an INLINE-ARRAY return, and `([3]int) -> ()` has an
//     INLINE-ARRAY parameter slot, so the comparison failed too.
//
// `findIndexer` therefore returned null, the itable slot kept its
// `ConstantPointerNull` initialiser, and the interface-typed read CALLED
// ADDRESS 0x0.
//
// MEASURED on r30-c1 @ e70e47ff9 (`bezelc --sanitize=address`, the base this
// test is RED on):
//
//   AddressSanitizer:DEADLYSIGNAL
//   ==1773433==ERROR: AddressSanitizer: SEGV on unknown address 0x000000000000
//   (pc 0x000000000000 bp 0x7ffe136e6790 sp 0x7ffe136e66d8 T0)
//   ==1773433==HINT: pc points to the zero page.
//
// for the getter, the setter and the compound assignment alike, and for the
// `() -> ()`, `(int) -> ()`, `(ref int) -> ()`, `(owned Box) -> ()`,
// `() -> (int) -> ()`, `() -> [3]int` and `([3]int) -> ()` spellings. The
// CLASS-typed read of the very same indexer answered, and the interface-typed
// read of an `int`-RETURNING function-typed indexer answered — the defect is
// specific to a function type whose return (or parameter slot) is UNIT or an
// INLINE ARRAY, read through an INTERFACE-typed receiver.
//
// THE TWINS THAT ARE ALREADY CORRECT (rows 6 and 7) — the class-typed read of
// the very same unit-returning indexer, and the interface-typed read of an
// `int`-returning function-typed indexer.
//
// THE SPEC. §11.2 (v0.359), interface declaration: "A member **matches** an
// interface member when it is declared `public`, has the same name, the same
// arity (parameter count), the same number of type parameters of its own, the
// same parameter modifiers (`ref`, `out`, `owned`, `scoped`), parameter types
// that are exactly equal under the interface's substitution, and a return type
// that are exactly equal under the interface's substitution". The indexer's
// parameter type here is `() -> ()` on BOTH sides — exactly equal — so the
// class's indexer MATCHES the interface's and the interface-typed access
// MUST dispatch to the implementation (§23.4, v0.359: "Indexers may be
// overloaded by parameter type list. They participate in `virtual`/`override`/
// `sealed`/`abstract` like methods."). §27.1 (v0.359) makes a function type a
// first-class type whose identity is its slot types, its modifiers and its
// return type; §4.7 (v0.359) makes `()` the one unit type ("`()` is the type
// with exactly one value"), and §15.1 (v0.359) makes the size N part of an
// inline array's type identity ("The size N is part of the type. `[3]int` and
// `[4]int` are distinct types that do not interconvert."). §23.5 (v0.359)
// forbids only `ref`/`out` in an indexer parameter list, leaving a plain
// (call-bounded, §28.14) callback parameter admitted.
//
// THE CHANGE. One site, `indexerParamTypeMatchesUnder` in
// src/codegen/LlvmBackend_Assign.cpp: the comparison gained the UNIT and
// INLINE-ARRAY arms, mirroring sema's `typeRefEqualImpl` — the UNIT arm
// answers `true` for two unit refs, and the INLINE-ARRAY arm compares the
// ownedness, the FOLDED size and the element type. The CALL-SITE overload
// selection (`selectIndexer`, same file) is NOT touched: it answers
// PARAMETER-vs-ARGUMENT and keeps `indexerArgMatchesUnder`, mirroring sema's
// own `allParamsMatch`.
//
// THE ORACLE. Every row runs the program TWICE: once plain (`runAot`) for the
// VALUE oracle — the program's own exit code and output — and once checked
// (`runAotChecked`) for the MEMORY oracle. The callback is CAPTURE-FREE on
// purpose: a capturing callback leaks its capture cell for a reason that has
// nothing to do with this issue (the leak is present on the base too, on the
// class-typed path as well, and even with no indexer involved at all), and
// pinning it here would make this suite a second tracker for that separate
// defect.
//
// ROWS.
//   Row 1 — the issue's own shape: the interface-typed READ of a
//           `() -> ()`-parameterized indexer, 200 iterations. RED on the base
//           (the SEGV), GREEN with the fix (200, no leak).
//   Row 2 — the SETTER shape: `fl[lambda() => do end] := 3;` then a read,
//           200 iterations. RED on the base (the SEGV), GREEN with the fix
//           (800, no leak).
//   Row 3 — the COMPOUND shape: `fl[lambda() => do end] += 1;` then a read,
//           200 iterations. RED on the base (the SEGV), GREEN with the fix
//           (600, no leak).
//   Row 4 — the UNIT return with a PARAMETER slot: `(int) -> ()`. RED on the
//           base (the SEGV), GREEN with the fix (200, no leak).
//   Row 5 — the INLINE-ARRAY return: `() -> [3]int`. RED on the base (the
//           SEGV), GREEN with the fix (200, no leak).
//   Row 6 — GREEN CONTROL: the CLASS-typed read of the very same
//           unit-returning indexer (the twin the issue says already works).
//           Green on both arms; it is the guard that the fix does not change
//           the class-typed dispatch.
//   Row 7 — GREEN CONTROL: the interface-typed read of an `int`-RETURNING
//           function-typed indexer. Green on both arms; it is the guard that
//           the fix does not change the `int`-returning interface dispatch.
//
// per §4.7, §7.1, §11.2, §15.1, §22.3, §23.4, §23.5, §27.1, §28.9,
// §28.14 (issue #9137)

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// The VALUE oracle: the program compiled, ran to completion, exited with its
// own exit code and printed exactly this output. The plain (uninstrumented)
// run has no sanitizer, so its exit code is the program's own.
void expectValue(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The MEMORY oracle: the checked run leaked NOTHING at all, and no memory
// error fired.
void expectNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// The unit-returning function-typed indexer family: an interface declaring an
// indexer over a callback parameter, a class implementing it (its getter
// calls the callback and returns `_last + 1`, its setter calls the callback
// and stores `value`), and a `Main` that loops 200 times.
//
// `cls` names the module/class stem, `cbType` the index parameter's type
// spelling (`() -> ()`, `(int) -> ()`, `() -> [3]int`), `ifaceDecl` the
// interface's indexer declaration (`readonly property [...]` for the
// getter-only shape, `property [...]` for the read-write one), `getBody` /
// `setBody` the class's accessor bodies, `stmt` the statement(s) executed per
// iteration and `read` the expression whose value is accumulated.
//
// The arithmetic (why the expected outputs are 200 / 800 / 600): the getter
// returns `_last + 1` and the setter stores `value`.
//   * read only                       -> 0 + 1 = 1             -> 200 * 1  = 200
//   * `:= 3` then read                -> _last = 3, read = 3 + 1 = 4
//                                                            -> 200 * 4  = 800
//   * `+= 1` then read                -> 1 + 1 = 2, _last = 2,
//                                       read = 2 + 1 = 3        -> 200 * 3  = 600
std::string unitRetProgram(const std::string& cls,
                            const std::string& cbType,
                            const std::string& ifaceDecl,
                            const std::string& getBody,
                            const std::string& setBody,
                            const std::string& stmt,
                            const std::string& read) {
    return "module " + cls + "; "
           "public interface IFl" + cls + " "
           "  " + ifaceDecl + " "
           "end IFl" + cls + "; "
           "public class FlI" + cls + " : IFl" + cls + " "
           "  private _last: int := 0; "
           "  public property [q: " + cbType + "]: int "
           "    get => " + getBody + "; "
           "    set => " + setBody + "; "
           "  end; "
           "  public constructor() end constructor; "
           "end FlI" + cls + "; "
           "public class Program" + cls + " "
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var j in 0..<200 do "
           "      using var c := new FlI" + cls + "(); "
           "      var fl: IFl" + cls + " := c; "
           "      " + stmt + " "
           "      t := t + " + read + "; "
           "    end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program" + cls + ";";
}

// The UNIT-returning getter and setter bodies: the callback is CALLED (so
// the dispatch really hands the callback to the accessor) and its unit result
// is discarded, which is what a `() -> ()` callback returns.
const char* kUnitGet = "do q(); return _last + 1; end";
const char* kUnitSet = "do q(); _last := value; end";

// ── Row 1 (§11.2 / §23.4 / §27.1): the issue's own shape — the
// interface-typed READ of a `() -> ()`-parameterized indexer, 200 iterations.
//
// RED on the base (the SEGV at address 0x0), GREEN with the fix (200, and
// the checked run leaks nothing: the call-bounded callback's producer is
// discharged at the call's end).
BEZEL_TEST(interface_indexer_unit_return_fn_param_dispatch_9137,
           interface_typed_read_of_a_unit_returning_function_typed_indexer_dispatches) {
    const std::string src = unitRetProgram(
        "U9137R", "() -> ()",
        "readonly property [q: () -> ()]: int;",
        kUnitGet, kUnitSet, "", "fl[lambda() => do end]");
    auto v = runAot(src);
    expectValue(v, 0, "200\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

// ── Row 2 (§11.2 / §23.4 / §23.5 / §28.14): the SETTER shape —
// `fl[lambda() => do end] := 3;` through the interface-typed variable, then
// a read. 200 iterations.
//
// RED on the base (the SEGV), GREEN with the fix (800, no leak).
BEZEL_TEST(interface_indexer_unit_return_fn_param_dispatch_9137,
           interface_typed_write_of_a_unit_returning_function_typed_indexer_dispatches) {
    const std::string src = unitRetProgram(
        "U9137S", "() -> ()",
        "property [q: () -> ()]: int;",
        kUnitGet, kUnitSet, "fl[lambda() => do end] := 3;",
        "fl[lambda() => do end]");
    auto v = runAot(src);
    expectValue(v, 0, "800\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

// ── Row 3 (§6.6 / §23.4 / §28.14): the COMPOUND shape —
// `fl[lambda() => do end] += 1;` through the interface-typed variable, then
// a read. 200 iterations.
//
// RED on the base (the SEGV), GREEN with the fix (600, no leak).
BEZEL_TEST(interface_indexer_unit_return_fn_param_dispatch_9137,
           interface_typed_compound_write_of_a_unit_returning_function_typed_indexer_dispatches) {
    const std::string src = unitRetProgram(
        "U9137C", "() -> ()",
        "property [q: () -> ()]: int;",
        kUnitGet, kUnitSet, "fl[lambda() => do end] += 1;",
        "fl[lambda() => do end]");
    auto v = runAot(src);
    expectValue(v, 0, "600\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

// ── Row 4 (§11.2 / §22.3 / §27.1): the UNIT return with a PARAMETER slot —
// `(int) -> ()`. The callback takes an int and returns unit.
//
// RED on the base (the SEGV), GREEN with the fix (200, no leak).
BEZEL_TEST(interface_indexer_unit_return_fn_param_dispatch_9137,
           interface_typed_read_of_a_unit_returning_callback_with_a_parameter_dispatches) {
    const std::string src = unitRetProgram(
        "U9137P", "(int) -> ()",
        "readonly property [q: (int) -> ()]: int;",
        "do q(1); return _last + 1; end", "do q(1); _last := value; end",
        "", "fl[lambda(x: int) => do end]");
    auto v = runAot(src);
    expectValue(v, 0, "200\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

// ── Row 5 (§4.7 / §11.2 / §15.1 / §27.1): the INLINE-ARRAY
// return — `() -> [3]int`. The size N is part of the inline array's type
// identity, so the comparison has to look at it.
//
// RED on the base (the SEGV), GREEN with the fix (200, no leak).
BEZEL_TEST(interface_indexer_unit_return_fn_param_dispatch_9137,
           interface_typed_read_of_an_inline_array_returning_function_typed_indexer_dispatches) {
    const std::string src = unitRetProgram(
        "U9137A", "() -> [3]int",
        "readonly property [q: () -> [3]int]: int;",
        "do var a := q(); return _last + 1; end", "do var a := q(); _last := value; end",
        "", "fl[lambda() => do var a: [3]int := [1, 2, 3]; return a; end]");
    auto v = runAot(src);
    expectValue(v, 0, "200\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

// ── Row 6 (GREEN CONTROL — the CLASS-typed twin): the very same
// unit-returning indexer, read through the CLASS-typed variable `c`.
//
// Green on the base and green with the fix: the issue says the class-typed
// read already works, and this row is the guard that the fix does not change
// it (a fix that routed the class-typed access through the itable would
// turn this row red).
BEZEL_TEST(interface_indexer_unit_return_fn_param_dispatch_9137,
           class_typed_read_of_the_same_unit_returning_indexer_is_unchanged) {
    const std::string src = unitRetProgram(
        "U9137K", "() -> ()",
        "readonly property [q: () -> ()]: int;",
        kUnitGet, kUnitSet, "", "c[lambda() => do end]");
    auto v = runAot(src);
    expectValue(v, 0, "200\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

// ── Row 7 (GREEN CONTROL — the `int`-RETURNING twin): an interface indexer
// over a `() -> int` parameter, read through the interface-typed variable.
//
// Green on the base and green with the fix: this is the shape the earlier
// `narrow #9137` commit fixed, and this row is the guard that the fix does
// not change it.
BEZEL_TEST(interface_indexer_unit_return_fn_param_dispatch_9137,
           interface_typed_read_of_an_int_returning_function_typed_indexer_is_unchanged) {
    const std::string src = unitRetProgram(
        "U9137I", "() -> int",
        "readonly property [q: () -> int]: int;",
        "_last + q()", "_last := q() + value", "", "fl[lambda() => 7]");
    auto v = runAot(src);
    expectValue(v, 0, "1400\n");
    auto r = runAotChecked(src);
    expectNoLeak(r);
}

}  // namespace
