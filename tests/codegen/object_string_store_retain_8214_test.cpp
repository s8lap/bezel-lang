// Codegen — a `string` converted to `Object` and stored into a LONGER-LIVED
// slot is not retained: the source's scope-end release frees the string under
// the slot (use-after-free). §28.9 (Runtime-managed types), §4.6, §8.6,
// §17.4 (`as` is a borrow of its operand), v0.355.
//
// THE DEFECT (issue #8214). `as Object` on a `string` operand lowers to the
// OPERAND ITSELF with no retain (src/codegen/LlvmBackend_Layout.cpp:4225 —
// "`as Object` RETURNS the operand — no release"). The store into an
// `Object`-typed class field then took the PLAIN-STORE path: `isBoxFieldLvalue`
// wants a boxable VALUE source (a `string` is not one) and `lowerStringFieldStore`
// wants a `string`-typed TARGET (`O: Object` is not one), so nothing retained.
// The source local's own scope-end `bz_string_release` then freed the very
// string the field still pointed at.
//
// MEASURED on the pre-fix build (`bezelc`, the r30-c1 tip), the issue's own
// repro `y_boxhole_oneshot.bzl`:
//
//   plain build    → `free(): invalid pointer`, rc 134
//   --sanitize=address → SEGV / heap-use-after-free in bz_string_to_utf8z
//
// The IMPLICIT twin (`h.O := x;`) did not crash — the string-escape walk reads
// the implicit store as an ESCAPE and suppresses `x`'s scope-end release, so
// the string survived — but it LEAKED its reference (39 bytes / 2 allocations
// under LSan, `bz$M.Make$Str`). One shape, two symptoms.
//
// THE FIX (narrow). A `:=` store of a STRING-typed source (a `string`, or
// `x as Object` of one) into a class field whose DECLARED type is
// `Object`/interface RETAINS the string, so the field holds its own +1 and the
// source's scope-end release no longer frees the string under it. The
// string-escape walk reads such a store's right-hand side as a SAFE READ (a
// borrow, not an escape), so the SOURCE keeps its own +1 and still releases it
// at its scope end.
//
// NOTHING RELEASES THE FIELD'S +1 — IT LEAKS. The first version of this fix
// also released the field on overwrite and on disposal, keyed on a pre-pass
// that admitted a field only when every store it could see was a string. The
// non-author review (rev-8214) MEASURED that classification unsound: it did
// not see a store inside a block-lambda body (`this.O := f`), a `??=` store,
// or a store made by ANOTHER MODULE into a public field, and a read
// `o := h.O` into an `Object` local that outlives `h` keeps an un-retained
// alias. Each turned the release into a string release of a class instance
// (heap corruption) or a use-after-free, on programs the base ran cleanly.
// An `Object` slot may hold a class instance, a box, a function value or a
// string (§5.1, §8.1), and no static or runtime discriminator says which
// (issue #8215 / #5059), so no release keyed on the slot is sound. A leak is
// the pre-#8214 behaviour for every such slot; a use-after-free is not.
//
// So the rows that store a HEAP string run through `runAotCheckedLenient`:
// the generated-code AddressSanitizer (use-after-free, double free,
// over-release) is armed, LeakSanitizer is not, because the field's +1 is the
// documented residual leak. The rows that store no heap string (the literal,
// the int box, the class instance, the `Array<string>` neighbour) stay on
// `runAotChecked` and are leak-checked.
//
// SCOPE — WHAT THESE ROWS DO NOT CLAIM. Two neighbouring shapes are still
// open and are NOT pinned as fixed here:
//
//   * an `Object`-typed ARRAY element (`Array<Object>`) and an
//     `Object`-typed DICTIONARY value: the collection's element slot has no
//     per-field classification (the element type is `Object` and the value
//     kind is per-ELEMENT), and a borrowing collection does not own its
//     elements, so the release half has no sound home without the runtime
//     discriminator #8215 asks for. MEASURED pre-fix: `arr.Add(x as Object)`
//     aborts with `free(): invalid pointer` (rc 134) and
//     `d["k"] := x as Object` segfaults (rc 139).
//   * a `string` returned from a method whose declared return type is
//     `Object`: the callee's own scope-end release frees the value the
//     caller receives. MEASURED pre-fix: rc 139.
//
// Both are the SAME defect at other `Object`-typed slots; they are not
// touched here, so the subject is `narrow #8214`. The rows below cover the
// CLASS-FIELD shape, which is the one issue #8214 is filed against, the
// review's regressions, and the controls that must not move.
//
// `runAotChecked` arms LeakSanitizer BY CONSTRUCTION (`detectLeaks=true` is a
// hardcoded argument in aot_support.hpp); `runAotCheckedLenient` passes
// `false` and runs `detect_leaks=0`, so NO row on the lenient helper makes a
// leak claim. `BEZEL_AOT_DETECT_LEAKS` is read only by `runAot`, never by the
// `runAotChecked` family.
//
// Every row asserts `!checkedToolchainSkipped(r)` BEFORE any guard that could
// absorb the skip: a box without clang fails the row loudly rather than
// passing vacuously.

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;
using bezel::test::runAotModules;

