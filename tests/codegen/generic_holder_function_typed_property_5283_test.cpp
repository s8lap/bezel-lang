// fix #5283 (§14.1 "Generic types" / §27.6 "Method references" / §23.1,
// v0.313): a `T`-typed property of a generic class, read at an instantiation
// whose SUBSTITUTED type for `T` is a function type, took two codegen paths
// that both contradicted the substitution.
//
// THE DEFECTS (both measured on a build of r23-c28 @ ccd0be14).
//
//  1. `makePropertyThunk` (src/codegen/LlvmBackend_Layout.cpp) computes the
//     thunk's result type `retTy = lowerType(pd->type.get())` — and builds
//     `thunkFnTy` from it — roughly 130 lines BEFORE it installs the receiver's
//     §14.1 generic substitution (`genericSubst_ = {T -> lowerTypeRef(arg)}`).
//     At `retTy` time `genericSubst_` was still the CALLER's (empty in a
//     non-generic `Main`), so `lowerType` on the property's declared bare `T`
//     missed `lowerTypeRef`'s substitution lookup and fell through to the
//     bare-parameter placeholder `i32()`. The BODY, emitted later with the
//     substitution correctly installed, then loaded the real
//     `class.Holder$spec0` field — a `ptr` at `T = () -> int`, since a function
//     value is a closure-cell pointer (§27.6) — and emitted `ret ptr %3` from a
//     function declared `i32 (ptr)`. LLVM's verifier rejects that outright:
//     "Function return type does not match operand type of return inst!",
//     `bezelc: codegen error: IR verification failed` — the program never
//     compiled. Read off `BEZEL_DUMP_IR=1`:
//
//         define internal i32 @"pthunk$0"(ptr %0) {
//         entry:
//           %1 = load ptr, ptr %0, align 8
//           %2 = getelementptr inbounds nuw %"class.Holder$spec0", ptr %1, i32 0, i32 2
//           %3 = load ptr, ptr %2, align 8
//           ret ptr %3          ; <-- ptr out of an i32 (ptr)
//         }
//
//     and, after the fix, the same function correctly typed:
//
//         define internal ptr @"pthunk$0"(ptr %0) { ... ret ptr %3 }
//
//  2. The §27.6 property-reference gate in `lowerMemberAccess`
//     (src/codegen/LlvmBackend_Exprs.cpp, the #756 promotion guarded by
//     #1187) discriminates on the property's DECLARED type: a property whose
//     declared type is ITSELF a function type is an ordinary VALUE read (the
//     getter already yields the function value), so the getter-as-closure thunk
//     must be skipped. A bare `T` declared type is not `ast::TypeKind::Function`,
//     so at `Holder<() -> int>` the gate built a `() -> (() -> int)` getter
//     closure where a `(…) -> T` slot was being bound; the subsequent call then
//     ran that closure and returned a closure POINTER read as an `int`
//     (measured: exit 32, where the identical NON-generic program returns 7).
//     This one only becomes reachable once defect 1 stops the build.
//
//  Defect 2's fix resolves the declared type through the receiver's own type
//  arguments at this instantiation (§14.1 monomorphization — the same
//  both-sources walk `makePropertyThunk`'s §14.1 block performs), so a
//  substitution that is itself a function type takes the ordinary read path,
//  exactly as the non-generic spelling does.
//
//  Neither defect is specific to a function-typed substitution: defect 1
//  reached ANY `T` whose LLVM type is not `i32` whenever such a property was
//  referenced in function-value position. The `T = string` row below is that
//  witness, and it is the CLEANEST pin in this file: no function types are
//  involved anywhere in that program, so it is free of the §28.14 storage
//  question the repro rows sit on (see below), and it still fails to compile
//  pre-fix. `T = int` is the one shape defect 1 could NOT reach — `int` lowers
//  to `i32` and the placeholder coincidentally matched — kept as a control
//  showing the fix did not over-reach into the ordinary getter-as-closure path.
//
//  §28.14 AND THESE REPRO ROWS. The issue's own programs store a plain
//  (non-`scoped`) function-typed constructor parameter into an auto-property's
//  backing field. §28.14 (v0.177) makes that parameter **call-bounded** — "a
//  plain function-typed parameter may be invoked, bound to locals, and passed
//  to another plain function-typed parameter, but may not be stored in a field
//  of any object" — and MEASURED: the DIRECT-field twin IS rejected today
//  (`public _f: () -> int; _f := x;` → "parameter `x` is call-bounded;
//  declare it `scoped` to store it (§28.14)"), while the property spelling is
//  NOT, and the accepted program then leaks its 48-byte closure cell (also
//  reproduced with a fully NON-generic `Holder`, so that leak is a separate,
//  generic-independent defect, filed separately — it is not this lane's and it
//  is not caused by this fix). `scoped` is not an escape hatch on these
//  programs: `public constructor(scoped x: T)` is rejected in turn ("`scoped`
//  is valid only on a function-typed, class-typed, or interface-typed
//  parameter … `x` does not have any of those types" — `T`'s substituted type
//  is not visible at the declaration), and reading a `scoped` parameter back
//  into the property is itself rejected.
//
//  So the three repro rows below run on plain `runAot` rather than
//  `runAotChecked`: they pin the compiler's observable behaviour on the exact
//  programs the issue reported, while the ASan leg is reserved for the rows
//  that are free of that pre-existing leak (`T = string`, `T = int`), both of
//  which are ASan-clean (measured: no `LeakSanitizer` report). If the §28.14
//  storage ban is later enforced through a property setter — the correct end
//  state, filed as a follow-up — these three rows become compile-time
//  REJECTIONS and must flip at that time; that flip is recorded here so it is
//  not discovered as a surprise.
//
//  FAIL-WITHOUT / PASS-WITH: rows 1–4 do not merely change a value — they do
//  not COMPILE without the fix (the verifier rejects the module), so `r.ran`
//  is false pre-fix and the expected result is observed post-fix. The `int`
//  row and the construct-only row pass both before and after, and are
//  labelled as controls, not fail-without rows. Measured mechanically via the
//  scoped `git apply -R` revert-src + rebuild + re-run cycle (see the commit
//  body); every row was re-measured individually.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The issue's own minimal repro (issue #5283's `q/b7.bzl`), verbatim:
// `var g: () -> int := h.v;` on a `Holder<() -> int>`, expecting exit 7.
constexpr const char* kB7 =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public constructor(x: T)\n"
    "    v := x;\n"
    "  end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var f: () -> int := lambda () => 7;\n"
    "    var h := new Holder<() -> int>(f);\n"
    "    var g: () -> int := h.v;\n"
    "    return g();\n"
    "  end Main;\n"
    "end Program;\n";

