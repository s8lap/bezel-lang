// Spec-derived regression test for issue #2923.
//
// §18.5 (Interpolated strings) / §8.5 (`ToString`) / §8.1 (`Object` is the
// universal root) / §28.9 (Runtime-managed types), v0.240. stdlib §3.1
// (`StringBuilder`), stdlib §6.2 (`Mutex`), stdlib §3 (`Random`).
//
// THE RULE. §18.5, verbatim: "A hole *without* a format spec formats via the
// value's ordinary `ToString()` ([§8.5])." §8.5, verbatim: "`ToString` is
// invoked implicitly inside string interpolation ([§18.5]) via virtual dispatch
// — for present values", and its compiler-generated default, verbatim: "For
// classes: the type's qualified name." Every stdlib built-in named here is
// declared `public class` (stdlib §3, §3.1, §6.2, §6.4, §4.4), so §8.5's class
// bullet governs each — except `StringBuilder`, which DECLARES its own
// `public method ToString(): string` (stdlib §3.1) and therefore renders its
// content, not its name.
//
// THE DEFECT. `emitValueToString`'s declaration-less fallback handed the
// receiver to `lowerObjectToString`, whose premise is that the dynamic value is
// either a `%bz.string` or an object with a Bezel vtable at word 0. It
// separates the two with `bz_obj_is_string`, a NEGATIVE test — "word 0 is not
// one of the emitted vtables ⇒ it is a string". A built-in class is a plain
// runtime C struct with no vtable, so the test answered "string" for every one
// of them; the receiver was then read as a `%bz.string` (garbage data/len/width
// → the hole rendered EMPTY) and, since #2846 made that arm RETAIN what it
// returns, retained through the receiver's own bytes at the rc offset and
// RELEASED by the consumer — freeing memory the program still owns.
//
// MEASURED on f069645 (the parent commit):
//
//   using var x := new Mutex(); Console.WriteLine($"val={x}");
//       → prints "val=", then `free(): double free detected in tcache 2`,
//         core dumped, exit 134
//   using var x := new Mutex(); Console.WriteLine($"a{x}b{x}c");
//       → exit 139 (SIGSEGV — the second hole reads freed memory)
//   using var r := new Random(); $"...{r}..."   under generated-code ASan
//       → heap-buffer-overflow READ
//
// SCOPE, measured over a 16-receiver × 5-spelling grid: EVERY built-in class
// receiver reproduced it, including `Array<int>`, `Dictionary<K,V>`,
// `Buffer<T>`, `OwningBuffer<T>` and `Atomic<T>` — which #2923's own scoping had
// classified as unaffected, having inferred "unaffected" from a zero exit code.
// Whether a given receiver aborted, segfaulted, or merely printed an empty hole
// and exited 0 depended only on the byte pattern that happened to sit at the
// `%bz.string` rc offset inside that runtime struct.
//
// THE FIX asks ONE property — "does this receiver carry a Bezel vtable?" —
// through `common::isVtableLessBuiltinClassName`, ahead of the
// `lowerObjectToString` fallback whose premise those receivers violate.
//
// WHY THE ORACLES ARE WHAT THEY ARE. A zero exit code proves nothing here (the
// defect's own filing was mis-scoped by exactly that inference), so every case
// below asserts either the RENDERED TEXT or an ASan/LSan verdict:
//   * the rendering cases pin §8.5's answer as a string, so an "it stopped
//     crashing but renders nothing" regression fails;
//   * the memory case loop-amplifies 200× and USES the receiver after the
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

int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// module + class skeleton around a Main body that prints one line.
std::string prog(const std::string& mainBody, const std::string& prelude = "") {
    return "module M; import System.Threading; " + prelude +
           "class C public static method Main(): int " + mainBody +
           " return 0; end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// §8.5 "For classes: the type's qualified name" — the hole renders the built-in
// class's name. On f069645 this program printed "val=" and aborted with
// "free(): double free detected in tcache 2" (exit 134).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_interp_vtableless_builtin_2923, mutex_hole_renders_type_name_8_5) {
    BEZEL_EXPECT_EQ(
        out(prog("using var x := new Mutex(); "
                 "Console.WriteLine($\"val={x}\");")),
        std::string("val=Mutex"));
}

// The same receiver TWICE in one interpolation — the shape that escalated to
// SIGSEGV (exit 139) on f069645, because the first hole freed what the second
// then read.
BEZEL_TEST(codegen_interp_vtableless_builtin_2923, mutex_two_holes_no_use_after_free_18_5) {
    BEZEL_EXPECT_EQ(
        out(prog("using var x := new Mutex(); "
                 "Console.WriteLine($\"a{x}b{x}c\");")),
        std::string("aMutexbMutexc"));
}

// stdlib §3: `Random` is a class; §8.5's class default applies. Under ASan on
// f069645 this receiver gave a heap-buffer-overflow READ.
BEZEL_TEST(codegen_interp_vtableless_builtin_2923, random_hole_renders_type_name_8_5) {
    BEZEL_EXPECT_EQ(
        out(prog("using var r := new Random(); "
                 "Console.WriteLine($\"[{r}]\");")),
        std::string("[Random]"));
}

