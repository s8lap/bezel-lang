// Regression tests for #8037 — `StringBuilder.AppendFormat` SEGFAULTED when
// one of its `args` was an INT held in an `Array<Object>` slot.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   stdlib §3.1 (v0.355), the declared signature, verbatim:
//     "public method AppendFormat(format: string, args: Array<Object>): StringBuilder;"
//
//   ⇒ The `args` parameter is a single `Array<Object>`-typed parameter — NOT a
//   `params`/variadic `object[]` list. An `Array<Object>` argument binds to it
//   DIRECTLY, with no element-type widening step at the call.
//
//   stdlib §3.1 (v0.355), the Contracts clause, verbatim:
//     "`AppendFormat`'s format string contains literal text and holes `{n}` —
//      `n` a zero-based index into `args` — each hole optionally carrying a
//      format spec after a colon (`{0:F2}`), interpreted per the §2.18
//      vocabulary against the argument's runtime type; `{{` and `}}` escape
//      literal braces."
//
//   ⇒ AppendFormat substitutes THE STRING FORM of `args[N]` — the text
//   `args[N].ToString()` produces (spec §8.5).
//
//   spec §8.6 (v0.355), the boxing rule, verbatim:
//     "When a value type is used where `Object` (or an interface implemented by
//      the value type) is expected, the value is **boxed**: a heap-allocated
//      wrapper holds the value."
//
//   ⇒ AN INT IN AN `Object` SLOT IS LEGAL AND IS BOXED. The issue's own "what
//   must hold after the fix" item 1 — "REJECTED at compile time with a real
//   diagnostic" — is a FALSE REJECTION: it would refuse a program the spec
//   admits. The program must PRINT the int, not be rejected and not crash.
//
//   spec §9.6 (v0.355): a `string`'s `ToString()` is the IDENTITY — its own
//   text — so a `string` argument's string form is the string itself, and the
//   two routes MUST agree: `args[N].ToString()` and `AppendFormat("{0}", args)`
//   render the same text.
//
// THE DEFECT (re-derived on this lane's tip, d13e2fbd, and confirmed from the
// emitted IR — not merely from the symptom).
//
//   An `Array<Object>` slot holds an OBJECT: a class instance, a `string`, or a
//   §8.6 BOX (whose word 0 is a vtable). The three ways a program puts a
//   VALUE-typed value into such a slot did not all box it:
//
//     * `Array<T>.Add(item)`          — BOXES (the `boxForTarget` call in the
//                                       generic Add/Push/Enqueue arm).
//     * `new Array<T>(size, fill)`    — DID NOT BOX. The fill value was
//                                       spilled raw into the pointer-sized
//                                       element slot (a 4-byte `i32` store
//                                       into an 8-byte alloca), so the slot
//                                       held the integer 7 READ AS A POINTER.
//     * `Array<T>[i] := value`        — DID NOT BOX (same raw store).
//     * `Array<T>.Insert(index, item)`— DID NOT BOX (same raw store).
//
//   The first `Object` operation on such an element then read the slot's word
//   0 as a vtable. MEASURED on the tip: `new Array<Object>(1, 7)` +
//   `sb.AppendFormat("{0}", args)` died with SIGSEGV (exit 139) inside
//   `bz_obj_is_string` with `rdi = 0x000000000007` — the integer 7 read as an
//   address. `args[0] := 3` and `args.Insert(0, 3)` died the same way with
//   `rdi = 0x3`.
//
//   ⇒ THE FIX IS §8.6's BOXING AT THE ELEMENT STORE, on the three paths that
//   lacked it. It is NOT a rejection: the call is legal and must print.
//
// ORACLES.
//
//   Every row asserts a VALUE — the printed text — never "it compiles". The
//   rows are paired: the element-store routes (the fill constructor, the
//   indexer setter, `Insert`) are RED on the pre-fix compiler (the program
//   dies on a signal, so `r.ran` is false) and GREEN after the fix; the
//   `.Add` route is the POSITIVE CONTROL (green on both arms — it already
//   boxed), and the string/class routes are the false-rejection guards the
//   issue names.
//
//   THE LEAK ORACLE. The rows that put a BOXED value into an `Array<Object>`
//   use the plain `runAot` oracle, and say why: the box is a +1 producer
//   whose reference the borrowing array's slot does NOT discharge, so the
//   program leaks 24 bytes. MEASURED, and PRE-EXISTING: the ALREADY-CORRECT
//   `.Add(3)` route leaks exactly the same 24 bytes on the pre-fix compiler —
//   "SUMMARY: AddressSanitizer: 24 byte(s) leaked in 1 allocation(s)" — so the
//   leak is a property of the box's lifetime in a borrowing `Array<Object>`
//   slot (the shape issue #2791 fixed for the COLLECTION-LITERAL path), NOT
//   of this fix, and this fix does not make it worse. It is reported to the
//   coordinator. The rows that do NOT box (a `string` element, a class
//   element) run on `runAotChecked` — the ASan + LSan oracle — so the leak
//   oracle really runs on this file, and the 100x loop row runs under
//   `BEZEL_AOT_FORCE_SANITIZE=address` (ASan-instrumented generated code, with
//   LSan off because `runAot` runs programs with `detect_leaks=0`), so a
//   use-after-free or a double-free of the box is caught even though the
//   pre-existing leak is not.
//
//   NOT PINNED HERE: the format SPECIFIER (`{0:D}`). stdlib §3.1 says a hole
//   may carry one, "interpreted per the §2.18 vocabulary"; the runtime
//   helper does not implement it (MEASURED: `{0:D}` over a boxed int renders
//   "3", the un-specified rendering). That gap is a SEPARATE, PRE-EXISTING
//   defect — it is the same for a `string` and for a class argument, and it
//   is not this issue's shape — so no row here freezes it.
//
//   NOT PINNED HERE: the rendering of a NULL element. stdlib §3.1 requires the
//   hole to be read "against the argument's runtime type", and a null has no
//   runtime type, so the documents do not fix it. The row below pins the
//   CURRENT rendering of the ALREADY-CORRECT `.Add` route and requires the
//   repaired routes to AGREE with it — the "two routes of one operation" rule
//   of §9.6 — rather than asserting a fresh answer.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The REPAIRED route under test: the sized `Array<T>` constructor's fill
// value. §4.4 (stdlib): "public constructor(size: int, fill: T); // Count =
// size, every slot = fill (#1375)". With `T = Object` the fill value is an
// INT, and §8.6 requires it to be BOXED before it reaches the slot.
//
// MEASURED on the pre-fix tip d13e2fbd: exit 139 (SIGSEGV) inside
// `bz_obj_is_string` with `rdi = 0x7` — the integer 7 read as a pointer.
// AFTER the fix: exit 0 and "3" — the int's own text (spec §8.5).
std::string fillCtorIntProgram() {
    return
        "module M;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var sb := new StringBuilder();\n"
        "    using var args: Array<Object> := new Array<Object>(1, 3);\n"
        "    sb.AppendFormat(\"{0}\", args);\n"
        "    Console.WriteLine(sb.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
}

}  // namespace

