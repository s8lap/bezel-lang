// Regression tests for #8976: the `?.` read of a FUNCTION-TYPED BLOCK getter
// on a GENERIC class instantiated at a REFERENCE type jumped to a wild address
// (a SEGV in `.bz.str.data`).
//
// Spec (v0.359):
//  - §14.1: "an **instantiation** of a generic declaration is the constructed
//    type or constructed member obtained by substituting type arguments for
//    its type parameters … `var b: Box<string>;` forms `Box<string>` exactly
//    as `new Box<string>(…)` does, and the nullable form `G<A>?` forms
//    `G<A>`." So the receiver of a `?.` read on a `Box<string>?` is an
//    instantiation of `Box<string>`, and the getter that read calls must be
//    `Box<string>`'s getter, with `Box<string>`'s layout.
//  - §23.3: a block-form property's accessor body is `get => do … end;`.
//  - §7.5: "`x?.M` returns null if `x` is null; otherwise it performs the
//    access. The result type is always nullable."
//  - §28.9: "a getter never transfers a reclamation obligation to its reader"
//    — so the `?.` spelling (whose always-nullable result has no owner for a
//    closure reference) reads a BORROW, and its getter is the separate
//    `$get$lend` variant #8763 introduced.
//
// THE DEFECT. The `?.` read of a block-bodied property (the
// `NullCondMemberExpr` arm of `lowerExpr`, LlvmBackend_Exprs.cpp) called
// `ensurePropertyBlockGetter` with `ps = nullptr` and WITHOUT installing the
// receiver's generic substitution. Two consequences, both measured:
//
//  (1) The getter is emitted OUT OF LINE, and its body's `this` is typed by
//      `Emitter::classStruct`, which is substitution-SENSITIVE: with no live
//      substitution it builds the ERASED layout (`%class.Box` =
//      { ptr, i8, i32, ptr } — the `T` placeholder), and with the
//      receiver's substitution the SPECIALIZED one (`%"class.Box$spec0"` =
//      { ptr, i8, ptr, ptr } for `T = string`). Under the erased layout the
//      getter's GEP for `f: () -> int` lands at byte offset 16 — which in the
//      specialized layout is `v: string`, NOT `f` — so the getter handed the
//      reader the STRING as the closure value and the program called it.
//      MEASURED on r30-c1 (f79705960) with `bezelc --sanitize=address` on
//      #8976's literal repro: "SUMMARY: AddressSanitizer: SEGV bezel in
//      .bz.str.data.58".
//      `Box<int>` was unaffected only BY COINCIDENCE: `T = int` lowers to
//      `i32`, so the erased layout { ptr, i8, i32, ptr } IS `Box<int>`'s
//      layout; any reference-typed `T` widens the slot to `ptr` and shifts
//      `f` by four bytes.
//  (2) `ensurePropertyBlockGetter` keys its memo on the owner's
//      instantiation ONLY when `ps` is non-null (`propertyThunkSubstKey`),
//      so the `?.` getter was memoized in the ERASED key space: two
//      instantiations of one generic class shared a single getter body.
//
// The `.` twin of the same read (the `MemberAccessExpr` arm) installs the
// receiver's substitution (`setupReceiverGenericSubst`) and passes `ps`, so it
// was already correct — the defect is specific to the `?.` spelling.
//
// Rows 1-4 are the issue's own shapes; row 5 loops the `?.` read 200x so a
// one-shot crash cannot be mistaken for a stable verdict.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// #8976's LITERAL repro, verbatim.
constexpr const char* k_literal_repro =
    "module M;\n"
    "public class Box<T>\n"
    "  public v: T;\n"
    "  private f: () -> int;\n"
    "  public constructor(a: T) v := a; f := lambda(): int => 9; end;\n"
    "  public property P: () -> int\n"
    "    get => do return f; end;\n"
    "  end;\n"
    "end Box;\n"
    "public static class Program\n"
    "  public static method Main(args: Array<string>): int\n"
    "    using var bx := new Box<string>(\"s\");\n"
    "    var bn: Box<string>? := bx;\n"
    "    var y := bn?.P;\n"
    "    if y <> null then Console.Write(y!()); end;\n"
    "    return 0;\n"
    "  end;\n"
    "end Program;\n";