// ---------------------------------------------------------------------------
// stdlib §3.1 declares `StringBuilder.ToString(): string`, so §18.5's "formats
// via the value's ordinary `ToString()`" means the CONTENT — §8.5's type-name
// default does not apply to a built-in that declares the member. The two
// spellings must agree: the hole and the explicit call are now one lowering.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_interp_vtableless_builtin_2923, stringbuilder_hole_renders_content_3_1) {
    BEZEL_EXPECT_EQ(
        out(prog("using var sb := new StringBuilder(); "
                 "sb.Append(\"he\"); sb.Append(\"llo\"); "
                 "Console.WriteLine($\"[{sb}]\");")),
        std::string("[hello]"));
}

BEZEL_TEST(codegen_interp_vtableless_builtin_2923, stringbuilder_hole_equals_explicit_tostring_18_5) {
    BEZEL_EXPECT_EQ(
        out(prog("using var sb := new StringBuilder(); "
                 "sb.Append(\"abc\"); "
                 "var viaHole := $\"{sb}\"; "
                 "var viaCall := sb.ToString(); "
                 "Console.WriteLine($\"{viaHole}|{viaCall}\");")),
        std::string("abc|abc"));
}

// ---------------------------------------------------------------------------
// The interpolation hole and the explicit `.ToString()` call are two spellings
// of ONE §8.5 rule and must not disagree. `Array<T>`'s explicit call already
// answered "Array" (the #2457 slice); the hole crashed. Pin them equal.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_interp_vtableless_builtin_2923, array_hole_agrees_with_explicit_tostring_8_5) {
    BEZEL_EXPECT_EQ(
        out(prog("using var a := new Array<int>(2); "
                 "var viaCall := a.ToString(); "
                 "Console.WriteLine($\"{a}|{viaCall}\");")),
        std::string("Array|Array"));
}

// A generic built-in renders its head name, exactly as the explicit call does.
BEZEL_TEST(codegen_interp_vtableless_builtin_2923, dictionary_hole_renders_type_name_8_5) {
    BEZEL_EXPECT_EQ(
        out(prog("using var d := new Dictionary<string, int>(); "
                 "Console.WriteLine($\"{d}\");")),
        std::string("Dictionary"));
}

// ---------------------------------------------------------------------------
// §28.2 exactly-once / §28.9, the MEMORY oracle. 200 iterations, the receiver
// interpolated twice AND used after the interpolation:
//   * over-release  → the post-interpolation `Acquire()` reads freed memory,
//                     which generated-code ASan reports as a use-after-free;
//   * under-release → LeakSanitizer reports the leaked structs at exit.
// A one-shot Main is not a reliable LSan oracle (a leftover register pointer
// makes a leaked block read as reachable), hence the loop.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_interp_vtableless_builtin_2923, interpolation_releases_nothing_it_does_not_own_28_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var n := 0; var i := 0; "
            "while i < 200 do "
            "  using var mx := new Mutex(); "
            "  var s := $\"a{mx}b{mx}c\"; "
            "  if s.Length > 0 then n := n + 1; end; "
            "  using var lk := mx.Acquire(); "   // USE after the interpolation
            "  i := i + 1; "
            "end; "
            "if n <> 200 then return 3; end;")),
        0);
}

BEZEL_TEST(codegen_interp_vtableless_builtin_2923, stringbuilder_hole_leak_free_over_200_iterations_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var n := 0; var i := 0; "
            "while i < 200 do "
            "  using var sb := new StringBuilder(); "
            "  sb.Append(\"xy\"); "
            "  var s := $\"<{sb}>\"; "
            "  if s.Length = 4 then n := n + 1; end; "
            "  i := i + 1; "
            "end; "
            "if n <> 200 then return 4; end;")),
        0);
}

// ---------------------------------------------------------------------------
// Guards: the fix must not disturb the paths whose premise IS satisfied.
// ---------------------------------------------------------------------------

// §8.5 "For classes: the type's qualified name" — a USER class with no
// ToString override still renders its name (the vtable path is untouched).
BEZEL_TEST(codegen_interp_vtableless_builtin_2923, user_class_without_override_still_renders_name_8_5) {
    BEZEL_EXPECT_EQ(
        out(prog("using var u := new U(); Console.WriteLine($\"{u}\");",
                 "class U public constructor() end constructor; end U; ")),
        std::string("M.U"));   // #2934 (§8.5, v0.252): was "U"
}

// A user class WITH a ToString override still dispatches it virtually
// (§8.5 "via virtual dispatch").
BEZEL_TEST(codegen_interp_vtableless_builtin_2923, user_class_override_still_dispatches_8_5) {
    BEZEL_EXPECT_EQ(
        out(prog("using var u := new U(); Console.WriteLine($\"{u}\");",
                 "class U "
                 "  public constructor() end constructor; "
                 "  public override method ToString(): string return \"mine\"; "
                 "  end ToString; "
                 "end U; ")),
        std::string("mine"));
}

// #1118: a generic type-PARAMETER operand is the other inhabitant of the
// declaration-less Named shape and must keep dispatching dynamically — the fix
// asks the built-in question first, and a type-parameter name is in no builtin
// table, so this path is unchanged.
BEZEL_TEST(codegen_interp_vtableless_builtin_2923, generic_type_param_operand_still_dispatches_8_5) {
    BEZEL_EXPECT_EQ(
        out(prog("using var u := new U(); Console.WriteLine(H.Show<U>(u));",
                 "class U "
                 "  public constructor() end constructor; "
                 "  public override method ToString(): string return \"gen\"; "
                 "  end ToString; "
                 "end U; "
                 "class H "
                 "  public static method Show<T>(v: T): string "
                 "    return $\"{v}\"; "
                 "  end Show; "
                 "end H; ")),
        std::string("gen"));
}
