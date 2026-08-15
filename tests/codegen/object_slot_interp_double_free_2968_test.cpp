// Spec-derived regression test for issue #2968.
//
// §8.1 (`Object` is the universal root) / §8.5 (`ToString`) / §18.5
// (Interpolated strings) / §28.9 (Runtime-managed types), v0.240.
// stdlib §6.2 (`Mutex`), stdlib §4.4 (`Array<T>`).
//
// THE RULE. §8.1, verbatim, puts these receivers on the REFERENCE side of the
// boxing line:
//
//   "classes, interfaces, `string`, heap arrays (`Array<T>`,
//    `OwningArray<T>`), and function types are reference types whose values
//    are already heap-allocated and reach `Object` directly through the
//    inheritance chain"
//
// So `var o: Object := mx;` (mx: Mutex) is legal with no conversion, and the
// slot holds the Mutex ITSELF — not a box. §18.5, verbatim: "A hole *without*
// a format spec formats via the value's ordinary `ToString()` ([§8.5])", and
// §8.5's compiler-generated default for classes is "the type's qualified
// name".
//
// THE DEFECT. #2923 fixed the CONCRETE spelling (`$"{mx}"`) by routing a
// receiver whose STATIC type names a vtable-less built-in away from
// `lowerObjectToString`; #2969 fixed the built-in INTERFACE HEAD spelling the
// same way. The `Object`-typed slot was the remaining spelling, and it failed
// for a reason neither of those gates caught: sema models the universal root
// as a PRIMITIVE (`PrimitiveType::Object`, TypeResolution.cpp), NOT as a Named
// TypeRef, so both prior gates — which test Named kinds — were structurally
// unable to fire on it. The hole fell into `emitPrimitiveToString`'s
// `P::Object` arm, which calls `lowerObjectToString`, whose `bz_obj_is_string`
// is a NEGATIVE test ("word 0 is not one of the emitted vtables ⇒ a string").
// A built-in is a plain C struct with no vtable at word 0, so the test answers
// "string": the receiver is read as a `%bz.string` (garbage data/len/width →
// the hole renders EMPTY) and, since #2846 made that arm RETAIN what it
// returns, retained through the receiver's own bytes at the rc offset and
// RELEASED by the consumer — freeing memory the program still owns.
//
// MEASURED on ec4ab7d (the parent commit):
//   using var mx := new Mutex(); var o: Object := mx;
//   Console.WriteLine($"val={o}");
//       → "val=", then "free(): double free detected in tcache 2", exit 134;
//         under generated-code ASan, "attempting double-free" on the 48-byte
//         Mutex allocation.
//
// WHY THESE ORACLES. A zero exit code is not evidence here — the bug's plain
// signature can be "renders empty, exits 0" when the bytes at the rc offset
// happen to look like a sentinel (this mis-scoped #2923's own filing). So the
// rendering cases pin §8.5's answer as TEXT so a "stopped crashing but renders
// nothing" regression still fails, and the memory case uses an ASan/LSan
// verdict with the receiver USED after the interpolation (catching the
// over-release direction as a UAF, which LSan alone is blind to) and a loop
// (a one-shot Main is not a reliable LSan oracle).

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
    return "module M; " + prelude +
           "class C public static method Main(): int " + mainBody +
           " return 0; end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// THE DEFECT ITSELF. §8.5 "For classes: the type's qualified name" — an
// `Object`-typed slot holding a `Mutex` renders the qualified module path
// ("System.Threading.Mutex"). On ec4ab7d this printed "val=" and aborted
// with a double free (exit 134).
//
// fix #3119 (spec §8.5, v0.252; stdlib §2.13 rule 1): the rendered name is
// the QUALIFIED module path, not the bare identifier (#2934 covered the
// `TypeSymbol::moduleName`-populated half; #3119 covers the synthetic-
// builtin half whose `moduleName` is empty by construction).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_object_slot_interp_double_free_2968,
           object_slot_holding_builtin_renders_runtime_type_name_8_5) {
    BEZEL_EXPECT_EQ(
        out(prog("using var mx := new Mutex(); "
                 "var o: Object := mx; "
                 "Console.WriteLine($\"val={o}\");")),
        std::string("val=System.Threading.Mutex"));   // #3119: was "val=Mutex"
}

// The same receiver TWICE in one interpolation, with literal text around and
// between the holes — the shape that escalated the sibling spellings from a
// double free to a SIGSEGV on the second hole (#2923/#2969).
BEZEL_TEST(codegen_object_slot_interp_double_free_2968,
           object_slot_two_holes_no_use_after_free_18_5) {
    BEZEL_EXPECT_EQ(
        out(prog("using var mx := new Mutex(); "
                 "var o: Object := mx; "
                 "Console.WriteLine($\"a{o}b{o}c\");")),
        std::string("aSystem.Threading.MutexbSystem.Threading.Mutexc"));   // #3119
}

// stdlib §4.4: a heap ARRAY in an `Object` slot — §8.1 names `Array<T>`
// explicitly in the sentence quoted in the header, so it reaches `Object` as
// itself and hits the identical vtable-less premise violation.
//
// fix #3119 (stdlib §2.13 rule 1 + rule 3): rendering is the QUALIFIED
// module path WITH the generic-args tail — "System.Collections.Generic.Array<int>"
// for `Array<int>`.
BEZEL_TEST(codegen_object_slot_interp_double_free_2968,
           object_slot_holding_array_renders_name_8_1) {
    BEZEL_EXPECT_EQ(
        out(prog("using var a := new Array<int>(2); "
                 "a.Add(10); a.Add(20); "
                 "var o: Object := a; "
                 "Console.WriteLine($\"val={o}\");")),
        std::string("val=System.Collections.Generic.Array<int>"));   // #3119: was "val=Array"
}