// The same shape with a CLASS-typed type argument (`T = Item`): the layout
// widens exactly as `T = string` does, so the same wild jump.
constexpr const char* k_class_type_arg =
    "module M;\n"
    "public class Item\n"
    "  public n: int;\n"
    "  public constructor(a: int) n := a; end;\n"
    "end Item;\n"
    "public class Box<T>\n"
    "  public v: T;\n"
    "  private f: () -> int;\n"
    "  public constructor(a: T) v := a; f := lambda(): int => 9; end;\n"
    "  public property P: () -> int\n"
    "    get => do return f; end;\n"
    "  end;\n"
    "end Box;\n"
    "public static class Program\n"
    "  public static method Main(args: Array<string>): int\n"
    "    using var bx := new Box<Item>(new Item(1));\n"
    "    var bn: Box<Item>? := bx;\n"
    "    var y := bn?.P;\n"
    "    if y <> null then Console.Write(y!()); end;\n"
    "    return 0;\n"
    "  end;\n"
    "end Program;\n";

// The `??` spelling (§7.7) of the same read: the left operand is the same
// `?.` read, so it is the same getter.
constexpr const char* k_coalesce_spelling =
    "module M;\n"
    "public class Box<T>\n"
    "  public v: T;\n"
    "  private f: () -> int;\n"
    "  public constructor(a: T) v := a; f := lambda(): int => 9; end;\n"
    "  public property P: () -> int\n"
    "    get => do return f; end;\n"
    "  end;\n"
    "end Box;\n"
    "public static class Program\n"
    "  public static method Main(args: Array<string>): int\n"
    "    using var bx := new Box<string>(\"s\");\n"
    "    var bn: Box<string>? := bx;\n"
    "    var y := bn?.P ?? (lambda(): int => 0);\n"
    "    Console.Write(y());\n"
    "    return 0;\n"
    "  end;\n"
    "end Program;\n";

// THREE instantiations of one generic class in one program: each `?.` read
// must reach ITS OWN instantiation's getter (the erased-key-space sharing of
// consequence (2) above). `Box<int>` is included so the value-typed layout is
// in the same program as the two reference-typed ones.
constexpr const char* k_three_instantiations =
    "module M;\n"
    "public class Item\n"
    "  public n: int;\n"
    "  public constructor(a: int) n := a; end;\n"
    "end Item;\n"
    "public class Box<T>\n"
    "  public v: T;\n"
    "  private f: () -> int;\n"
    "  public constructor(a: T) v := a; f := lambda(): int => 9; end;\n"
    "  public property P: () -> int\n"
    "    get => do return f; end;\n"
    "  end;\n"
    "end Box;\n"
    "public static class Program\n"
    "  public static method Main(args: Array<string>): int\n"
    "    using var bs := new Box<string>(\"s\");\n"
    "    using var bi := new Box<int>(3);\n"
    "    using var bp := new Box<Item>(new Item(1));\n"
    "    var bns: Box<string>? := bs;\n"
    "    var bni: Box<int>? := bi;\n"
    "    var bnp: Box<Item>? := bp;\n"
    "    var ys := bns?.P;\n"
    "    var yi := bni?.P;\n"
    "    var yp := bnp?.P;\n"
    "    var total: int := 0;\n"
    "    if ys <> null then total := total + ys!(); end;\n"
    "    if yi <> null then total := total + yi!(); end;\n"
    "    if yp <> null then total := total + yp!(); end;\n"
    "    Console.Write(total);\n"
    "    return 0;\n"
    "  end;\n"
    "end Program;\n";

// 200 reads of the `?.` spelling, so the row is not a one-shot crash.
constexpr const char* k_loop_200x =
    "module M;\n"
    "public class Box<T>\n"
    "  public v: T;\n"
    "  private f: () -> int;\n"
    "  public constructor(a: T) v := a; f := lambda(): int => 9; end;\n"
    "  public property P: () -> int\n"
    "    get => do return f; end;\n"
    "  end;\n"
    "end Box;\n"
    "public static class Program\n"
    "  public static method Main(args: Array<string>): int\n"
    "    var total: int := 0;\n"
    "    var i: int := 0;\n"
    "    while i < 200 do\n"
    "      using var bx := new Box<string>(\"s\");\n"
    "      var bn: Box<string>? := bx;\n"
    "      var y := bn?.P;\n"
    "      if y <> null then total := total + y!(); end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.Write(total);\n"
    "    return 0;\n"
    "  end;\n"
    "end Program;\n";

