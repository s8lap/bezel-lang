// fix #1854 / §19.3 / §31: a `case`/`when` TYPE-TEST pattern whose target is a
// BUILT-IN exception type (`case e of InvalidOperationException => …`) always
// evaluated FALSE at runtime — the arm never fired and control fell through to
// `_`, selecting the WRONG branch (a silent miscompile; the program compiled
// clean and ran, but the result was wrong).
//
// Root cause (src/codegen/LlvmBackend_Exprs2.cpp, the NamedPattern type-test
// path): the built-in exception types carry no user `TypeSymbol`, so their sema
// `TypeRef` is `Kind::Named` with a NULL `declaration`. The `objectLike` guard
// that gates the runtime `emitIsA` walk only recognised a scrutinee whose
// declaration is a class/interface (or the `Object` primitive), so a scrutinee
// statically typed as a built-in exception (`catch e: Exception`) was treated as
// non-object-like and the test short-circuited to `false`. §19.3 (v0.196): a
// built-in exception type is a valid type-test pattern and "Subtypes match", so
// the walk must run. The built-in exception object DOES carry a vtable whose
// word 0 is the `btd$<name>` descriptor `emitIsA` walks (setupBuiltinExceptions),
// so it is object-like exactly like a class instance.
//
// The companion TYPED-BINDING form (`case e of var x: IOException => …`) shared
// the same root cause on the BindingPattern path: with the pattern type's
// `declaration` null, the pattern returned nullptr and left `case.end`
// unterminated → malformed IR (a hard codegen abort). Both are fixed here.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §19.3 / §31 (#1854 repro): a bare built-in-exception type-name pattern matches
// when the caught exception's runtime type IS that type. Throw an
// InvalidOperationException, then `case e of InvalidOperationException => 2`.
// Previously: always false → fell through to `_` → 9 (WRONG). Now: 2.
BEZEL_TEST(case_builtin_exception_type_test_1854, bare_type_test_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var r := 0; "
        "    try throw new InvalidOperationException(\"x\"); "
        "    catch e: Exception => "
        "      r := (case e of "
        "        ArgumentException => 1, "
        "        InvalidOperationException => 2, "
        "        _ => 9); "
        "    end; "
        "    return r; "
        "  end Main; end Program;"), 2);
}

// §19.3 / §31 (#1854): the CORRECT built-in-exception arm is selected and the
// WRONG (non-matching) built-in arm is correctly NOT taken. Throw an
// ArgumentException with the InvalidOperationException arm listed FIRST: the
// InvalidOperationException test must be false (not the runtime type) and the
// ArgumentException test true. Previously BOTH built-in tests were false → `_` →
// 9 (WRONG). Now: 1 (ArgumentException matches; InvalidOperationException does
// not).
BEZEL_TEST(case_builtin_exception_type_test_1854, correct_arm_selected_not_sibling) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var r := 0; "
        "    try throw new ArgumentException(\"y\"); "
        "    catch e: Exception => "
        "      r := (case e of "
        "        InvalidOperationException => 2, "
        "        ArgumentException => 1, "
        "        _ => 9); "
        "    end; "
        "    return r; "
        "  end Main; end Program;"), 1);
}

// §31 (#1854): "Subtypes match" — a FileNotFoundException is-a IOException, so a
// `case e of IOException => …` type-test over a thrown FileNotFoundException
// matches via the descriptor base-chain (§31 exception hierarchy). Previously:
// false → 0 (WRONG). Now: 5.
BEZEL_TEST(case_builtin_exception_type_test_1854, subtype_matches_via_base_chain) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var r := 0; "
        "    try throw new FileNotFoundException(\"f\"); "
        "    catch e: Exception => "
        "      r := (case e of IOException => 5, _ => 0); "
        "    end; "
        "    return r; "
        "  end Main; end Program;"), 5);
}

// §19.3 / §31 (#1854): a type-test that does NOT match must fall through. Throw a
// FormatException (neither ArgumentException nor InvalidOperationException); the
// wildcard arm fires → 9. Guards that the emitIsA walk over a built-in exception
// correctly rejects an unrelated built-in target (does not over-match).
BEZEL_TEST(case_builtin_exception_type_test_1854, nonmatching_type_falls_through) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var r := 0; "
        "    try throw new FormatException(\"z\"); "
        "    catch e: Exception => "
        "      r := (case e of "
        "        ArgumentException => 1, "
        "        InvalidOperationException => 2, "
        "        _ => 9); "
        "    end; "
        "    return r; "
        "  end Main; end Program;"), 9);
}

// §19.3 / §31 (#1854): the TYPED-BINDING form `var x: T` over a built-in
// exception. Throw an InvalidOperationException; `case e of var x:
// InvalidOperationException => 3`. Previously: malformed IR (unterminated
// case.end) — a hard compile abort (run() would see r.ran==false → -999). Now:
// compiles and returns 3.
BEZEL_TEST(case_builtin_exception_type_test_1854, typed_binding_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var r := 0; "
        "    try throw new InvalidOperationException(\"b\"); "
        "    catch e: Exception => "
        "      r := (case e of "
        "        var x: InvalidOperationException => 3, "
        "        _ => 0); "
        "    end; "
        "    return r; "
        "  end Main; end Program;"), 3);
}
