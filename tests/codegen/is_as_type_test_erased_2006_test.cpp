// #2006 (§12.9/§14.2/§17.4, v0.197) — the runtime-correctness half of the plain
// `is`/`as` erasure guard. The sema half (is_as_type_test_erased_2006_test.cpp)
// pins that a FUNCTION-typed generic argument is REJECTED at compile time on the
// plain expression path (parity with the case-binding path). This half pins the
// OTHER side: the REIFIED arguments that stay admitted must still compile AND
// return the CORRECT runtime answer — the guard must not over-reject the sound
// cases, and #2001's per-monomorphization descriptor precision must be intact for
// the plain `is`/`as` path.
//
// Each test observes a bool via the process exit code (1 = true, 0 = false), or
// packs several into one integer.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// A phantom `Wrap<W>` (W is only a type argument, never stored — isolates the
// descriptor key) plus a concrete `Foo`, followed by a Main body.
int runProg(const std::string& mainBody) {
    return run(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class Foo end Foo; "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;");
}

}  // namespace

// (1) Concrete PRIMITIVE argument: `Wrap<int> is Wrap<int>` matches (admitted +
// runtime-correct), `Wrap<int> is Wrap<string>` does not.
BEZEL_TEST(codegen_2006, primitive_arg_is_precise) {
    BEZEL_EXPECT_EQ(runProg(
        "using var w := new Wrap<int>(7); "
        "var o: Object := w; "
        "var a := (if o is Wrap<int> then 1 else 0); "
        "var b := (if o is Wrap<string> then 1 else 0); "
        "return a * 2 + b;"), 2);  // match(1)*2 + wrong(0)
}

// (2) Concrete NAMED-class argument stays admitted + precise.
BEZEL_TEST(codegen_2006, named_class_arg_is_precise) {
    BEZEL_EXPECT_EQ(runProg(
        "using var w := new Wrap<Foo>(7); "
        "var o: Object := w; "
        "var a := (if o is Wrap<Foo> then 1 else 0); "
        "var b := (if o is Wrap<int> then 1 else 0); "
        "return a * 2 + b;"), 2);
}

// (3) FOLDED inline-array argument (post-#2001): `Wrap<[3]int>` is admitted and
// distinguished from `Wrap<[4]int>` by size — the guard must not undo #2001.
BEZEL_TEST(codegen_2006, inline_array_arg_is_precise) {
    BEZEL_EXPECT_EQ(runProg(
        "using var w := new Wrap<[3]int>(7); "
        "var o: Object := w; "
        "var a := (if o is Wrap<[3]int> then 1 else 0); "
        "var b := (if o is Wrap<[4]int> then 1 else 0); "
        "return a * 2 + b;"), 2);
}

// (4) NESTED reified argument: `Wrap<Wrap<int>>` admitted; distinguished from
// `Wrap<Wrap<string>>`.
BEZEL_TEST(codegen_2006, nested_reified_arg_is_precise) {
    BEZEL_EXPECT_EQ(runProg(
        "using var w := new Wrap<Wrap<int>>(7); "
        "var o: Object := w; "
        "var a := (if o is Wrap<Wrap<int>> then 1 else 0); "
        "var b := (if o is Wrap<Wrap<string>> then 1 else 0); "
        "return a * 2 + b;"), 2);
}

// (5) `as` to a reified argument: a matching cast is non-null, a wrong one is null
// (§17.4). Admitted + runtime-correct.
BEZEL_TEST(codegen_2006, as_reified_arg_is_precise) {
    BEZEL_EXPECT_EQ(runProg(
        "using var w := new Wrap<int>(7); "
        "var o: Object := w; "
        "var hit := o as Wrap<int>; "
        "var miss := o as Wrap<string>; "
        "var a := (if hit <> null then 1 else 0); "
        "var b := (if miss = null then 1 else 0); "
        "return a * 2 + b;"), 3);  // hit non-null(1)*2 + miss null(1) = 3
}

// (6) BARE TYPE PARAMETER in a generic body (the #1843 shape) stays admitted and
// resolves to the concrete argument at monomorphization: `o is Wrap<Wrap<T>>`
// with T bound to int matches a `Wrap<Wrap<int>>` and rejects a
// `Wrap<Wrap<string>>`. The over-rejection guard, verified at runtime.
BEZEL_TEST(codegen_2006, bare_type_parameter_resolves_at_monomorphization) {
    int code = run(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class Checker<T> "
        "  public method probe(m: Object, w: Object): int "
        "    => (if m is Wrap<Wrap<T>> then 1 else 0) * 2 "
        "     + (if w is Wrap<Wrap<T>> then 1 else 0); "
        "end Checker; "
        "class C public static method Main(): int "
        "  using var wi := new Wrap<Wrap<int>>(1); "
        "  using var ws := new Wrap<Wrap<string>>(2); "
        "  using var ch := new Checker<int>(); "
        "  var mi: Object := wi; "
        "  var os: Object := ws; "
        "  return ch.probe(mi, os); "  // match(1)*2 + wrong(0) = 2
        " end Main; end C;");
    BEZEL_EXPECT_EQ(code, 2);
}
