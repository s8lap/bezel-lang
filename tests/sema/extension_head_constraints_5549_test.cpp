// fix #5549 / spec §26.4 and §14.2 *A type parameter as the type argument*
// (v0.338): a generic extension head names the extended type at the block's
// own parameters, and each parameter's constraints are the block's `where`
// clause and nothing else. `extension Box<T>` over `class Box<T> where T:
// new()` is a compile error naming `where T: new()`; restating the clause on
// the block (`extension Box<T> where T: new()`) is admitted and the extension
// runs. A block parameter inherits no constraint from the extended type, even
// one spelled the same way.
//
// Every reject row asserts the §14.2 needle (the constraint and the parameter)
// and that the program did not run. Every accept row asserts the program ran
// and printed the extension's result, so a silently-admitted head cannot pass.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasHeadDiagnostic(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos &&
            d.message.find("(§14.2)") != std::string::npos)
            return true;
    return false;
}

// Shared declarations: a constructible user class to instantiate with, and a
// generic `Box<T> where T: new()` whose extension heads the tests vary.
const std::string kWidget =
    "module M; "
    "class Widget "
    "  public constructor() end constructor; "
    "end Widget; ";

const std::string kBox =
    "class Box<T> where T: new() "
    "  public constructor() end constructor; "
    "end Box; ";

// Main calls the extension method on a receiver `Box<Widget>`.
const std::string kMainBox =
    "class C "
    "  public static method Main(): int "
    "    var b := new Box<Widget>(); "
    "    Console.WriteLine(b.P().ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// ---------------------------------------------------------------------------
// Reject rows: an unrestated (or differently restated) head over a constrained
// extended type.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_extension_head_constraints_5549,
           unrestated_head_over_new_constraint_rejected_sec_26_4) {
    // §26.4 (v0.338): `extension Box<T>` leaves T with no constraint; Box
    // requires `where T: new()` of its parameter, so the head is rejected.
    auto r = runAot(kWidget + kBox +
                    "extension Box<T> "
                    "  public method P(): int return 7; end P; "
                    "end; " + kMainBox);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(
        r, "type argument `T` does not satisfy the constraint `where T: new()`"));
}

BEZEL_TEST(sema_extension_head_constraints_5549,
           unrestated_head_over_stdlib_owning_array_rejected_sec_26_4) {
    // §26.4 (v0.338) with the issue's OwningArray example: the stdlib owning
    // collection requires `where T: class` of its element, which the block
    // must state.
    auto r = runAot(
        "module M; "
        "extension OwningArray<T> "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(
        r, "type argument `T` does not satisfy the constraint `where T: class`"));
}

BEZEL_TEST(sema_extension_head_constraints_5549,
           unrestated_head_over_hashable_constraint_rejected_sec_26_4) {
    // §26.4 (v0.338): `extension Counter<K>` must state `where K: hashable`.
    auto r = runAot(
        "module M; "
        "class Counter<K> where K: hashable "
        "  public constructor() end constructor; "
        "end Counter; "
        "extension Counter<K> "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(
        r, "type argument `K` does not satisfy the constraint `where K: hashable`"));
}

BEZEL_TEST(sema_extension_head_constraints_5549,
           concrete_looking_head_argument_is_a_parameter_rejected_sec_26_4) {
    // §26.4 (v0.338): `IShape` in an argument position is a fresh block
    // parameter, not the interface; it inherits nothing, so it does not
    // satisfy the constraint `new()` Box requires.
    auto r = runAot(
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        + kBox +
        "extension Box<IShape> "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(
        r, "type argument `IShape` does not satisfy the constraint `where IShape: new()`"));
}

BEZEL_TEST(sema_extension_head_constraints_5549,
           same_named_parameter_does_not_inherit_constraint_rejected_sec_26_4) {
    // §26.4 (v0.338): a module type `T` does not make the block's `T` inherit
    // Box's constraint; the head binds a fresh parameter that states nothing.
    auto r = runAot(
        "module M; "
        "class T end T; "
        + kBox +
        "extension Box<T> "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(
        r, "type argument `T` does not satisfy the constraint `where T: new()`"));
}

BEZEL_TEST(sema_extension_head_constraints_5549,
           different_constraint_on_block_rejected_sec_26_4) {
    // §26.4 (v0.338): `where T: class` does not guarantee Box's `new()`; a
    // class without a public parameterless constructor is not admitted.
    auto r = runAot(kWidget + kBox +
                    "extension Box<T> where T: class "
                    "  public method P(): int return 7; end P; "
                    "end; " + kMainBox);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(
        r, "type argument `T` does not satisfy the constraint `where T: new()`"));
}