// ---------------------------------------------------------------------------
// §28.2 exactly-once / §28.9, the MEMORY oracle. 200 iterations, the receiver
// interpolated twice AND used after the interpolation: the over-release
// direction fires generated-code ASan as a use-after-free at the
// post-interpolation `Acquire` (stdlib §6.2), while the under-release
// direction fires LeakSanitizer at exit.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_object_slot_interp_double_free_2968,
           object_slot_interpolation_releases_nothing_it_does_not_own_28_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var n := 0; var i := 0; "
            "while i < 200 do "
            "  using var mx := new Mutex(); "
            "  var o: Object := mx; "
            "  var s := $\"a{o}b{o}c\"; "
            "  if s.Length > 0 then n := n + 1; end; "
            "  using var g := mx.Acquire(); "    // USE after the interpolation
            "  i := i + 1; "
            "end; "
            "if n <> 200 then return 3; end;")),
        0);
}

// §7 nullable reference: `Object?` holding a vtable-less built-in takes the
// nullable-ref LIVE arm, which reached `lowerObjectToString` by a different
// route and double-freed identically (measured on ec4ab7d: "p=" then an ASan
// double-free on the 48-byte Mutex allocation). The narrowing applies to that
// arm too; the surrounding runtime null check is unaffected.
BEZEL_TEST(codegen_object_slot_interp_double_free_2968,
           nullable_object_slot_holding_builtin_renders_name_8_1) {
    BEZEL_EXPECT_EQ(
        out(prog("using var mx := new Mutex(); "
                 "var o: Object? := mx; "
                 "Console.WriteLine($\"val={o}\");")),
        std::string("val=System.Threading.Mutex"));   // #3119: was "val=Mutex"
}

// The null arm of the same slot must still render the literal "null" — the
// narrowing must not swallow the runtime null test.
BEZEL_TEST(codegen_object_slot_interp_double_free_2968,
           nullable_object_slot_null_still_renders_null_7) {
    BEZEL_EXPECT_EQ(
        out(prog("var o: Object? := null; "
                 "Console.WriteLine($\"val={o}\");")),
        std::string("val=null"));
}

// ---------------------------------------------------------------------------
// Guards: the populations that already worked on ec4ab7d and must STAY clean.
// The `Object` slot is the one opaque slot that can hold a genuine `string`
// (an interface-head slot cannot), so the narrowing this fix adds is only
// sound because it fires exclusively when the AST source names a vtable-less
// built-in CLASS. These pin that boundary.
// ---------------------------------------------------------------------------

// §8.1 lists `string` among the reference types that "reach `Object` directly".
// A real string in an `Object` slot satisfies `bz_obj_is_string`'s premise, so
// it MUST keep the dynamic path and render its CONTENTS — not the type name.
// This is the case the fix would break if it narrowed on the declared type
// instead of on the initializer's built-in-class-ness.
BEZEL_TEST(codegen_object_slot_interp_double_free_2968,
           object_slot_holding_real_string_renders_contents_8_1) {
    BEZEL_EXPECT_EQ(
        out(prog("var s: string := \"hi\"; "
                 "var o: Object := s; "
                 "Console.WriteLine($\"val={o}\");")),
        std::string("val=hi"));
}

// A USER class in an `Object` slot carries a real vtable at word 0, so §8.5's
// "via virtual dispatch" applies and its ToString override must still win.
BEZEL_TEST(codegen_object_slot_interp_double_free_2968,
           object_slot_holding_user_class_dispatches_override_8_5) {
    BEZEL_EXPECT_EQ(
        out(prog("using var u := new U(); "
                 "var o: Object := u; "
                 "Console.WriteLine($\"val={o}\");",
                 "class U "
                 "  public override method ToString(): string return \"mine\"; "
                 "  end ToString; "
                 "end U; ")),
        std::string("val=mine"));
}

// REASSIGNMENT — the case that makes this fix's narrowing conditional. The
// narrowing reads the local's INITIALIZER type, which describes the slot only
// while the slot still holds the initializer's value. Here the slot starts as
// a `Mutex` and is then reassigned to a string: rendering "Mutex" would be a
// REGRESSION (the pre-fix dynamic path renders a genuine string correctly), so
// the fix must DECLINE the narrowing on a reassigned local. Without the
// `reassignedOpaqueLocals_` guard this prints "val=Mutex".
BEZEL_TEST(codegen_object_slot_interp_double_free_2968,
           reassigned_object_slot_declines_narrowing_8_5) {
    BEZEL_EXPECT_EQ(
        out(prog("using var mx := new Mutex(); "
                 "var o: Object := mx; "
                 "o := \"reassigned\"; "
                 "Console.WriteLine($\"val={o}\");")),
        std::string("val=reassigned"));
}

// The reassignment guard must not over-fire either: a slot reassigned from one
// vtable-less built-in to ANOTHER value still renders by the dynamic path, and
// the program must stay memory-clean under ASan/LSan.
BEZEL_TEST(codegen_object_slot_interp_double_free_2968,
           reassigned_object_slot_stays_memory_clean_28_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var n := 0; var i := 0; "
            "while i < 200 do "
            "  using var mx := new Mutex(); "
            "  var o: Object := mx; "
            "  o := \"s\"; "
            "  var s := $\"a{o}b\"; "
            "  if s.Length > 0 then n := n + 1; end; "
            "  i := i + 1; "
            "end; "
            "if n <> 200 then return 3; end;")),
        0);
}
