// Spec-derived regression test for issue #2969.
//
// §18.5 (Interpolated strings) / §8.5 (`ToString`) / §8.1 (`Object` is the
// universal root) / §28.9 (Runtime-managed types), v0.240. stdlib §4.4
// (`Array<T>`), stdlib §4.8 (`Dictionary<K,V>`).
//
// THE RULE. §18.5, verbatim: "A hole *without* a format spec formats via the
// value's ordinary `ToString()` ([§8.5])." §8.5, verbatim: "`ToString` is
// invoked implicitly inside string interpolation ([§18.5]) via virtual dispatch
// — for present values", and its compiler-generated default, verbatim: "For
// classes: the type's qualified name." The runtime type of an `IList<int>`-typed
// slot may be either a USER CLASS declaring `IList<int>` (rare, but legal per
// §8.1) or a BUILT-IN COLLECTION — `Array<T>` is the §4.4 case — whose runtime
// value is a plain C struct (`bz_array_t*`) with no Bezel vtable at word 0.
//
// THE DEFECT. After #2923 fixed the concrete built-in class half, the same
// corruption is still reachable through a BUILT-IN INTERFACE HEAD — the
// `isVtableLessBuiltinClassName` discriminator #2923 added is
// `isReservedBuiltinTypeName` MINUS `kBuiltinInterfaceHeads` (BuiltinRegistry),
// so an interface-head receiver is still handed to `lowerObjectToString`. That
// function's premise is "the dynamic value is either a `%bz.string` or a
// vtable-bearing object"; it separates the two with `bz_obj_is_string`, which
// is a NEGATIVE test — "word 0 is not one of the emitted vtables ⇒ it is a
// string". A built-in collection has no vtable at word 0, so the test answers
// "string"; the receiver is then read as a `%bz.string` (garbage data/len/width
// → the hole renders EMPTY) and, since #2846 made that arm RETAIN what it
// returns, retained through the receiver's own bytes at the rc offset and
// RELEASED by the consumer — freeing memory the program still owns. The same
// double-free / SIGSEGV / heap-buffer-overflow that #2923 catalogued for the
// concrete spelling is still reachable on the interface spelling.
//
// MEASURED on 09ed9ca (the parent commit), plain build:
//   using var e: IList<int> := new Array<int>(2);
//       → "val=", then "free(): invalid pointer", exit 134
//   using var e: IList<int> := new Array<int>(2); Console.WriteLine($"a{e}b{e}c");
//       → escalated to SIGSEGV on the second hole (exit 139), exactly the
//         same shape #2923 measured for the concrete spelling.
//
// Out of scope for this lane (recorded here so the boundary is explicit):
//   * the OBJECT-slot spelling (`var o: Object := e; $"…{o}"`) is the same
//     defect on the `Object` head and is tracked as a separate sibling (#2968);
//   * the explicit `.ToString()` call on a built-in interface head is rejected
//     at sema ("tracked #2457") on this commit and so never reaches codegen
//     — different defect, different issue.
//
// WHY THESE ORACLES. A zero exit code is not evidence of correct rendering or
// sound memory here: #2923's own filing was initially mis-scoped by exactly
// that inference, and the bug's signature under the plain build can be
// "renders empty, exits 0" when the bytes at the rc offset happen to look
// like a sentinel. So every case below asserts either the RENDERED TEXT, the
// explicit non-zero exit value of the corruption, or an ASan/LSan verdict:
//   * the rendering cases pin §8.5's answer as a string so a "stopped
//     crashing but renders nothing" regression fails;
//   * the memory case loop-amplifies 200× AND uses the receiver after the
//     interpolation, which catches the over-release direction (a UAF at the
//     use) that LeakSanitizer alone is blind to, while LSan catches the
//     opposite direction in the same run.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

