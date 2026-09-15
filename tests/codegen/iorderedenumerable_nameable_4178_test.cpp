// fix #4178 (stdlib §5, `Resolver::builtinTypeArity` /
// `common::kHeapPointerReferenceBuiltins`): `IOrderedEnumerable<T>` — the
// `OrderBy`/`OrderByDescending` result interface — could not be named
// explicitly in source at all: `resolveNamed` reported "unresolved type
// name `IOrderedEnumerable`" for a return type, parameter type, local
// declaration, generic argument, or field, even though the type is fully
// functional when only ever INFERRED (`var sorted := xs.OrderBy(...)`
// already worked and dispatched `.First()` etc. correctly, per the issue's
// own repro).
//
// TWO fixes landed together, because the first alone regresses to a WORSE
// failure than the one it fixes:
//
//   1. `Resolver::builtinTypeArity` (TypeResolution_Names.cpp) did not list
//      `IOrderedEnumerable` at all — the literal "unresolved type name" bug.
//      Registered at arity 1 (`IOrderedEnumerable<T>`), the same
//      declaration-less Named-ref shape every other builtin interface in
//      that table already uses (mirrors sibling `IGrouping<TKey,T>`,
//      issue #2794).
//
//   2. With (1) alone, a function whose declared return type is the new
//      `IOrderedEnumerable<T>` annotation IR-verify-CRASHES at codegen
//      ("Function return type does not match operand type of return inst!
//      ret ptr %linq.owncl / i32") — `lowerTypeRef`'s Named case fell
//      through to the i32 default instead of the `ptr` every other
//      lazy-query-shaped builtin (`IEnumerable`, `IGrouping`, both already
//      in `kHeapPointerReferenceBuiltins`) gets. Fixed by adding
//      `IOrderedEnumerable` to that table (BuiltinRegistry.cpp) — the
//      OrderBy runtime representation was always a correct `bz_lazy_t*`
//      pointer; only the STATIC type's LLVM lowering was wrong.
//
// Deliberately NOT included here (see issue #4207, filed as this fix's own
// "or say which do not [work]" half): calling a terminal LINQ operator
// (`.First()`, `.Count()`, ...) or using `for`-in directly on a value whose
// STATIC type is the named `IOrderedEnumerable<T>` (as opposed to being
// merely inferred) is still unimplemented in codegen's consuming-side
// dispatch. A sema-only fix for that was written, measured to silently
// degrade `.First()` to the WRONG value (0 instead of the correct sorted
// first element) via the pre-existing `#2491` zero/null-return backstop
// (Severity::Warning — compiles and RUNS with a wrong answer), and reverted
// rather than shipped. The grid below therefore round-trips the named type
// through every position WITHOUT calling an unimplemented member on it, and
// separately proves the underlying OrderBy/First() value pipeline is
// unaffected via the pre-existing, already-working `var`-inferred path.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// FAIL-WITHOUT (issue #4178's own repro shape, adapted — see note below): a
// return-type annotation alone. Pre-fix (1): "unresolved type name
// `IOrderedEnumerable`" at compile time (r.ran == false). Pre-fix (2) only
// (arity registered, heap-pointer table not yet updated): an IR-verifier
// crash ("IR verification failed (malformed IR emitted...)"), also
// r.ran == false — a DIFFERENT, WORSE failure than (1)'s clean diagnostic.
// Post-fix: r.ran == true.
//
// NOTE, measured while writing this test: the issue's own repro chains
// `Foo(xs).First()` directly on the call result. That specific chain is
// NOT what this row exercises, and deliberately so — `var sorted :=
// Foo(xs)` infers `sorted`'s type from `Foo`'s DECLARED return type
// (the explicit `IOrderedEnumerable<int>` this fix makes nameable), which
// is a DIFFERENT internal shape than the nameless "lazy sentinel"
// `var sorted := xs.OrderBy(...)` produces when inferred directly — and
// calling `.First()` on that explicitly-named-typed shape is issue #4207's
// still-open gap (measured: r.ran == true, but the printed value is 0, not
// the correct 1 — a silently WRONG answer, not a clean rejection). Chaining
// `.First()` here would make this row depend on #4207 landing first, and
// assert on a value THIS fix cannot make correct without a codegen change
// out of scope for a name-resolution fix (see #4207). This row instead
// pins exactly what #4178 fixes: the return-type annotation itself
// resolves and round-trips through real AOT compile + link + native exec
// without crashing.
BEZEL_TEST(iorderedenumerable_nameable_4178, issue_repro_return_type_roundtrips) {
    const std::string src =
        "module M; "
        "public static class Program "
        "  public static method Foo(xs: Array<int>): IOrderedEnumerable<int> => "
        "      xs.OrderBy(lambda(x) => x); "
        "  public static method Main(): int "
        "    var sorted := Foo([3, 1, 2]); "
        "    Console.WriteLine(\"ran\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "ran"));
}

// The full grid #4178 asks for: declared local, parameter, return type,
// generic argument, and field — all naming `IOrderedEnumerable<int>`
// explicitly, round-tripping ONE value through every position without
// calling an unimplemented member on the named-typed bindings themselves.
// A SEPARATE, independent `var`-inferred `.OrderBy(...).First()` call (the
// pre-existing, always-working path — the issue's own "works when the type
// is never named explicitly" control) proves the underlying LINQ ordering
// pipeline itself is byte-for-byte unaffected by this fix.
BEZEL_TEST(iorderedenumerable_nameable_4178, full_grid_local_param_return_generic_field) {
    const std::string src =
        "module M; "
        "public class Widget "
        "  public property Seq: IOrderedEnumerable<int>?; "
        "end Widget; "
        "public static class Program "
        // return type
        "  public static method MakeOrdered(xs: Array<int>): IOrderedEnumerable<int> => "
        "      xs.OrderBy(lambda(x) => x); "
        // parameter type (+ return type again, pass-through)
        "  public static method PassThrough(seq: IOrderedEnumerable<int>): IOrderedEnumerable<int> => "
        "      seq; "
        "  public static method Identity<T>(x: T): T => x; "
        "  public static method Main(): int "
        "    var xs: Array<int> := [30, 10, 20]; "
        // declared local
        "    var s: IOrderedEnumerable<int> := MakeOrdered(xs); "
        "    var s2 := PassThrough(s); "
        // explicit generic argument
        "    var s3 := Identity<IOrderedEnumerable<int>>(s2); "
        // field
        "    using var w := new Widget(); "
        "    w.Seq := s3; "
        // control: the pre-existing var-inferred path, unaffected.
        "    var sorted := xs.OrderBy(lambda(x) => x); "
        "    var first := sorted.First(); "
        "    Console.WriteLine($\"first={first}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // xs sorted ascending is [10, 20, 30] — First() is 10.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "first=10"));
    // Regression pin: neither backstop diagnostic from the two pre-fix
    // states (LANE_RULES §18 — pair a compile-success assertion with an
    // explicit text check, since a crash can also leave r.ran indeterminate
    // in other shapes even though this one is a hard r.ran==false).
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("unresolved type name") ==
                          std::string::npos);
        BEZEL_EXPECT_TRUE(d.message.find("IR verification failed") ==
                          std::string::npos);
    }
}