// #8037 / spec §8.6 / stdlib §3.1 (v0.355) — THE SIZED CONSTRUCTOR'S FILL.
//
// The RED row: the program died on a signal before the fix, so `r.ran` is
// false there and the row fails. The oracle is the printed text, because
// "the program ran" alone would accept a fix that renders the WRONG thing.
BEZEL_TEST(codegen_sb_append_format_object_array_8037,
           sized_array_constructor_fill_of_an_int_renders_the_int) {
    auto r = runAot(fillCtorIntProgram());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// #8037 / spec §8.6 / stdlib §3.1 (v0.355) — THE INDEXER SETTER.
//
// `args[0] := 3` puts an int into an `Object` slot. §8.6 boxes it. The
// displaced element is a borrow (a borrowing `Array<T>` does not own its
// elements — §4.4), so the overwrite itself is sound.
//
// MEASURED on the pre-fix tip d13e2fbd: exit 139 (SIGSEGV) with `rdi = 0x3`.
BEZEL_TEST(codegen_sb_append_format_object_array_8037,
           indexer_set_of_an_int_renders_the_int) {
    auto r = runAot(
        "module M;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var sb := new StringBuilder();\n"
        "    using var args: Array<Object> := new Array<Object>();\n"
        "    args.Add(\"x\");\n"
        "    args[0] := 3;\n"
        "    sb.AppendFormat(\"{0}\", args);\n"
        "    Console.WriteLine(sb.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// #8037 / spec §8.6 / stdlib §3.1 (v0.355) — `Insert`.
//
// The third element store that did not box.
//
// MEASURED on the pre-fix tip d13e2fbd: exit 139 (SIGSEGV) with `rdi = 0x3`.
BEZEL_TEST(codegen_sb_append_format_object_array_8037,
           insert_of_an_int_renders_the_int) {
    auto r = runAot(
        "module M;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var sb := new StringBuilder();\n"
        "    using var args: Array<Object> := new Array<Object>();\n"
        "    args.Add(\"x\");\n"
        "    args.Insert(0, 3);\n"
        "    sb.AppendFormat(\"{0}\", args);\n"
        "    Console.WriteLine(sb.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// #8037 / spec §8.6 / stdlib §3.1 / §8.5 (v0.355) — THE ELEMENT-KIND MATRIX.
//
// Every value kind that can sit in an `Object` slot, each through the
// repaired sized-constructor route, each rendered by the ONE operation
// AppendFormat performs (stdlib §3.1's "the §2.18 vocabulary against the
// argument's runtime type"):
//
//   int 3            -> "3"      (§8.5's ordinary ToString)
//   long 7           -> "7"
//   double 1.5       -> "1.5"    (§2.18: the shortest form that parses back)
//   bool true        -> "True"   (§9.4's boolean rendering)
//   char 'c'         -> "c"
//   string "str"     -> "str"    (§9.6: a string's ToString is the identity)
//   record Pt(1, 2)  -> "Pt(x: 1, y: 2)"  (§9.2/§9.3's positional-record
//                                 default)
//
// The record is a VALUE type, so it is BOXED on this route too — a
// non-borrow-bearing record, so §8.6's borrow-bearing-record restriction does
// not apply.
//
// MEASURED on the pre-fix tip d13e2fbd: exit 139 (the first int already
// killed it).
BEZEL_TEST(codegen_sb_append_format_object_array_8037,
           every_value_kind_renders_through_the_sized_constructor) {
    auto r = runAot(
        "module M;\n"
        "public record Pt(x: int, y: int);\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var sb := new StringBuilder();\n"
        "    using var a0: Array<Object> := new Array<Object>(1, 3);\n"
        "    using var a1: Array<Object> := new Array<Object>(1, 7L);\n"
        "    using var a2: Array<Object> := new Array<Object>(1, 1.5);\n"
        "    using var a3: Array<Object> := new Array<Object>(1, true);\n"
        "    using var a4: Array<Object> := new Array<Object>(1, 'c');\n"
        "    using var a5: Array<Object> := new Array<Object>(1, \"str\");\n"
        "    using var a6: Array<Object> := new Array<Object>(1, new Pt(1, 2));\n"
        "    sb.AppendFormat(\"{0}|\", a0);\n"
        "    sb.AppendFormat(\"{0}|\", a1);\n"
        "    sb.AppendFormat(\"{0}|\", a2);\n"
        "    sb.AppendFormat(\"{0}|\", a3);\n"
        "    sb.AppendFormat(\"{0}|\", a4);\n"
        "    sb.AppendFormat(\"{0}|\", a5);\n"
        "    sb.AppendFormat(\"{0}|\", a6);\n"
        "    Console.WriteLine(sb.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("3|7|1.5|True|c|str|Pt(x: 1, y: 2)|\n"));
}

// #8037 / spec §8.6 / stdlib §3.1 (v0.355) — TWO HOLES, TWO ELEMENTS.
//
// The issue's own item 2 names the two-hole spelling. The format is "{0}{1}"
// over an `Array<Object>` with TWO int elements, filled by the repaired
// sized constructor — so both slots carry a §8.6 box.
//
// MEASURED on the pre-fix tip d13e2fbd: exit 139.
BEZEL_TEST(codegen_sb_append_format_object_array_8037,
           two_holes_over_two_int_elements_render_both) {
    auto r = runAot(
        "module M;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var sb := new StringBuilder();\n"
        "    using var args: Array<Object> := new Array<Object>(2, 3);\n"
        "    sb.AppendFormat(\"{0}{1}\", args);\n"
        "    Console.WriteLine(sb.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("33\n"));
}

// #8037 / spec §8.6 (v0.355) — A NULL ELEMENT.
//
// A null in an `Object` slot is legal (§7.1) and the slot holds a null
// reference; §8.6 has nothing to box. The hole's rendering is read "against
// the argument's runtime type" (stdlib §3.1) and a null has no runtime type,
// so the DOCUMENTS do not fix it — this row pins the CURRENT rendering of the
// already-correct `.Add` route and requires the repaired route to AGREE with
// it.
//
// The row therefore also carries the ROUTE-AGREEMENT oracle: the `.Add` route
// and the sized-constructor route must print the same text.
//
// MEASURED: the `.Add` route renders the empty text (the runtime helper's
// "a null/undispatchable arg yields \"\"") — unchanged by this fix.
BEZEL_TEST(codegen_sb_append_format_object_array_8037,
           a_null_element_renders_the_same_on_both_routes) {
    auto r = runAot(
        "module M;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var sb := new StringBuilder();\n"
        "    using var added: Array<Object> := new Array<Object>();\n"
        "    added.Add(null);\n"
        "    using var filled: Array<Object> := new Array<Object>(1, null);\n"
        "    sb.AppendFormat(\"[{0}]\", added);\n"
        "    sb.AppendFormat(\"[{0}]\", filled);\n"
        "    Console.WriteLine(sb.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("[][]\n"));
}

// #8037 / spec §9.6 / §8.5 (v0.355) — THE TWO ROUTES AGREE.
//
// stdlib §3.1 says AppendFormat substitutes the string form of `args[N]`,
// and spec §8.5 makes that the value's ordinary `ToString()`. So the
// formatted text and `args[N].ToString()` are the SAME operation and MUST
// produce the same text. The boxed int's `ToString()` reads the box's
// PAYLOAD (spec §8.6), so both routes answer "3".
//
// This row is what makes the fix's SHAPE checkable rather than merely
// crash-free: it pins that the repaired slot holds a real §8.6 BOX (whose
// ToString speaks about the VALUE), not a raw integer that happens not to
// crash.
BEZEL_TEST(codegen_sb_append_format_object_array_8037,
           the_element_to_string_and_the_formatted_text_agree) {
    auto r = runAot(
        "module M;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var args: Array<Object> := new Array<Object>(1, 3);\n"
        "    Console.WriteLine(args[0].ToString());\n"
        "    using var sb := new StringBuilder();\n"
        "    sb.AppendFormat(\"[{0}]\", args);\n"
        "    Console.WriteLine(sb.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n[3]\n"));
}

// #8037 / issue item 3 (v0.355) — THE NAMED SPELLING.
//
// The issue requires the POSITIONAL and the NAMED spellings to behave the
// same. Both the constructor's named arguments (`size :=`, `fill :=`) and
// AppendFormat's (`format :=`, `args :=`) are exercised here.
//
// MEASURED on the pre-fix tip d13e2fbd: exit 139.
BEZEL_TEST(codegen_sb_append_format_object_array_8037,
           the_named_spelling_behaves_the_same) {
    auto r = runAot(
        "module M;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var sb := new StringBuilder();\n"
        "    using var args: Array<Object> :=\n"
        "        new Array<Object>(size := 1, fill := 7);\n"
        "    sb.AppendFormat(args := args, format := \"{0}\");\n"
        "    Console.WriteLine(sb.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// CONTROL — THE ISSUE'S OWN REPRO, VERBATIM, AND THE `.Add` ROUTE.
//
// This is the program the issue filed, with the rendering printed so the
// oracle is a VALUE and not "it ran":
//
//     var sb := new StringBuilder();
//     var args: Array<Object> := new Array<Object>();
//     args.Add(3);
//     sb.AppendFormat("{0}", args);
//
// The `.Add` route ALREADY boxed (the generic Add/Push/Enqueue arm's
// `boxForTarget` call), so this row is GREEN on both arms — it is the
// POSITIVE CONTROL that shows the repaired routes now behave like the one
// that was already right, and it is the issue's item 2 false-rejection guard
// (a legal `.Add(3)` must keep working).
BEZEL_TEST(codegen_sb_append_format_object_array_8037,
           the_add_route_still_renders_the_int) {
    auto r = runAot(
        "module M;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var sb := new StringBuilder();\n"
        "    using var args: Array<Object> := new Array<Object>();\n"
        "    args.Add(3);\n"
        "    sb.AppendFormat(\"{0}\", args);\n"
        "    Console.WriteLine(sb.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// CONTROL — A STRING ELEMENT (the issue's false-rejection guard, LSan-clean).
//
// The issue's item 2: "sb.AppendFormat("{0}", someString) must still work".
// A `string` is a REFERENCE type: it reaches the `Object` slot as itself
// (§9.6), so §8.6 boxes nothing and the row is LSan-clean — which is why it
// runs on the `runAotChecked` (ASan + LSan) oracle.
//
// MEASURED on the pre-fix tip: GREEN on both arms — as a control must be.
BEZEL_TEST(codegen_sb_append_format_object_array_8037,
           a_string_element_is_unchanged) {
    auto r = runAotChecked(
        "module M;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var sb := new StringBuilder();\n"
        "    using var args: Array<Object> := new Array<Object>(1, \"seven\");\n"
        "    sb.AppendFormat(\"{0}\", args);\n"
        "    Console.WriteLine(sb.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("seven\n"));
}

// CONTROL — A CLASS ELEMENT (the vtable-bearing arm, LSan-clean).
//
// A class instance is a REFERENCE type: it reaches the `Object` slot as its
// bare pointer, and the runtime helper's vtable dispatch (slot 3 = ToString)
// is the arm it was written for. The row is the positive control for the
// whole fix: without it, a fix that answered "string" for EVERY argument
// would pass the rows above.
//
// MEASURED on the pre-fix tip: GREEN on both arms — as a control must be.
BEZEL_TEST(codegen_sb_append_format_object_array_8037,
           a_class_element_is_unchanged) {
    auto r = runAotChecked(
        "module M;\n"
        "public class W public override method ToString(): string => \"W\"; end W;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var sb := new StringBuilder();\n"
        "    using var w := new W();\n"
        "    using var args: Array<Object> := new Array<Object>(1, w);\n"
        "    sb.AppendFormat(\"x{0}y\", args);\n"
        "    Console.WriteLine(sb.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("xWy\n"));
}

// CONTROL — A VALUE-TYPED ELEMENT SLOT IS STILL STORED BY COPY.
//
// §4.4 (stdlib): "Value types (byte, int, …) are stored by copy; class and
// interface elements are stored as borrowed references". `Array<int>`'s
// element slot holds the int ITSELF — §8.6 does not apply (the slot is not an
// `Object` position), and the fix must not box it. `Array<string>`'s slot
// holds the string reference.
//
// MEASURED on the pre-fix tip: GREEN on both arms — as a control must be.
BEZEL_TEST(codegen_sb_append_format_object_array_8037,
           a_value_typed_element_slot_is_still_stored_by_copy) {
    auto r = runAotChecked(
        "module M;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var ints: Array<int> := new Array<int>(2, 42);\n"
        "    Console.WriteLine(ints[0].ToString());\n"
        "    Console.WriteLine(ints[1].ToString());\n"
        "    using var strs: Array<string> := new Array<string>(2, \"x\");\n"
        "    Console.WriteLine(strs[0]);\n"
        "    Console.WriteLine(strs[1]);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n42\nx\nx\n"));
}

// THE 100x LOOP — the leak / use-after-free / double-free oracle.
//
// One hundred iterations, each minting a fresh §8.6 box through the repaired
// sized-constructor route and reading it back through AppendFormat. The
// oracle is the COUNT of iterations whose rendered text matches the int —
// "100" — so a use-after-free (a box freed and reused) or a double-free
// shows up as a wrong count or as a crash.
//
// Run under `BEZEL_AOT_FORCE_SANITIZE=address` the generated code is
// ASan-instrumented, so a use-after-free or a double-free of the box aborts
// the program (and the row fails). `runAot` runs its programs with
// `detect_leaks=0` (#1822), so the PRE-EXISTING 24-byte box leak (the same
// one the already-correct `.Add` route has — see the file comment) does not
// mask the value oracle.
//
// MEASURED on the pre-fix tip d13e2fbd: exit 139 on the FIRST iteration.
BEZEL_TEST(codegen_sb_append_format_object_array_8037,
           the_sized_constructor_route_survives_a_hundred_iterations) {
    auto r = runAot(
        "module M;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 0..<100 do\n"
        "      using var sb := new StringBuilder();\n"
        "      using var args: Array<Object> := new Array<Object>(1, i);\n"
        "      sb.AppendFormat(\"{0}\", args);\n"
        "      if sb.ToString() = i.ToString() then total := total + 1; end;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}
