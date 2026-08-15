// Spec-derived regression test for issue #3119.
//
// §8.5 "ToString" (v0.252) / stdlib §2.13 "The `Type` class" rule 1, rule 2,
// and rule 3. Pins the qualified-name default for a reserved BUILT-IN CLASS
// (§2.13 rule 1), the keyword-aliased primitive exception (§2.13 rule 2),
// and the generic-instantiation generic-args tail (§2.13 rule 3).
//
// #2934's fix delivered the module-path half for every type that carries a
// `TypeSymbol::moduleName`; #3119's fix covers the residual:
//   (a) reserved built-in classes (no `TypeSymbol::decl`, empty
//       `moduleName`) — the table in `common::BuiltinRegistry` is the single
//       authority for the qualified module path;
//   (b) generic instantiation rendering per stdlib §2.13 rule 3 — the
//       head followed by `<arg1, arg2, …>` with each arg recursing.
//
// The eight §8.5 emission sites (`emitVtablelessBuiltinToString`,
// `setupBuiltinExceptions`, `valueBuiltinBoxToStringThunk`, the box thunk
// slot-3 default, the class-slot-3 thunk default, the enum default,
// `classToStringThunk` shared default, the #2457/#2458/#2459 literals)
// route through `common::builtinClassModulePathOf` and, where the `TypeRef`
// is in hand, `Emitter::spec85TypeRefQualifiedName`.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string out(const std::string& prog) {
    return runAotOut(prog);
}

// module + class skeleton around a Main body.
std::string prog(const std::string& mainBody,
                 const std::string& prelude = "") {
    return "module M; import System.Threading; "
           "import System.Collections.Generic; " +
           prelude +
           "class C public static method Main(): int " + mainBody +
           " return 0; end Main; end C;";
}

}  // namespace

// ===========================================================================
// (a) stdlib §2.13 rule 1: a reserved built-in class renders its QUALIFIED
// module path, not the bare identifier. Pinned per class shape; each entry
// is from the stdlib section-structure table in `common::BuiltinRegistry`.
// ===========================================================================

// stdlib §6.2: `Mutex` lives in `System.Threading`.
BEZEL_TEST(builtin_qualified_tostring_3119, mutex_renders_system_threading) {
    BEZEL_EXPECT_EQ(
        out(prog("using var x := new Mutex(); "
                 "Console.WriteLine($\"v={x}\");",
                 "import System.Threading; ")),
        std::string("v=System.Threading.Mutex"));
}

// stdlib §2.7: `Random` lives in `System`.
BEZEL_TEST(builtin_qualified_tostring_3119, random_renders_system) {
    BEZEL_EXPECT_EQ(
        out(prog("using var r := new Random(); "
                 "Console.WriteLine($\"v={r}\");")),
        std::string("v=System.Random"));
}

// stdlib §3.1: `StringBuilder` lives in `System.Text`.
BEZEL_TEST(builtin_qualified_tostring_3119, stringbuilder_renders_system_text) {
    BEZEL_EXPECT_EQ(
        out(prog("using var sb := new StringBuilder(); "
                 "sb.Append(\"x\"); "
                 "Console.WriteLine($\"v={sb}\");",
                 "import System.Text; ")),
        std::string("v=x"));   // §3.1: StringBuilder DECLARES ToString → content
}

// stdlib §2.17: `Exception` (and the whole hierarchy) lives in `System`.
BEZEL_TEST(builtin_qualified_tostring_3119, exception_renders_system) {
    BEZEL_EXPECT_EQ(
        out(prog("using var e := new Exception(\"x\"); "
                 "Console.WriteLine($\"v={e}\");",
                 "import System; ")),
        std::string("v=System.Exception"));
}

// stdlib §7.2: `FileStream` lives in `System.IO`.
BEZEL_TEST(builtin_qualified_tostring_3119, filestream_renders_system_io) {
    BEZEL_EXPECT_EQ(
        out(prog("using var f := new FileStream(\"/dev/null\", 1); "
                 "Console.WriteLine($\"v={f}\");",
                 "import System.IO; ")),
        std::string("v=System.IO.FileStream"));
}

// stdlib §8.1: `Stopwatch` lives in `System.Diagnostics`.
BEZEL_TEST(builtin_qualified_tostring_3119, stopwatch_renders_system_diagnostics) {
    BEZEL_EXPECT_EQ(
        out(prog("using var s := new Stopwatch(); "
                 "Console.WriteLine($\"v={s}\");",
                 "import System.Diagnostics; ")),
        std::string("v=System.Diagnostics.Stopwatch"));
}

// stdlib §2.17: an Exception SUBCLASS — `ArgumentException` — also lives
// in `System`. (Pins that the whole exception hierarchy, not just the
// root, gets the qualified module path.)
BEZEL_TEST(builtin_qualified_tostring_3119, argumentexception_renders_system) {
    BEZEL_EXPECT_EQ(
        out(prog("using var e := new ArgumentException(\"x\", \"p\"); "
                 "Console.WriteLine($\"v={e}\");",
                 "import System; ")),
        std::string("v=System.ArgumentException"));
}

