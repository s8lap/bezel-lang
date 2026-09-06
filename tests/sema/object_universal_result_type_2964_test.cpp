// Sema — issue #2964. The §8.1 universal `GetHashCode` / `Equals` carry §8.1's
// declared RESULT TYPE on every receiver (language spec §8.1 "Definition",
// §8.3 "`Equals` and `GetHashCode`", v0.240).
//
// §8.1 declares `Object` verbatim as
//
//     public class Object
//       public method GetType(): Type;
//       public virtual method Equals(other: Object?): bool;
//       public virtual method GetHashCode(): int;
//       public virtual method ToString(): string;
//       public static method ReferenceEquals(a: Object?, b: Object?): bool;
//     end Object;
//
// and states, in the same section: "`Object` is the universal root. Every type
// derives from it". The result type of these members is therefore a property of
// the MEMBER, not of the receiver.
//
// Two of the five were typed that way already — `ToString` and `GetType`, each
// in its own block of `ExprTC::typeCall` — and `GetHashCode`/`Equals` were not.
// They were typed only where some receiver-specific handler happened to type
// the MemberAccessExpr (records / enums / choices, `string`, a genuinely-
// declared type parameter); the CallExpr then inherited that. MEASURED on
// 618c0c8 with a six-receiver grid, `var h := x.GetHashCode(); Console.
// WriteLine("h=" + h.ToString());`:
//
//     class (plain user class)   NOT typed   -> compile error on the `+`
//     class through an interface NOT typed   -> compile error on the `+`
//     int (primitive)            NOT typed   -> compile error on the `+`
//     record                     typed
//     enum                       typed
//     string                     typed
//
// and `var b := x.Equals(x);` likewise untyped on a class receiver.
//
// TWO OBSERVABLES, and only one of them is loud. Written as `"h=" + h
// .ToString()` the missing type surfaces as §4.6's "`+` requires both operands
// to be `string`" — an error about an unrelated line. Written as #2964's own
// repro, `Console.WriteLine(h.ToString())`, it is SILENT: the untyped call
// infects the local, the next member access on that local is untyped too, and
// codegen drops the whole enclosing statement. #2964 measured exactly that —
// ZERO BYTES printed, zero diagnostics at any severity, between two
// `Console.WriteLine`s that print normally, exit code unchanged. Both shapes
// are pinned below, and the silent one asserts the program's OUTPUT: an exit-
// code-only assertion passes while nothing happens.
//
// The value itself was never missing — §8.3's identity hash is lowered and two
// calls on one instance agree — so this is a typing gap, not an unimplemented
// member. That is why the close is to TYPE the call rather than to reject it
// (#2922's own first draft rejected the untyped-receiver case and was reverted
// for exactly this reason).
//
// WHAT THESE TESTS ASSERT ABOUT HASH VALUES, and what they deliberately do not.
// No IDENTITY hash is pinned to a number: §8.3 promises only that one is stable
// within a run and address-independent ("Programs must continue to treat
// identity hashes as opaque per-run values"), and the auto-generated record/
// choice combiner is explicitly implementation-defined. Those cases assert
// §8.3's own two statements instead — `if a.Equals(b) then a.GetHashCode() =
// b.GetHashCode()`, and within-run stability — plus the thing #2964 is actually
// about: that the result has a TYPE, observed as `h.ToString()` producing a
// non-empty decimal line rather than the statement disappearing.
//
// Two numbers ARE pinned, and only on §8.3's per-kind default table, which is
// normative: the "Primitive" row gives `Value-based` (so `7.GetHashCode()` is
// 7) and the "Enum" row gives `Hash of integer value` (so `E.Blue`, the second
// constant, is 1). If a future revision replaces either default the table moves
// with it and these two expectations move with the table.
//
// Mechanical verification: reverting the `#2964` block in
// src/sema/ExprTypecheck_Calls.cpp (`typeCall`'s second `if (!calleeSym)`)
// leaves every `..._untyped_*` case below failing — the loud ones on the §4.6
// diagnostic, the silent one on the vanished line — and leaves every control
// passing.

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::vector<std::string> lines(const std::string& out) {
    std::vector<std::string> ls;
    std::string cur;
    for (char c : out) {
        if (c == '\n') { ls.push_back(cur); cur.clear(); }
        else if (c != '\r') { cur.push_back(c); }
    }
    ls.push_back(cur);
    return ls;
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// Does the program compile AND print exactly `expected`? The output is the
// oracle: #2964's signature is a statement that vanishes with no diagnostic,
// which "it compiled" and "exit 0" both report as success.
bool prints(const std::string& program, const std::string& expected) {
    return runAotOut(program) == expected;
}

bool hasErrorContaining(const std::string& program, const char* needle) {
    JitResult r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// #2964's own repro, in shape: the SILENT drop.
// ---------------------------------------------------------------------------

// The `Console.WriteLine(h.ToString())` between two printing WriteLines emitted
// ZERO bytes — not a blank line, not "0". Assert that three lines come out and
// that the middle one is a decimal integer (its VALUE is opaque per §8.3).
BEZEL_TEST(sema_object_universal_result_2964, gethashcode_on_user_class_prints_spec_8_1) {
    std::string out = runAotOut(
        "module M; "
        "class P public method Nop(): int return 0; end Nop; end P; "
        "class C public static method Main(): int "
        "  using var p := new P(); "
        "  var h := p.GetHashCode(); "
        "  Console.WriteLine(\"before\"); "
        "  Console.WriteLine(h.ToString()); "
        "  Console.WriteLine(\"after\"); "
        "  return 0; "
        "end Main; end C;");
    auto ls = lines(out);
    BEZEL_EXPECT_EQ(static_cast<int>(ls.size()), 3);
    if (ls.size() == 3) {
        BEZEL_EXPECT_EQ(ls[0], std::string("before"));
        BEZEL_EXPECT_TRUE(allDigits(ls[1]));
        BEZEL_EXPECT_EQ(ls[2], std::string("after"));
    }
}

// #2964's second confirmation: the result feeds arithmetic, and the derived
// local inherited the absent type ("`m.ToString()` is not `string`, because
// `m` inherited `h`'s absent type"). `mod 7` keeps the printed value bounded
// without pinning the opaque hash itself; the point is that it prints at all.
BEZEL_TEST(sema_object_universal_result_2964, gethashcode_result_feeds_arithmetic_spec_8_3) {
    std::string out = runAotOut(
        "module M; "
        "class P public method Nop(): int return 0; end Nop; end P; "
        "class C public static method Main(): int "
        "  using var x := new P(); "
        "  var h := x.GetHashCode(); "
        "  var m := h mod 7; "
        "  Console.WriteLine(\"m=\" + m.ToString()); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(out.rfind("m=", 0) == 0 && allDigits(out.substr(2)));
}

// ---------------------------------------------------------------------------
// The receiver grid: the three kinds that carried no type. Each asserts §8.3's
// within-run stability ("assigned on first request ... and stores it with the
// object thereafter") AND that the result's `.ToString()` reaches stdout.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_object_universal_result_2964, gethashcode_untyped_on_class_receiver_spec_8_1) {
    BEZEL_EXPECT_TRUE(prints(
        "module M; "
        "class P public method Nop(): int return 0; end Nop; end P; "
        "class C public static method Main(): int "
        "  using var p := new P(); "
        "  var h1 := p.GetHashCode(); "
        "  var h2 := p.GetHashCode(); "
        "  if h1 = h2 then Console.WriteLine(\"stable\"); "
        "             else Console.WriteLine(\"unstable\"); end; "
        "  if h1.ToString().Length > 0 then Console.WriteLine(\"typed\"); end; "
        "  return 0; "
        "end Main; end C;", "stable\ntyped"));
}

// A class reached through an interface it implements — the `isIfaceRecv`
// sibling of the class case in the same dispatch family.
BEZEL_TEST(sema_object_universal_result_2964, gethashcode_untyped_on_interface_impl_spec_8_1) {
    BEZEL_EXPECT_TRUE(prints(
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "class Sq: IShape public method Area(): int return 4; end Area; end Sq; "
        "class C public static method Main(): int "
        "  using var q := new Sq(); "
        "  var h := q.GetHashCode(); "
        "  if h.ToString().Length > 0 then Console.WriteLine(\"typed\"); end; "
        "  return 0; "
        "end Main; end C;", "typed"));
}

// A PRIMITIVE receiver — §8.3's "Primitive | Value comparison | Value-based"
// row. Not named in #2964; found by the grid. §8.3's hash-equality invariant is
// the assertable part: two `int`s that are `Equals` hash the same.
BEZEL_TEST(sema_object_universal_result_2964, gethashcode_untyped_on_primitive_receiver_spec_8_3) {
    BEZEL_EXPECT_TRUE(prints(
        "module M; class C public static method Main(): int "
        "  var i := 7; "
        "  var j := 7; "
        "  var hi := i.GetHashCode(); "
        "  var hj := j.GetHashCode(); "
        "  if hi = hj then Console.WriteLine(\"agree\"); "
        "             else Console.WriteLine(\"differ\"); end; "
        "  Console.WriteLine(\"hi=\" + hi.ToString()); "
        "  return 0; "
        "end Main; end C;", "agree\nhi=7"));
}

// §8.1 declares `Equals(other: Object?): bool`. On a class receiver the call
// was untyped for the same reason; `if b then …` happened to work (codegen
// lowers a real value), which is what made the gap look narrower than it is —
// any use that needs the TYPE, such as `b.ToString()`, did not.
BEZEL_TEST(sema_object_universal_result_2964, equals_untyped_on_class_receiver_spec_8_1) {
    BEZEL_EXPECT_TRUE(prints(
        "module M; "
        "class P public method Nop(): int return 0; end Nop; end P; "
        "class C public static method Main(): int "
        "  using var p := new P(); "
        "  var b := p.Equals(p); "
        "  if b then Console.WriteLine(\"eq\"); end; "
        "  Console.WriteLine(\"b=\" + b.ToString()); "
        "  return 0; "
        "end Main; end C;", "eq\nb=True"));
}

// ---------------------------------------------------------------------------
// Controls — receivers that were ALREADY typed must keep their answers. These
// pass before and after the fix; they exist so a future change that replaces
// the receiver-specific handlers with the last-resort rule cannot silently
// change what those receivers report. Each asserts §8.3's normative invariant
// "if `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`".
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_object_universal_result_2964, control_record_receiver_still_typed_spec_8_3) {
    BEZEL_EXPECT_TRUE(prints(
        "module M; record R(A: int, B: int); "
        "class C public static method Main(): int "
        "  var r1 := new R(1, 2); "
        "  var r2 := new R(1, 2); "
        "  if r1.Equals(r2) then Console.WriteLine(\"equal\"); end; "
        "  if r1.GetHashCode() = r2.GetHashCode() then "
        "      Console.WriteLine(\"hashagree\"); end; "
        "  return 0; "
        "end Main; end C;", "equal\nhashagree"));
}

BEZEL_TEST(sema_object_universal_result_2964, control_enum_receiver_still_typed_spec_8_3) {
    BEZEL_EXPECT_TRUE(prints(
        "module M; enum E Red, Blue; end E; "
        "class C public static method Main(): int "
        "  var a := E.Blue; "
        "  var b := E.Blue; "
        "  if a.GetHashCode() = b.GetHashCode() then "
        "      Console.WriteLine(\"hashagree\"); end; "
        "  Console.WriteLine(\"h=\" + a.GetHashCode().ToString()); "
        "  return 0; "
        "end Main; end C;", "hashagree\nh=1"));
}

BEZEL_TEST(sema_object_universal_result_2964, control_string_receiver_still_typed_spec_8_3) {
    BEZEL_EXPECT_TRUE(prints(
        "module M; class C public static method Main(): int "
        "  var s := \"abc\"; "
        "  var h := s.GetHashCode(); "
        "  if h = \"abc\".GetHashCode() then Console.WriteLine(\"hashagree\"); end; "
        "  return 0; "
        "end Main; end C;", "hashagree"));
}

// ---------------------------------------------------------------------------
// The one reject that must SURVIVE. Typing the CALL must not un-reject
// anything, so it is asserted on its diagnostic TEXT (per tests/README.md
// a bare `!r.ran` would also be satisfied by a crash).
// ---------------------------------------------------------------------------

// fix-forward (spec §8.3, v0.294): this row used to pin that `Set<int>`'s
// `GetHashCode` was REJECTED as "not yet implemented for this receiver" —
// `narrow #3896` (audit Set/OwningSet/SortedSet/FrozenArray/Dictionary/
// OwningDictionary/LinkedList/OwningLinkedList for §8.3/§8.5) closed exactly
// that gap for Set, joining it to `isAuditedElsewhere` and to the codegen
// identity default (LlvmBackend_Calls2.cpp's
// `isBuiltinIdentityResidualCollection`), so the reject this row pinned is
// STALE — MEASURED on this tree: the program below now compiles and runs
// clean (`bezelc` exit 0), where the pre-#3896 compiler rejected it. §2465's
// separate collection-receiver reject (gated on the MemberAccessExpr's type,
// `!ctx_.exprType(ma)`) is untouched by this row moving — it never covered
// Set to begin with once #3896 lands, and stays pinned by this file's other
// rows and by collection_universal_member_narrowing_2465_test.cpp.
//
// The replacement pins §8.3's Class-kind identity default with VALUES, not
// "it compiles": `GetHashCode` is stable across two calls on the SAME
// instance and DIFFERENT for a distinct, separately-constructed `Set<int>`
// with the same elements (ruling out a degenerate constant-zero default,
// #2804's trap) — and `Equals` is reference equality (true against itself,
// false against that distinct instance). The identity hash's actual VALUE is
// deliberately left unpinned (`allDigits`, not a literal) — per this file's
// own header comment, §8.3 promises only within-run stability and address-
// independence, not a specific number — and the "h=" + ToString() prefix
// keeps #2964's own point alive: the result still has a TYPE that `+`
// accepts.
BEZEL_TEST(sema_object_universal_result_2964,
           gethashcode_on_set_receiver_implements_spec_8_3_identity_default) {
    std::string out = runAotOut(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  s.Add(1); "
        "  using var t := new Set<int>(); "
        "  t.Add(1); "
        "  var h1 := s.GetHashCode(); "
        "  Console.WriteLine(\"h=\" + h1.ToString()); "
        "  var h2 := s.GetHashCode(); "
        "  if h1 = h2 then Console.WriteLine(\"stable\"); "
        "             else Console.WriteLine(\"unstable\"); end; "
        "  var h3 := t.GetHashCode(); "
        "  if h1 <> h3 then Console.WriteLine(\"distinct\"); "
        "              else Console.WriteLine(\"collided\"); end; "
        "  if s.Equals(s) then Console.WriteLine(\"eqself\"); end; "
        "  if s.Equals(t) then Console.WriteLine(\"eqother\"); "
        "                 else Console.WriteLine(\"noteqother\"); end; "
        "  return 0; "
        "end Main; end C;");
    auto ls = lines(out);
    BEZEL_EXPECT_EQ(static_cast<int>(ls.size()), 5);
    if (ls.size() == 5) {
        BEZEL_EXPECT_TRUE(ls[0].rfind("h=", 0) == 0 && allDigits(ls[0].substr(2)));
        BEZEL_EXPECT_EQ(ls[1], std::string("stable"));
        BEZEL_EXPECT_EQ(ls[2], std::string("distinct"));
        BEZEL_EXPECT_EQ(ls[3], std::string("eqself"));
        BEZEL_EXPECT_EQ(ls[4], std::string("noteqother"));
    }
}

// §8.2 / #487: `GetType` is the one §8.1 instance member that is deferred, and
// it reports so in sema while still typing itself `Type`. #2964 must not have
// swallowed that.
BEZEL_TEST(sema_object_universal_result_2964, gettype_still_reports_its_deferral_spec_8_2) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        "module M; "
        "class P public method Nop(): int return 0; end Nop; end P; "
        "class C public static method Main(): int "
        "  using var p := new P(); "
        "  var t := p.GetType(); "
        "  return 0; "
        "end Main; end C;",
        "reflection is not yet implemented: 'GetType()'"));
}