// The same shape with the CALL spelled at the property (`q/b2.bzl`): the
// read is still an ordinary value read, so `h.v()` invokes the stored lambda.
constexpr const char* kB2 =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public constructor(x: T)\n"
    "    v := x;\n"
    "  end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var f: () -> int := lambda () => 7;\n"
    "    var h := new Holder<() -> int>(f);\n"
    "    var t := h.v();\n"
    "    return t;\n"
    "  end Main;\n"
    "end Program;\n";

// The reviewer's loop form (`q/b3.bzl`): the value-position read inside an
// expression, which is the shape the thunk wrongly captured.
constexpr const char* kB3 =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public constructor(x: T)\n"
    "    v := x;\n"
    "  end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    var f: () -> int := lambda () => 7;\n"
    "    var h := new Holder<() -> int>(f);\n"
    "    total := total + h.v();\n"
    "    return total;\n"
    "  end Main;\n"
    "end Program;\n";

// Construct-only (`q/b6.bzl`) — the issue records this as ALREADY working
// pre-fix (the fault is in the read, not the construction). Kept as a
// control row: it must keep compiling and keep returning 0, so a fix that
// changed the CONSTRUCTION path would be caught here.
constexpr const char* kB6 =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public constructor(x: T)\n"
    "    v := x;\n"
    "  end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var f: () -> int := lambda () => 7;\n"
    "    var h := new Holder<() -> int>(f);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The CLEAN fail-without pin for defect 1: a REFERENCE-type substitution
// (`T = string`) with NO function type anywhere in the program, so this row
// carries none of the §28.14 storage question the repro rows sit on. The
// property reference itself is a legitimate §27.6 getter-as-closure (`string`
// is not a function type, so defect 2's carve-out correctly does NOT fire) —
// but the field is a `ptr` and the pre-fix thunk signature was the `i32`
// placeholder, so pre-fix this program does not compile either.
std::string narrowedShape(const std::string& tArg, const std::string& fnTy,
                          const std::string& lit) {
    return
        "module M;\n"
        "class Holder<T>\n"
        "  public property v: T;\n"
        "  public constructor(x: T)\n"
        "    v := x;\n"
        "  end constructor;\n"
        "end Holder;\n"
        "class Program\n"
        "  public static method Main(): int\n"
        "    var h := new Holder<" + tArg + ">(" + lit + ");\n"
        "    var g: " + fnTy + " := h.v;\n"
        "    var n := g();\n"
        "    Console.WriteLine(n);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
}

}  // namespace