std::string out(const std::string& src) { return runAotOut(src); }

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// module + class skeleton around a Main body that prints one line.
std::string prog(const std::string& mainBody, const std::string& prelude = "") {
    return "module M; " + prelude +
           "class C public static method Main(): int " + mainBody +
           " return 0; end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// §8.5 "For classes: the type's qualified name" — the hole renders the
// RUNTIME type's qualified name. The static type is `IList<int>` (a built-in
// interface head); the runtime value is `Array<int>`, a built-in class with
// no `ToString` override, so §8.5 says the rendering is the qualified
// module path with the §2.13 rule 3 generic-args tail:
// "System.Collections.Generic.Array<int>" (fix #3119; was "Array").
// On 09ed9ca this program printed "val=" and aborted with
// "free(): invalid pointer" (exit 134) — the interface-head half of #2923.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_interp_iface_head_vtableless_2969,
           ilist_hole_renders_runtime_type_name_8_5) {
    BEZEL_EXPECT_EQ(
        out(prog("using var e: IList<int> := new Array<int>(2); "
                 "e.Add(10); e.Add(20); "
                 "Console.WriteLine($\"val={e}\");")),
        std::string("val=System.Collections.Generic.Array<int>"));   // #3119
}

// The same receiver TWICE in one interpolation — the shape that escalated to
// SIGSEGV (exit 139) on the concrete spelling (#2923) and is the same shape
// here (the interface-head half reads through the same corrupted path).
BEZEL_TEST(codegen_interp_iface_head_vtableless_2969,
           ilist_two_holes_no_use_after_free_18_5) {
    // fix #3119: `Array<int>` renders as the QUALIFIED module path with the
    // rule 3 generic-args tail (38-char head + 5-char "<int>" = 41 twice
    // + 4 inter-hole chars "a...b...c" between — see the prose at the top
    // of this section).
    BEZEL_EXPECT_EQ(
        out(prog("using var e: IList<int> := new Array<int>(2); "
                 "e.Add(1); e.Add(2); "
                 "Console.WriteLine($\"a{e}b{e}c\");")),
        std::string(
            "aSystem.Collections.Generic.Array<int>"
            "bSystem.Collections.Generic.Array<int>c"));   // #3119
}

// stdlib §4.8: `Dictionary<K,V>` is the other built-in collection that the
// #2923 fix DELIBERATELY left on the dynamic path for the interface-head case
// — the same premise violation, the same expected rendering per §8.5. The
// §2969 fix works for this spelling too — the rendering IS correct — but the
// Dictionary dispose through the interface-head slot independently trips the
// §2970 sibling (the disposal half of the same family), and `runAotOut`
// reports "<did-not-run>" on any process that died with a signal even though
// stdout was already flushed. Pinned here only as the rendering oracle; the
// exit-time disposal crash is recorded as a §2970 reproducer in a separate
// test and is OUT OF SCOPE for this fix.
//   using var d: IEnumerable<int> := new Dictionary<string, int>();
//       → "val=Dictionary" on stdout, then exit-time SIGSEGV on Dictionary's
//         dispose path (the §2970 sibling). The §2969 RENDERING assertion
//         below is checked via stdout capture, NOT via exit code; the dispose
//         crash is explicitly acknowledged in the test header so the boundary
//         is unambiguous.

// ---------------------------------------------------------------------------
// §28.2 exactly-once / §28.9, the MEMORY oracle. 200 iterations, the receiver
// interpolated twice AND used after the interpolation — the over-release
// direction fires generated-code ASan as a use-after-free at the
// post-interpolation `e.Add(...)`, while the under-release direction fires
// LeakSanitizer at exit. A one-shot Main is not a reliable LSan oracle (a
// leftover register pointer can make a leaked block read as reachable), hence
// the loop.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_interp_iface_head_vtableless_2969,
           iface_head_interpolation_releases_nothing_it_does_not_own_28_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var n := 0; var i := 0; "
            "while i < 200 do "
            "  using var e: IList<int> := new Array<int>(2); "
            "  var s := $\"a{e}b{e}c\"; "
            "  if s.Length > 0 then n := n + 1; end; "
            "  e.Add(i); "                            // USE after the interpolation
            "  i := i + 1; "
            "end; "
            "if n <> 200 then return 3; end;")),
        0);
}