BEZEL_TEST(sema_extension_head_constraints_5549,
           value_block_over_class_constraint_rejected_sec_26_4) {
    // §26.4 (v0.338): a block `where T: value` cannot guarantee Box's `class`
    // constraint for a non-value parameter; here Box requires `value`, and the
    // unrestated head states nothing.
    auto r = runAot(
        "module M; "
        "class Box<T> where T: value "
        "  public constructor() end constructor; "
        "end Box; "
        "extension Box<T> "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(
        r, "type argument `T` does not satisfy the constraint `where T: value`"));
}

BEZEL_TEST(sema_extension_head_constraints_5549,
           two_parameters_partly_restated_rejected_sec_26_4) {
    // §26.4 (v0.338): the block restates `where A: class` only; Two's second
    // parameter requires `new()`, which the block does not state.
    auto r = runAot(
        "module M; "
        "class Two<A, B> where A: class where B: new() "
        "  public constructor() end constructor; "
        "end Two; "
        "extension Two<A, B> where A: class "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(
        r, "type argument `B` does not satisfy the constraint `where B: new()`"));
}

BEZEL_TEST(sema_extension_head_constraints_5549,
           type_bound_parameter_not_restated_rejected_sec_26_4) {
    // §14.2 *Type-parameter bounds* / §26.4 (v0.338): Rel's `where T: U` at
    // the head `Rel<A, B>` needs the block to state `where A: B`.
    auto r = runAot(
        "module M; "
        "class Rel<T, U> where T: U "
        "  public constructor() end constructor; "
        "end Rel; "
        "extension Rel<A, B> "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(
        r, "type argument `A` does not satisfy the constraint `where A: B`"));
}

// ---------------------------------------------------------------------------
// Accept rows: the block restates the clause; the program compiles and runs.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_extension_head_constraints_5549,
           restated_head_over_new_constraint_accepted_sec_26_4) {
    // §26.4 (v0.338): `extension Box<T> where T: new()` restates Box's clause;
    // the extension applies to Box<Widget> and runs.
    auto r = runAot(kWidget + kBox +
                    "extension Box<T> where T: new() "
                    "  public method P(): int return 7; end P; "
                    "end; " + kMainBox);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

BEZEL_TEST(sema_extension_head_constraints_5549,
           stronger_restated_head_accepted_sec_26_4) {
    // §26.4 (v0.338): a block clause stronger than the extended type's
    // (`class, new()` over `new()`) guarantees it and is admitted.
    auto r = runAot(kWidget + kBox +
                    "extension Box<T> where T: class, new() "
                    "  public method P(): int return 7; end P; "
                    "end; " + kMainBox);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

BEZEL_TEST(sema_extension_head_constraints_5549,
           restated_hashable_head_accepted_sec_26_4) {
    // §26.4 (v0.338): `extension Counter<K> where K: hashable` restates the
    // clause; Counter<string> instantiates it with a hashable string.
    auto r = runAot(
        "module M; "
        "class Counter<K> where K: hashable "
        "  public constructor() end constructor; "
        "end Counter; "
        "extension Counter<K> where K: hashable "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Counter<string>(); "
        "    Console.WriteLine(c.P().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

BEZEL_TEST(sema_extension_head_constraints_5549,
           two_parameters_fully_restated_accepted_sec_26_4) {
    // §26.4 (v0.338): the block restates both clauses; Two<Widget, Widget>
    // satisfies them and the extension runs.
    auto r = runAot(
        kWidget +
        "class Two<A, B> where A: class where B: new() "
        "  public constructor() end constructor; "
        "end Two; "
        "extension Two<A, B> where A: class where B: new() "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var t := new Two<Widget, Widget>(); "
        "    Console.WriteLine(t.P().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

BEZEL_TEST(sema_extension_head_constraints_5549,
           type_bound_parameter_restated_accepted_sec_26_4) {
    // §14.2 *Type-parameter bounds* / §26.4 (v0.338): the block states
    // `where A: B`, which is the bound Rel's `where T: U` requires.
    auto r = runAot(
        kWidget +
        "class Rel<T, U> where T: U "
        "  public constructor() end constructor; "
        "end Rel; "
        "extension Rel<A, B> where A: B "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var r := new Rel<Widget, Widget>(); "
        "    Console.WriteLine(r.P().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}