// ─── The issue's own repro ────────────────────────────────────────────────
// Pre-fix: `bezelc` aborts with "IR verification failed (malformed IR
// emitted)" — the program does not build, so `r.ran` is false. Post-fix: exit 7.
BEZEL_TEST(generic_holder_function_typed_property_5283,
           issue_repro_property_read_as_function_value) {
    JitResult r = runAot(kB7);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// The same read with the call at the property (`q/b2.bzl`). Same oracle.
BEZEL_TEST(generic_holder_function_typed_property_5283,
           property_read_called_in_place) {
    JitResult r = runAot(kB2);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// The reviewer's `total + h.v()` form (`q/b3.bzl`) — the value-position read
// inside an expression, where the wrong `() -> (() -> int)` closure would have
// been the operand. Same oracle.
BEZEL_TEST(generic_holder_function_typed_property_5283,
           property_read_in_arithmetic_expression) {
    JitResult r = runAot(kB3);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// Construct-only control (`q/b6.bzl`): the issue records this as working
// pre-fix, so it is NOT a fail-without row. It exists to catch a fix that
// disturbed the CONSTRUCTION path for a function-typed substitution.
BEZEL_TEST(generic_holder_function_typed_property_5283,
           construct_only_control_does_not_regress) {
    JitResult r = runAot(kB6);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Defect 1's cleanest pin, and the row that would still fail to compile if the
// §28.14 storage ban on the repro rows above is later enforced: a `T`-typed
// property read in function-value position at a REFERENCE-type substitution.
// No function type appears anywhere in this program, so it involves no storage
// of a callback at all. ASan-clean (measured: no `LeakSanitizer` report).
BEZEL_TEST(generic_holder_function_typed_property_5283,
           reference_type_substitution_property_reference) {
    JitResult r = runAotChecked(narrowedShape("string", "() -> string", "\"s\""));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("s\n"));
}

// Control (NOT a fail-without row): the generic property read in function-value
// position at `T = int`, which lowers to `i32` — the SAME LLVM type as the
// placeholder defect 1 wrongly used, so the pre-fix thunk signature happened to
// match its body and this row passes BOTH pre- and post-fix. It exists to show
// the hoisted §14.1 substitution did not disturb the ordinary lowered shape:
// the getter-as-closure of §27.6 is still built (defect 2's carve-out
// correctly does NOT fire, because `int` is not a function type) and calling
// it yields the stored 7. ASan-clean (measured: no `LeakSanitizer` report).
BEZEL_TEST(generic_holder_function_typed_property_5283,
           generic_holder_int_substitution_control) {
    JitResult r = runAotChecked(narrowedShape("int", "() -> int", "7"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// Control (NOT a fail-without row), and the one that most directly guards the
// blast radius of the #5283 fix: the SAME `Holder<T>` generic read in
// function-value position at TWO instantiations in one program. `makePropertyThunk`
// memoises its thunk per `sema::PropertySymbol*` — NOT per instantiation
// (`propertyThunks_`; see its own comment there), so a program that instantiates
// one generic property at two different type arguments is exactly the shape where
// a substitution-sensitive signature could be baked from the first emission and
// reused by the second. Both orders are exercised: function-typed first, then
// `string`; and the reverse pairing below.
//
// MEASURED: this holds because defect 2's carve-out means a function-typed
// substitution never reaches the thunk at all (it takes the ordinary value-read
// path), so the only thunk ever cached is the `string` one — consistent in both
// orderings. If a future change routes a function-typed substitution back
// through the thunk, this row is what will catch the shared-signature bug.
BEZEL_TEST(generic_holder_function_typed_property_5283,
           two_instantiations_of_one_generic_property) {
    // Function-typed instantiation FIRST, then a reference-type one.
    JitResult r = runAot(
        "module M;\n"
        "class Holder<T>\n"
        "  public property v: T;\n"
        "  public constructor(x: T)\n"
        "    v := x;\n"
        "  end constructor;\n"
        "end Holder;\n"
        "class Program\n"
        "  public static method Main(): int\n"
        "    var hf := new Holder<() -> int>(lambda () => 7);\n"
        "    var gf: () -> int := hf.v;\n"
        "    var b := gf();\n"
        "    var hs := new Holder<string>(\"z\");\n"
        "    var gs: () -> string := hs.v;\n"
        "    var c := gs();\n"
        "    Console.WriteLine(b.ToString());\n"
        "    Console.WriteLine(c);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\nz\n"));

    // The reverse pairing: a value-type instantiation (which DOES take the
    // thunk path) alongside the function-typed one.
    JitResult r2 = runAot(
        "module M;\n"
        "class Holder<T>\n"
        "  public property v: T;\n"
        "  public constructor(x: T)\n"
        "    v := x;\n"
        "  end constructor;\n"
        "end Holder;\n"
        "class Program\n"
        "  public static method Main(): int\n"
        "    var hi := new Holder<int>(3);\n"
        "    var gi: () -> int := hi.v;\n"
        "    var a := gi();\n"
        "    var hf := new Holder<() -> int>(lambda () => 7);\n"
        "    var gf: () -> int := hf.v;\n"
        "    var b := gf();\n"
        "    Console.WriteLine(a.ToString());\n"
        "    Console.WriteLine(b.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r2.ran);
    BEZEL_EXPECT_EQ(r2.exitCode, 0);
    BEZEL_EXPECT_EQ(r2.stdoutText, std::string("3\n7\n"));
}