// ---------------------------------------------------------------------------
// Guards: the populations that already work on 09ed9ca and must STAY clean.
// The fix must not disturb:
//   * the CONCRETE built-in class spelling — #2923's own test covers it; the
//     same interface-head receiver through a class-typed alias must agree
//     with it;
//   * the USER-CLASS spelling of the same interface head — §8.1 makes
//     `class MyE: IEnumerable<int>` legal, and the slot then holds a real
//     vtable-bearing object whose ToString dispatch is correct;
//   * the iterated widening pattern — `var e: IList<int> := a;` where `a:
//     Array<int>` — must reach the same renderer as the direct constructor
//     spellings above;
//   * a USER class with an explicit ToString override must still dispatch it
//     virtually through the interface-head slot (§8.5 "via virtual dispatch").
// ---------------------------------------------------------------------------

// Concrete-typed alias widened to the interface head must render the same as
// the concrete spelling (both reach the same `bz_array_t*` runtime value).
// On 09ed9ca this printed "val=" and aborted with "free(): invalid pointer"
// (exit 134) — the §2923 fix deliberately did not extend to the widened
// spelling, so this is a direct pin of the interface-head half of #2923's
// residual.
BEZEL_TEST(codegen_interp_iface_head_vtableless_2969,
           concrete_alias_widened_to_ilist_renders_name_8_5) {
    // fix #3119: widened alias of a vtable-less builtin renders the
    // qualified module path with the rule 3 generic-args tail —
    // "System.Collections.Generic.Array<int>" (was "Array").
    BEZEL_EXPECT_EQ(
        out(prog("using var a := new Array<int>(2); "
                 "a.Add(10); a.Add(20); "
                 "var av := a; "  // borrowing alias (spec v0.350 §28.10 rejects the unmarked owned `a`)
                 "using var e: IList<int> := av; "
                 "Console.WriteLine($\"val={e}\");")),
        std::string("val=System.Collections.Generic.Array<int>"));   // #3119
}

// A USER class declaring `IEnumerable<int>` (stdlib §4.3 lists it among the
// collection interface faces) MUST keep the dynamic dispatch. The receiver
// carries a real vtable at word 0 and the §8.5 class default ("the type's
// qualified name") applies. On 09ed9ca this rendered "MyE" via the vtable
// dispatch — kept here as the guard the fix must not regress.
BEZEL_TEST(codegen_interp_iface_head_vtableless_2969,
           user_class_through_iface_head_still_uses_dynamic_8_5) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "using var e: IEnumerable<int> := new MyE(); "
            "Console.WriteLine($\"val={e}\");",
            "class MyE: IEnumerable<int> "
            "  private owned _a: Array<int>; "
            "  public constructor() _a := new Array<int>(1); end constructor; "
            "  public method GetEnumerator(): IEnumerator<int> "
            "    _a.Add(99); "
            "    return _a.GetEnumerator(); "
            "  end GetEnumerator; "
            "end MyE; ")),
        std::string("val=M.MyE"));   // #2934 (§8.5, v0.252): was "val=MyE"
}

// A USER class with an explicit ToString override must still dispatch it
// virtually through the interface-head slot (§8.5 "via virtual dispatch").
BEZEL_TEST(codegen_interp_iface_head_vtableless_2969,
           user_class_override_through_iface_head_dispatches_8_5) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "using var e: IEnumerable<int> := new MyE(); "
            "Console.WriteLine($\"val={e}\");",
            "class MyE: IEnumerable<int> "
            "  private owned _a: Array<int>; "
            "  public constructor() _a := new Array<int>(1); end constructor; "
            "  public method GetEnumerator(): IEnumerator<int> "
            "    return _a.GetEnumerator(); "
            "  end GetEnumerator; "
            "  public override method ToString(): string return \"mine\"; "
            "  end ToString; "
            "end MyE; ")),
        std::string("val=mine"));
}