namespace {

// The 100 iterations the brief asks for: the defect is a REFCOUNT defect, so
// one iteration can hide it (a single iteration's premature release may not be
// observed before the program ends). 100 makes the per-iteration residue
// unmistakable both ways.
constexpr int kIterations = 100;

std::string repeatLine(const std::string& line, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += line;
    return out;
}

// A class holding ONE `Object`-typed field, the issue's own shape.
const char* kHolderDecl =
    "public class Holder "
    "  public O: Object := \"\"; "
    "end Holder; ";

// The +1 string producer the issue's repro uses (`Make.Str`).
const char* kMakerDecl =
    "public class Make "
    "  public static method Str(n: int): string => \"s\" + n.ToString(); "
    "end Make; ";

// The store under test, wrapped in the issue's `do ... end;` block (the block
// is the scope whose end released the string under the field).
std::string storeInDoBlock(const std::string& statement) {
    return "      do "
           "        var x := Make.Str(12345); "
           + statement +
           "      end; ";
}

// Run `body` (the statements of Main) and require: the program RAN, exited 0,
// printed exactly `expected`, and no sanitizer fired (the runAotChecked
// contract — a leak, a use-after-free or a double free all surface as a
// "sanitizer fired" diagnostic).
void expectCleanRun(const std::string& body, const std::string& expected) {
    JitResult r = runAotChecked(body);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// Run `body` with the generated-code AddressSanitizer armed and LeakSanitizer
// OFF, and require: the program RAN, exited 0, printed exactly `expected`. A
// use-after-free, a double free, an over-release or a crash fails the row; the
// field's documented +1 leak does not (see the file header).
void expectNoUseAfterFree(const std::string& body, const std::string& expected) {
    JitResult r = runAotCheckedLenient(body);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// The class-field shape, parameterised by the store statement.
//
// The holder is created ONCE and the store runs kIterations times inside a
// `for` loop, so the field is OVERWRITTEN kIterations times and each read of
// it happens after the source's scope ended.
std::string fieldProgram(const std::string& storeStatement) {
    return std::string("module M; ") + kHolderDecl + kMakerDecl +
           "public class Program "
           "  public static method Main(): int "
           "    var h := new Holder(); "
           "    for var i in 0..<" + std::to_string(kIterations) + " do "
           + storeInDoBlock(storeStatement) +
           "      Console.WriteLine(h.O.ToString()); "
           "    end for; "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

}  // namespace

// ===========================================================================
// THE ISSUE'S OWN REPRO. `y_boxhole_oneshot.bzl` from issue #8214, verbatim
// apart from the body being wrapped in a 100-iteration loop (the defect is a
// REFCOUNT defect, so one iteration can hide it) and the `Program` class
// holding the loop.
//
// Pre-fix: `free(): invalid pointer` (rc 134) plain; a heap-use-after-free
// under ASan — the `do ... end;` block's scope-end release freed the string
// the field still pointed at, and `Console.WriteLine(h.O.ToString())` read it
// back AFTER the block ended.
//
// Post-fix: the program prints `s12345` / `6` per iteration and ASan reports
// no use-after-free. The field's +1 leaks (the file header says why), so the
// row runs with LeakSanitizer off.
// ===========================================================================
BEZEL_TEST(object_string_store_retain_8214,
           issue_repro_y_boxhole_oneshot_looped_100x_does_not_use_after_free) {
    const std::string program =
        "module M; "
        "public record Named(Tag: string, N: int); "
        "public class Holder "
        "  public V: string := \"\"; "
        "  public O: Object := \"\"; "
        "end Holder; "
        "public class Make "
        "  public static method Str(n: int): string => \"s\" + n.ToString(); "
        "  public static method Maybe(n: int): string? => \"s\" + n.ToString(); "
        "end Make; "
        "public extension string "
        "  public method Len2(): int => this.Length; "
        "end; "
        "public class Program "
        "  public static method Main(): int "
        "    for var i in 0..<" + std::to_string(kIterations) + " do "
        "      var t := 0; "
        "      var h := new Holder(); "
        "      do "
        "        var x := Make.Str(12345); "
        "        h.O := x as Object; "
        "      end; "
        "      var junk := Make.Str(99999); "
        "      Console.WriteLine(h.O.ToString()); "
        "      t := junk.Length; "
        "      Console.WriteLine(t); "
        "    end for; "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    expectNoUseAfterFree(program, repeatLine("s12345\n6\n", kIterations));
}

// ===========================================================================
// THE BUG. The issue's own shape: `h.O := x as Object;` inside a `do ... end;`
// block, then the field is read AFTER the block ended. Pre-fix: the block's
// scope-end release freed the string the field pointed at — `free(): invalid
// pointer` (rc 134) plain, a heap-use-after-free under ASan.
//
// The row is RED pre-fix and GREEN post-fix (no use-after-free; the field's
// +1 is the documented leak).
// ===========================================================================
BEZEL_TEST(object_string_store_retain_8214,
           object_field_explicit_as_object_store_retains_the_string) {
    expectNoUseAfterFree(fieldProgram("h.O := x as Object;"),
                   repeatLine("s12345\n", kIterations));
}

// ===========================================================================
// THE IMPLICIT TWIN. The same store WITHOUT the `as`: `h.O := x;` (§5.1's
// implicit string → Object conversion). Pre-fix this shape did not crash — the
// escape walk read the implicit store as an escape and suppressed `x`'s
// scope-end release — but it LEAKED the reference (39 bytes / 2 allocations).
// The row pins that the implicit conversion behaves EXACTLY like the explicit
// one: same value printed, and no use-after-free.
//
// The two rows together are what make "the implicit conversion must behave
// the same as the explicit one" a MEASURED claim rather than an assertion.
// ===========================================================================
BEZEL_TEST(object_string_store_retain_8214,
           object_field_implicit_conversion_twin_retains_the_string) {
    expectNoUseAfterFree(fieldProgram("h.O := x;"),
                   repeatLine("s12345\n", kIterations));
}

// ===========================================================================
// THE STRING LITERAL — "must not be over-released". A literal is a SENTINEL
// reference (the runtime's literal rc value): bz_string_retain /
// bz_string_release are no-ops on it, so the store's retain must not change
// it, and the row stays leak-checked: a literal store allocates nothing.
// ===========================================================================
BEZEL_TEST(object_string_store_retain_8214,
           object_field_string_literal_store_is_not_over_released) {
    expectCleanRun(fieldProgram("h.O := \"lit\";"),
                   repeatLine("lit\n", kIterations));
}

// ===========================================================================
// THE STRING PARAMETER. The store's source is a by-value `string` PARAMETER
// (not an `Object`-typed one): §28.1 makes a by-value `string` parameter the
// CALLER's borrow, so the callee must take its OWN reference to keep it alive
// past the call — the field's retain is exactly that.
//
// The `Object`-typed parameter shape is NOT here on purpose: its static type
// does not say the value is a string (it may be a class instance), so
// retaining it would be unsound — that is the mixed-slot case #8215 owns.
// ===========================================================================
BEZEL_TEST(object_string_store_retain_8214,
           object_field_string_parameter_store_retains_the_string) {
    const std::string program =
        "module M; " + std::string(kHolderDecl) +
        "public class Make "
        "  public static method Str(n: int): string => \"s\" + n.ToString(); "
        "  public static method Put(h: Holder, s: string) "
        "    h.O := s; "
        "  end Put; "
        "end Make; "
        "public class Program "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    for var i in 0..<" + std::to_string(kIterations) + " do "
        "      var s := Make.Str(12345); "
        "      Make.Put(h, s); "
        "      Console.WriteLine(h.O.ToString()); "
        "    end for; "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    expectNoUseAfterFree(program, repeatLine("s12345\n", kIterations));
}

// ===========================================================================
// CONTROL — AN INT BOX. A value type stored into an `Object`-typed field is
// BOXED (§8.6) and the box is runtime-managed. The box family owns that
// reference (bz_box_release at the owner's disposal), so the row must stay
// clean AND must not be double-released by the string family.
//
// The holder here is a SEPARATE class whose `Object` field's initializer is
// an INT (a box), not a string: a field that receives BOTH a box and a string
// is a MIXED field, which the box pre-pass excludes — so the control uses a
// box-only field, which is the shape the box family actually owns.
// ===========================================================================
BEZEL_TEST(object_string_store_retain_8214,
           object_field_int_box_control_is_unchanged) {
    const std::string program =
        "module M; "
        "public class BoxHolder "
        "  public O: Object := 0; "
        "end BoxHolder; "
        "public class Program "
        "  public static method Main(): int "
        "    var h := new BoxHolder(); "
        "    for var i in 0..<" + std::to_string(kIterations) + " do "
        "      h.O := 3; "
        "      Console.WriteLine(h.O.ToString()); "
        "    end for; "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    expectCleanRun(program, repeatLine("3\n", kIterations));
}

// ===========================================================================
// CONTROL — A CLASS INSTANCE. A class reference stored into an `Object`-typed
// field is NOT a string and NOT a box: it is the borrow case (§9.7 — an
// `Object` position announces no source). The field must neither retain it
// (a `bz_string_retain` on a class instance would corrupt it) nor release it
// at the owner's disposal (the instance's own holder owns it).
//
// The instance is passed as a PARAMETER, because §28.3's borrowed-field store
// rule rejects a scope-bound local as the source ("borrow of `t` stored into
// a field would outlive its source") and admits a parameter- or receiver-
// sourced borrow with a recorded trust clause. The instance is bound with
// `using var` at the call site so its own lifetime is discharged exactly once;
// the field only aliases it, and the alias is read inside the alias's scope.
// ===========================================================================
BEZEL_TEST(object_string_store_retain_8214,
           object_field_class_instance_control_is_unchanged) {
    const std::string program =
        "module M; " + std::string(kHolderDecl) +
        "public class Thing "
        "  public N: int := 7; "
        "end Thing; "
        "public class Make "
        "  public static method Put(h: Holder, t: Thing) "
        "    h.O := t; "
        "  end Put; "
        "end Make; "
        "public class Program "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    for var i in 0..<" + std::to_string(kIterations) + " do "
        "      using var t := new Thing(); "
        "      Make.Put(h, t); "
        "      Console.WriteLine(h.O.ToString()); "
        "    end for; "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    // The default ToString of a user class is its qualified type name (§8.5).
    expectCleanRun(program, repeatLine("M.Thing\n", kIterations));
}

// ===========================================================================
// THE ARRAY-ELEMENT NEIGHBOUR. A `string` stored into an ARRAY ELEMENT of a
// STRING-typed array (`Array<string>`) already had its own reference
// discipline before this fix (the element is retained at the store and
// released when it is displaced). The row pins that the string family did not
// change it.
//
// The `Object`-typed element shape (`Array<Object>`) is NOT claimed here —
// see the SCOPE paragraph in the file header.
// ===========================================================================
BEZEL_TEST(object_string_store_retain_8214,
           array_element_string_store_is_unaffected) {
    const std::string program =
        "module M; "
        "public class Make "
        "  public static method Str(n: int): string => \"s\" + n.ToString(); "
        "end Make; "
        "public class Program "
        "  public static method Main(): int "
        "    using var arr := new Array<string>(); "
        "    for var i in 0..<" + std::to_string(kIterations) + " do "
        "      arr.Add(Make.Str(12345)); "
        "    end for; "
        "    Console.WriteLine(arr[0]); "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    expectCleanRun(program, "s12345\n");
}

// ===========================================================================
// THE STATIC-FIELD NEIGHBOUR. There is no mutable `Object`-typed STATIC field
// in the language: §22.12 rule 2 admits only the primitive/enum value
// statics, the plain heap-free inline-array fields and the synchronized-
// handle/task-cache reference forms, so `public static O: Object := "";` is a
// COMPILE ERROR. The row pins that fact, so the "static field" site of the
// defect list is closed by the front end rather than by a missing retain.
//
// The diagnostic text is the front end's own; the row asserts the compile
// fails and names the rule, so a change that starts admitting the shape
// (which would then need the retain) turns this row red on purpose.
// ===========================================================================
BEZEL_TEST(object_string_store_retain_8214,
           mutable_object_static_field_is_rejected_by_the_front_end) {
    const std::string program =
        "module M; "
        "public class Holder "
        "  public static O: Object := \"\"; "
        "end Holder; "
        "public class Program "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.ran);
    bool mentions = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("22.12") != std::string::npos) mentions = true;
    BEZEL_EXPECT_TRUE(mentions);
}

// ===========================================================================
// THE REVIEW'S REGRESSIONS (rev-8214). Each program ran cleanly on the base
// and crashed on the first version of this fix, whose field release was keyed
// on an unsound "every store is a string" classification. Each is looped 100x
// and must print its exact value with no sanitizer report.
// ===========================================================================

// A block-lambda body stores a CLASS INSTANCE into the field the visible
// stores fill with strings. The first version's pre-pass never descended into
// `LambdaExpr` bodies, so it released the `Foo` as a string (`free(): invalid
// size`).
BEZEL_TEST(object_string_store_retain_8214,
           review_p02_lambda_body_class_store_is_not_string_released) {
    expectNoUseAfterFree(R"BZ(
module M;
public class Foo
  public V: int := 7;
end Foo;
public class Holder
  public O: Object := "";
  public method SetS(s: string)
    this.O := s;
  end SetS;
  public method Fill(f: Foo)
    var g := lambda() => do this.O := f; end;
    g();
  end Fill;
end Holder;
public class Program
  public static method Main(): int
    var i := 0;
    var keep := new Foo();
    while i < 100 do
      var h := new Holder();
      h.SetS("a" + i.ToString());
      h.Fill(keep);
      i := i + 1;
    end;
    Console.WriteLine(keep.V);
    return 0;
  end Main;
end Program;
)BZ", "7\n");
}

// A `??=` store of a class instance. The first version's pre-pass checked only
// `:=` (`refcount over-release (string)` under ASan).
BEZEL_TEST(object_string_store_retain_8214,
           review_p03_null_coalescing_assign_class_store_is_not_string_released) {
    expectNoUseAfterFree(R"BZ(
module M;
public class Foo
  public V: int := 7;
end Foo;
public class Holder
  public O: Object? := null;
  public method SetS(s: string)
    this.O := s;
  end SetS;
  public method Fill(f: Foo)
    this.O ??= f;
  end Fill;
end Holder;
public class Program
  public static method Main(): int
    var i := 0;
    var keep := new Foo();
    while i < 100 do
      var h := new Holder();
      h.Fill(keep);
      i := i + 1;
    end;
    var h2 := new Holder();
    h2.SetS("x" + i.ToString());
    Console.WriteLine(keep.V);
    return 0;
  end Main;
end Program;
)BZ", "7\n");
}

// The field is read into an `Object` local that OUTLIVES the holder. The read
// takes no reference, so the first version's disposal release freed the
// string under `o` (`free(): invalid pointer`).
BEZEL_TEST(object_string_store_retain_8214,
           review_p07_field_read_into_outliving_object_local_stays_alive) {
    expectNoUseAfterFree(R"BZ(
module M;
public class Holder
  public O: Object := "";
end Holder;
public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;
public class Program
  public static method Main(): int
    var i := 0;
    var total := 0;
    while i < 100 do
      var o: Object := "";
      do
        var h := new Holder();
        h.O := Make.Str(i);
        o := h.O;
      end;
      var junk := Make.Str(77777);
      total := total + o.ToString().Length + junk.Length - junk.Length;
      i := i + 1;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
)BZ", "290\n");
}

// ANOTHER MODULE stores a class instance into the library's public field. The
// library's own compile cannot see that store, so the first version released
// the `Foo` as a string. runAotModules has no lenient-checked twin (see
// aot_support.hpp), so the row runs on the plain AOT oracle and its sanitizer
// leg is the BEZEL_AOT_FORCE_SANITIZE=address sweep.
BEZEL_TEST(object_string_store_retain_8214,
           review_p17_cross_module_class_store_is_not_string_released) {
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", R"BZ(
module Lib;
public class Holder
  public O: Object := "";
  public method SetS(s: string)
    this.O := s;
  end SetS;
end Holder;
)BZ"});
    mods.push_back({"App", R"BZ(
module App;
import Lib;
public class Foo
  public V: int := 7;
end Foo;
public class Putter
  public static method Put(h: Holder, f: Foo)
    h.O := f;
  end Put;
end Putter;
public class Program
  public static method Main(): int
    var i := 0;
    var keep := new Foo();
    while i < 100 do
      var h := new Holder();
      h.SetS("a" + i.ToString());
      Putter.Put(h, keep);
      i := i + 1;
    end;
    Console.WriteLine(keep.V);
    return 0;
  end Main;
end Program;
)BZ"});
    JitResult r = runAotModules(mods, "App");
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// ===========================================================================
// CONTROLS (rev-8214's "now clean" set). Each crashed or leaked on the base
// and must keep printing its exact value with no use-after-free.
// ===========================================================================

// p01: the issue's shape in a `while` loop, the holder created per iteration.
BEZEL_TEST(object_string_store_retain_8214, control_p01_issue_loop) {
    expectNoUseAfterFree(R"BZ(
module M;
public class Holder
  public O: Object := "";
end Holder;
public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;
public class Program
  public static method Main(): int
    var i := 0;
    var total := 0;
    while i < 100 do
      var h := new Holder();
      do
        var x := Make.Str(i);
        h.O := x as Object;
      end;
      var junk := Make.Str(99999);
      total := total + h.O.ToString().Length + junk.Length - junk.Length;
      i := i + 1;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
)BZ", "290\n");
}

// p05: a GENERIC class's Object field, stored from a method and directly.
BEZEL_TEST(object_string_store_retain_8214, control_p05_generic_class_field) {
    expectNoUseAfterFree(R"BZ(
module M;
public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;
public class Cell<T>
  public O: Object := "";
  public Tag: T;
  public constructor(t: T)
    this.Tag := t;
  end constructor;
  public method Put(s: string)
    this.O := s;
  end Put;
end Cell;
public class Program
  public static method Main(): int
    var i := 0;
    var total := 0;
    while i < 100 do
      var c := new Cell<int>(i);
      do
        var x := Make.Str(i);
        c.Put(x);
        c.O := x as Object;
      end;
      total := total + c.O.ToString().Length;
      i := i + 1;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
)BZ", "290\n");
}

// p06: +1 producers, literals and a self-derived value overwrite the field.
BEZEL_TEST(object_string_store_retain_8214, control_p06_producer_overwrites) {
    expectNoUseAfterFree(R"BZ(
module M;
public class Holder
  public O: Object := "";
end Holder;
public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;
public class Program
  public static method Main(): int
    var i := 0;
    var total := 0;
    while i < 100 do
      var h := new Holder();
      h.O := Make.Str(i);
      total := total + h.O.ToString().Length;
      h.O := Make.Str(i + 1) as Object;
      total := total + h.O.ToString().Length;
      h.O := "lit" + i.ToString();
      h.O := "plain";
      h.O := h.O.ToString() + "!";
      total := total + h.O.ToString().Length;
      i := i + 1;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
)BZ", "1182\n");
}

// p08: the field read back through `as string` and `is string`.
BEZEL_TEST(object_string_store_retain_8214, control_p08_as_string_readback) {
    expectNoUseAfterFree(R"BZ(
module M;
public class Holder
  public O: Object := "";
end Holder;
public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;
public class Program
  public static method Main(): int
    var i := 0;
    var total := 0;
    while i < 100 do
      var h := new Holder();
      do
        var x := Make.Str(i);
        h.O := x;
      end;
      var t := h.O as string;
      if t is not null then
        total := total + t.Length;
      end;
      if h.O is string then
        total := total + 1;
      end;
      i := i + 1;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
)BZ", "390\n");
}

// p10: a conditional expression over two string locals.
BEZEL_TEST(object_string_store_retain_8214, control_p10_conditional_of_strings) {
    expectNoUseAfterFree(R"BZ(
module M;
public class Holder
  public O: Object := "";
end Holder;
public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;
public class Program
  public static method Main(): int
    var i := 0;
    var total := 0;
    while i < 100 do
      var h := new Holder();
      do
        var s := Make.Str(i);
        var t := Make.Str(i + 1);
        h.O := (if i mod 2 = 0 then s else t);
      end;
      var junk := Make.Str(5);
      total := total + h.O.ToString().Length + junk.Length - junk.Length;
      i := i + 1;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
)BZ", "292\n");
}

// p11: a `string?` source into an `Object?` field, null every third time.
BEZEL_TEST(object_string_store_retain_8214, control_p11_nullable_string_source) {
    expectNoUseAfterFree(R"BZ(
module M;
public class Holder
  public O: Object? := null;
end Holder;
public class Make
  public static method Maybe(n: int): string? => (if n mod 3 = 0 then null else "m" + n.ToString());
end Make;
public class Program
  public static method Main(): int
    var i := 0;
    var total := 0;
    while i < 100 do
      var h := new Holder();
      do
        var m := Make.Maybe(i);
        h.O := m;
      end;
      if h.O is not null then
        total := total + h.O!.ToString().Length;
      end;
      i := i + 1;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
)BZ", "192\n");
}

// p14: the field stored from a constructor parameter and from a method.
BEZEL_TEST(object_string_store_retain_8214, control_p14_constructor_store) {
    expectNoUseAfterFree(R"BZ(
module M;
public class Holder
  public O: Object;
  public constructor(s: string)
    this.O := s;
  end constructor;
  public method Reset(n: int)
    this.O := "r" + n.ToString();
  end Reset;
end Holder;
public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;
public class Program
  public static method Main(): int
    var i := 0;
    var total := 0;
    while i < 100 do
      var h: Holder? := null;
      do
        var x := Make.Str(i);
        h := new Holder(x);
      end;
      var junk := Make.Str(5);
      if h is not null then
        total := total + h.O.ToString().Length + junk.Length - junk.Length;
        h.Reset(i);
        total := total + h.O.ToString().Length;
      end;
      i := i + 1;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
)BZ", "580\n");
}

// p15: a block-lambda body stores a captured string into the field.
BEZEL_TEST(object_string_store_retain_8214, control_p15_lambda_string_store) {
    expectNoUseAfterFree(R"BZ(
module M;
public class Holder
  public O: Object := "";
end Holder;
public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;
public class Program
  public static method Main(): int
    var i := 0;
    var total := 0;
    while i < 100 do
      var h := new Holder();
      h.O := Make.Str(i);
      do
        var x := Make.Str(i + 1);
        var f := lambda() => do h.O := x; end;
        f();
      end;
      var junk := Make.Str(5);
      total := total + h.O.ToString().Length + junk.Length - junk.Length;
      i := i + 1;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
)BZ", "292\n");
}

// p16: an `Object`-typed AUTO-PROPERTY.
BEZEL_TEST(object_string_store_retain_8214, control_p16_auto_property) {
    expectNoUseAfterFree(R"BZ(
module M;
public class Foo
  public V: int := 7;
end Foo;
public class Holder
  public property A: Object := "";
end Holder;
public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;
public class Program
  public static method Main(): int
    var i := 0;
    var total := 0;
    while i < 100 do
      var h := new Holder();
      do
        var x := Make.Str(i);
        h.A := x as Object;
      end;
      var junk := Make.Str(5);
      total := total + h.A.ToString().Length + junk.Length - junk.Length;
      i := i + 1;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
)BZ", "290\n");
}

// ===========================================================================
// SHAPES THE FIRST VERSION LEFT OPEN, closed by retaining without a
// classification. Both crash on r30-c1 and on c9f52c5a.
// ===========================================================================

// A MIXED field: a method stores a class instance, then `x as Object` stores
// a string from a `do` block. The first version's pre-pass excluded the field
// (a class store), so the string store kept aliasing `x` and the block's
// scope-end release freed it (heap-use-after-free).
BEZEL_TEST(object_string_store_retain_8214,
           mixed_field_explicit_as_object_store_retains_the_string) {
    expectNoUseAfterFree(R"BZ(
module M;
public class Foo
  public V: int := 7;
end Foo;
public class Holder
  public O: Object := "";
  public method SetF(f: Foo)
    this.O := f;
  end SetF;
end Holder;
public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;
public class Program
  public static method Main(): int
    var i := 0;
    var total := 0;
    var keep := new Foo();
    while i < 100 do
      var h := new Holder();
      h.SetF(keep);
      do
        var x := Make.Str(i);
        h.O := x as Object;
      end;
      var junk := Make.Str(99999);
      total := total + h.O.ToString().Length + junk.Length - junk.Length;
      i := i + 1;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
)BZ", "290\n");
}

// `this.O ??= s` with a `string` parameter (§6.6). The `??=` arm stored the
// caller's borrow with no retain, and the caller's scope-end release freed it
// under the field (rev-8214's p18).
BEZEL_TEST(object_string_store_retain_8214,
           null_coalescing_assign_string_store_retains_the_string) {
    expectNoUseAfterFree(R"BZ(
module M;
public class Holder
  public O: Object? := null;
  public method SetS(s: string)
    this.O := s;
  end SetS;
  public method Fill(s: string)
    this.O ??= s;
  end Fill;
end Holder;
public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;
public class Program
  public static method Main(): int
    var i := 0;
    var total := 0;
    while i < 100 do
      var h := new Holder();
      do
        var x := Make.Str(i);
        h.Fill(x);
      end;
      var junk := Make.Str(5);
      total := total + h.O!.ToString().Length + junk.Length - junk.Length;
      h.SetS(junk);
      i := i + 1;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
)BZ", "290\n");
}

// `h.O ??= x` and `h.O ??= x as Object` from a `do`-block local. The
// string-escape walk's #8214 arm reads the RHS of ANY assignment operator into
// such a field as a safe read, so `x`'s scope-end release stays armed; the
// first version's `??=` arm did not retain, so that release freed the string
// under the field (heap-use-after-free on c9f52c5a; r30-c1 leaked instead).
BEZEL_TEST(object_string_store_retain_8214,
           null_coalescing_assign_from_block_local_retains_the_string) {
    expectNoUseAfterFree(R"BZ(
module M;
public class Holder
  public O: Object? := null;
end Holder;
public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;
public class Program
  public static method Main(): int
    var i := 0;
    var total := 0;
    while i < 100 do
      var h := new Holder();
      do
        var x := Make.Str(i);
        h.O ??= x;
        h.O ??= x as Object;
      end;
      var junk := Make.Str(5);
      total := total + h.O!.ToString().Length + junk.Length - junk.Length;
      i := i + 1;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
)BZ", "290\n");
}