// An INHERITED generic accessor: the receiver is a NON-generic derived class
// (`Derived : Base<string>`), so the substitution has to be found by walking
// the base chain (`setupReceiverGenericSubst`'s ancestor path) — the direct
// receiver's bindings are not enough. FAIL-WITHOUT: the compiler ABORTS with
// "codegen error: IR verification failed (malformed IR emitted)" — the getter
// was emitted against the erased `%class.Base` = { ptr, i8, i32 } layout, so
// the `string` field was read as an `i32`.
constexpr const char* k_inherited_generic_accessor =
    "module M;\n"
    "public class Base<T>\n"
    "  public v: T;\n"
    "  public constructor(a: T) v := a; end;\n"
    "  public property P: T\n"
    "    get => do return v; end;\n"
    "  end;\n"
    "end Base;\n"
    "public class Derived : Base<string>\n"
    "  public constructor() base(\"zz\"); end;\n"
    "end Derived;\n"
    "public static class Program\n"
    "  public static method Main(args: Array<string>): int\n"
    "    using var d := new Derived();\n"
    "    var dn: Derived? := d;\n"
    "    var y := dn?.P;\n"
    "    if y <> null then Console.Write(y); end;\n"
    "    return 0;\n"
    "  end;\n"
    "end Program;\n";

}  // namespace

// The issue's literal repro. FAIL-WITHOUT: the program dies with a wild jump
// ("SUMMARY: AddressSanitizer: SEGV bezel in .bz.str.data.58") — `ran` is
// false and the exit code is -1 on both oracles.
BEZEL_TEST(property_block_getter_null_cond_generic_8976,
           null_cond_read_of_generic_block_getter_string_type_arg) {
    JitResult r = runAot(k_literal_repro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9"));
    JitResult c = runAotChecked(k_literal_repro);
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, 0);
    BEZEL_EXPECT_EQ(c.stdoutText, std::string("9"));
}

// The class-typed type argument: `T = Item` widens the erased layout's slot
// exactly as `T = string` does. FAIL-WITHOUT: the same wild jump (the SEGV
// lands in the `Item` vtable's data).
BEZEL_TEST(property_block_getter_null_cond_generic_8976,
           null_cond_read_of_generic_block_getter_class_type_arg) {
    JitResult r = runAot(k_class_type_arg);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9"));
    JitResult c = runAotChecked(k_class_type_arg);
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, 0);
    BEZEL_EXPECT_EQ(c.stdoutText, std::string("9"));
}

// The `??` spelling (§7.7) reads the same `?.` value, so it reaches the same
// getter. FAIL-WITHOUT: the same wild jump.
BEZEL_TEST(property_block_getter_null_cond_generic_8976,
           null_cond_coalesce_of_generic_block_getter) {
    JitResult r = runAot(k_coalesce_spelling);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9"));
}

// Three instantiations in one program: each `?.` read must reach its OWN
// instantiation's getter. FAIL-WITHOUT: the `Box<string>` read dies with the
// wild jump (all three reads shared one erased-layout getter).
BEZEL_TEST(property_block_getter_null_cond_generic_8976,
           null_cond_reads_of_three_instantiations_use_their_own_getters) {
    JitResult r = runAot(k_three_instantiations);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("27"));
}

// 200 reads, under the ASan/LSan net: the crash is not a one-shot, and the
// borrow variant's contract (§28.9: the `?.` reader discharges no reference)
// leaves no residue. FAIL-WITHOUT: the first read dies with the wild jump.
BEZEL_TEST(property_block_getter_null_cond_generic_8976,
           null_cond_read_of_generic_block_getter_200x) {
    JitResult c = runAotChecked(k_loop_200x);
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, 0);
    BEZEL_EXPECT_EQ(c.stdoutText, std::string("1800"));
}

// The INHERITED generic accessor: the receiver is `Derived : Base<string>`,
// so the receiver's own bindings do not name `Base`'s parameter — the
// substitution has to come from the base-chain walk. FAIL-WITHOUT: the
// compiler aborts with "IR verification failed (malformed IR emitted)".
// This row is what catches a wrong fix that handles only the DIRECT generic
// receiver (the `ncRecvTy->declaration == ncPropSym->parent` gate).
BEZEL_TEST(property_block_getter_null_cond_generic_8976,
           null_cond_read_of_inherited_generic_block_getter) {
    JitResult r = runAot(k_inherited_generic_accessor);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("zz"));
    JitResult c = runAotChecked(k_inherited_generic_accessor);
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, 0);
    BEZEL_EXPECT_EQ(c.stdoutText, std::string("zz"));
}
