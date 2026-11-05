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
// THE FIX. A class field whose DECLARED type is `Object`/interface and whose
// EVERY store carries a string-typed source is an "object string storage
// field" (the string twin of the box storage family): the store RETAINS the
// value into the field (the field takes its own independent +1) and releases
// the displaced value, and the owner's disposal releases the field's +1. The
// string-escape walk reads such a store's right-hand side as a SAFE READ (a
// borrow, not an escape), so the SOURCE keeps its own +1 and still releases it
// at its scope end — the two halves balance.
//
// WHY A PRE-PASS AND NOT A RUNTIME TAG. An `Object`-typed slot may hold a
// class instance, a box, a function value or a string, and the static type
// cannot tell them apart. The retain half is decidable per STORE (the
// source's static type IS a string); the release half needs "is the slot's
// CURRENT value a string?", which is a RUNTIME question — issue #8215 / #5059
// ("no runtime tag distinguishes string from other objects behind `Object?`"),
// and the runtime's `bz_obj_is_string` is only sound in the SKIP direction (a
// cross-module class instance answers "string"), so it cannot gate a release.
// The per-field pre-pass answers it STATICALLY: a field whose every store is a
// string can only ever hold a string. A MIXED field is EXCLUDED, so it keeps
// the pre-fix plain-store behaviour.
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
// Both are the SAME defect at slots where the per-field static
// classification is unavailable; closing them is the discriminator work this
// fix deliberately does not attempt (it would trade a use-after-free for an
// unbounded leak). The rows below cover the CLASS-FIELD shape, which is the
// one issue #8214 is filed against, plus the controls that must not move.
//
// EVERY ROW GOES THROUGH `runAotChecked` — the helper that compiles the
// program NATIVELY with the generated-code AddressSanitizer AND arms
// LeakSanitizer BY CONSTRUCTION (`detectLeaks=true` is a hardcoded argument
// in aot_support.hpp). The leak rows therefore rest on that argument and NOT
// on an environment variable: `runAotCheckedLenient` passes `false` and runs
// `detect_leaks=0`, so a leak assertion written against THAT helper would be
// VACUOUS. `BEZEL_AOT_DETECT_LEAKS` is read only by `runAot`, never by the
// `runAotChecked` family, so it is not what arms these rows.
//
// Every row asserts `!checkedToolchainSkipped(r)` BEFORE any guard that could
// absorb the skip: a box without clang fails the row loudly rather than
// passing vacuously.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

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

// The class-field shape, parameterised by the store statement.
//
// The holder is created ONCE and the store runs kIterations times inside a
// `for` loop, so the field is OVERWRITTEN kIterations times: each overwrite
// releases the displaced value, which is what makes a missing/extra release
// show up as a leak (or an over-release) rather than hiding.
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
// Post-fix: the program prints `s12345` / `6` per iteration and is clean
// under ASan AND LSan (the field's reference is released by the owner's
// disposal; the source's by its own scope end).
// ===========================================================================
BEZEL_TEST(object_string_store_retain_8214,
           issue_repro_y_boxhole_oneshot_looped_100x_is_clean) {
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
    expectCleanRun(program, repeatLine("s12345\n6\n", kIterations));
}

// ===========================================================================
// THE BUG. The issue's own shape: `h.O := x as Object;` inside a `do ... end;`
// block, then the field is read AFTER the block ended. Pre-fix: the block's
// scope-end release freed the string the field pointed at — `free(): invalid
// pointer` (rc 134) plain, a heap-use-after-free under ASan.
//
// The row is RED pre-fix and GREEN post-fix, and it is leak-free post-fix
// (the field's +1 is released by the owner's disposal; the source's +1 by its
// own scope end) — so it pins the WHOLE balance, not just the absence of the
// crash.
// ===========================================================================
BEZEL_TEST(object_string_store_retain_8214,
           object_field_explicit_as_object_store_retains_the_string) {
    expectCleanRun(fieldProgram("h.O := x as Object;"),
                   repeatLine("s12345\n", kIterations));
}

// ===========================================================================
// THE IMPLICIT TWIN. The same store WITHOUT the `as`: `h.O := x;` (§5.1's
// implicit string → Object conversion). Pre-fix this shape did not crash — the
// escape walk read the implicit store as an escape and suppressed `x`'s
// scope-end release — but it LEAKED the reference (39 bytes / 2 allocations).
// The row pins that the implicit conversion behaves EXACTLY like the explicit
// one: same value printed, and no leak either.
//
// The two rows together are what make "the implicit conversion must behave
// the same as the explicit one" a MEASURED claim rather than an assertion.
// ===========================================================================
BEZEL_TEST(object_string_store_retain_8214,
           object_field_implicit_conversion_twin_retains_the_string) {
    expectCleanRun(fieldProgram("h.O := x;"),
                   repeatLine("s12345\n", kIterations));
}

// ===========================================================================
// THE STRING LITERAL — "must not be over-released". A literal is a SENTINEL
// reference (the runtime's literal rc value): bz_string_retain /
// bz_string_release are no-ops on it, so the store's retain/release pair must
// not free it and the program must not abort with an over-release.
//
// The literal is stored kIterations times into the same field, so an
// over-release would fire on the SECOND store (the first one released the
// field's initial literal) and an under-release would leak.
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
    expectCleanRun(program, repeatLine("s12345\n", kIterations));
}

// ===========================================================================
// CONTROL — AN INT BOX. A value type stored into an `Object`-typed field is
// BOXED (§8.6) and the box is runtime-managed. The box family owns that
// reference (bz_box_release at the owner's disposal), so the row must stay
// clean AND must not be double-released by the string family.
//
// The holder here is a SEPARATE class whose `Object` field's initializer is
// an INT (a box), not a string: a field that receives BOTH a box and a string
// is a MIXED field, which the pre-pass excludes from both families (a mixed
// field keeps the pre-fix plain-store behaviour) — so the control uses a
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