// stdlib §6.4: an atomic — `Atomic<int>` — lives in `System.Threading`.
// The §2.13 rule 3 generic-args tail `<int>` is also rendered: this site
// has the `TypeRef` in hand and recurses through `spec85TypeRefQualifiedName`.
BEZEL_TEST(builtin_qualified_tostring_3119, atomic_renders_system_threading) {
    BEZEL_EXPECT_EQ(
        out(prog("using var a := new Atomic<int>(0); "
                 "Console.WriteLine($\"v={a}\");")),
        std::string("v=System.Threading.Atomic<int>"));   // #3119: was "v=Atomic"
}

// ===========================================================================
// (b) stdlib §2.13 rule 3: a generic INSTANTIATION renders the head
// followed by `<arg1, arg2, …>`, each argument rendered recursively. The
// §8.5 / §2.13 example is
//   "Dictionary<string, Array<int>>"
//   → "System.Collections.Generic.Dictionary<string, System.Collections.Generic.Array<int>>"
// ===========================================================================

// `Array<int>` — single keyword-alias arg.
BEZEL_TEST(builtin_qualified_tostring_3119, array_generic_args) {
    BEZEL_EXPECT_EQ(
        out(prog("using var a := new Array<int>(1); "
                 "Console.WriteLine($\"v={a}\");")),
        std::string("v=System.Collections.Generic.Array<int>"));
}

// `Dictionary<string, int>` — two keyword-alias args.
BEZEL_TEST(builtin_qualified_tostring_3119, dictionary_two_keyword_args) {
    BEZEL_EXPECT_EQ(
        out(prog("using var d := new Dictionary<string, int>(); "
                 "Console.WriteLine($\"v={d}\");")),
        std::string("v=System.Collections.Generic.Dictionary<string, int>"));
}

// `Dictionary<string, Array<int>>` — stdlib §2.13's verbatim example
// (FullName form). Nested generic, recursing through the inner arg.
BEZEL_TEST(builtin_qualified_tostring_3119, dictionary_nested_array_arg) {
    BEZEL_EXPECT_EQ(
        out(prog("using var d := new Dictionary<string, Array<int>>(); "
                 "Console.WriteLine($\"v={d}\");")),
        std::string(
            "v=System.Collections.Generic.Dictionary<string, "
            "System.Collections.Generic.Array<int>>"));
}

// `Array<int?>` — nullable type ARGUMENT is part of identity and renders
// with a trailing `?`. (Top-level nullability is reified away per §17.10;
// inside `<…>` it renders.)
BEZEL_TEST(builtin_qualified_tostring_3119, array_nullable_arg_renders_question) {
    BEZEL_EXPECT_EQ(
        out(prog("using var a := new Array<int?>(1); "
                 "Console.WriteLine($\"v={a}\");")),
        std::string("v=System.Collections.Generic.Array<int?>"));
}

// A USER-class generic arg (`M.Res`) recurses through the module path.
BEZEL_TEST(builtin_qualified_tostring_3119, user_class_generic_arg) {
    BEZEL_EXPECT_EQ(
        out(prog("using var a := new OwningArray<Res>(); "
                 "a.Add(new Res(0)); "
                 "Console.WriteLine($\"v={a}\");",
                 "class Res public Tag: int; "
                 "  public constructor(t: int) Tag := t; end constructor; "
                 "end Res; ")),
        std::string("v=System.Collections.Generic.OwningArray<M.Res>"));
}

// ===========================================================================
// stdlib §2.13 rule 2: the 14 keyword-aliased built-in types render with
// their lowercase keyword in BOTH `Name` and `FullName` — the keyword is
// the canonical name, a recorded exception to qualification. Two pins:
// (1) the explicit .ToString() call site (boxed or otherwise) returns the
// value, not the type name, when the receiver is a primitive value; (2) the
// keyword itself is NOT qualified — e.g. `string` renders as "string"
// (length 6), not "System.String" (length 12), inside a composite.
// ===========================================================================

// `string` renders as "string" — 6 chars — inside a generic arg position.
BEZEL_TEST(builtin_qualified_tostring_3119, string_keyword_lowercase_in_arg) {
    // The string shows up as the LENGTH of an Array<string>'s rendered
    // type name — "System.Collections.Generic.Array<string>" is 40 chars
    // (matches the stdlib §2.13 contract for keyword-aliased primitives
    // appearing as type arguments, fix #3119 rule 2 + rule 3).
    auto r = runAot(prog("using var a := new Array<string>(1); "
                        "var s := a.ToString(); "
                        "return s.Length;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(40, r.exitCode);   // "System.Collections.Generic.Array<string>"
}

// ===========================================================================
// Regression guards: existing v0.252 (#2934) behavior is unchanged.
// ===========================================================================

// §8.5: a USER class with no ToString override renders its MODULE-qualified
// name (#2934's path).
BEZEL_TEST(builtin_qualified_tostring_3119, user_class_module_path_2934) {
    BEZEL_EXPECT_EQ(
        out(prog("using var u := new U(); "
                 "Console.WriteLine($\"v={u}\");",
                 "class U public constructor() end constructor; end U; ")),
        std::string("v=M.U"));
}

// §8.5: a USER class WITH a ToString override dispatches it (#8.5 "via
// virtual dispatch"); the override is unchanged.
BEZEL_TEST(builtin_qualified_tostring_3119, user_class_override_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog("using var u := new U(); "
                 "Console.WriteLine($\"v={u}\");",
                 "class U "
                 "  public constructor() end constructor; "
                 "  public override method ToString(): string return \"hi\"; "
                 "  end ToString; "
                 "end U; ")),
        std::string("v=hi"));
}