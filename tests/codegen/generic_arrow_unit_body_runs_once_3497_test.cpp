// Regression for #3497 (spec §4.7 / §22.2 / §14.1, v0.283): a GENERIC method
// whose ARROW body lowers as unit ran its body TWICE.
//
// §22.2 ("Expression-bodied methods"): "When the body is a single expression,
// `=> expression` replaces the block."  §4.7 ("The unit type"): "`()` is the
// type with exactly one value, also written `()`. It is the return type of
// methods declared without an explicit return type."  Together those make
// `public static method Fwd<T>(c: Counter, x: T) => c.Bump();` a legal §14.1
// generic declaration whose body is a single, unit-valued expression.  Nothing
// in §22.2 makes an arrow body a SECOND evaluation of anything.
//
// The defect was #2963's "`nullptr` means two categorically different things",
// at a different producer.  `emitInlineGenericMethod` inlines an expression
// body with `result = lowerExpr(*ci.exprBody);` and returns that value; for a
// unit-lowering body `lowerExpr` returns nullptr because the §4.7 result HAS no
// value (the invariant #3421 made universal at the producer).  Three of its
// four call sites in LlvmBackend_Calls2.cpp read that nullptr as "the inline
// path declined" —
//
//     llvm::Value* r = emitInlineGenericMethod(ms, args);
//     if (r != nullptr || diverged_) { emitPendingCheck(); return r; }
//     ... falls through and emits the ordinary compiled call
//
// — and emitted the compiled call ON TOP of the body they had just inlined.
// Measured on `ef339fa` with `objdump -dr`: `bz$M.C$Main` held BOTH the inlined
// `bz$M.Counter$Bump` call AND a call to `bz$M.C$Fwd`, whose own body calls
// `Bump` again.  Exit 0, no diagnostic, a doubled side effect.
//
// ⚠ An exit-code oracle is VACUOUS here: the program exits 0 either way and the
// wrong answer is a plausible number.  Every row below therefore asserts exact
// `stdoutText` over an observable that COUNTS — a field a method bumps — so
// "the body ran twice" is directly visible as `2` rather than inferred.
//
// FAILS-WITHOUT the fix: `site_d_*` and `class_generic_*` print "2\n" instead of
// "1\n"; `generic_method_on_generic_class` prints "3\n" (site C and site D BOTH
// fall through, so the body runs twice inline and once compiled); `two_calls`
// prints "4\n" instead of "2\n".  PASSES-WITH: every row prints its count.
//
// The `control_*` rows are shapes the fix must not perturb, and they pass on
// BOTH the pre- and post-fix compiler (measured): the non-generic spelling never
// reaches the inline path at all; the value-returning generic arrow body returns
// a non-null `r`, so its call site always returned; and the generic BLOCK body
// goes through `emitGenericSpecialization`, which hands back the (void-typed but
// non-null) `CallInst`.

#include "support/aot_support.hpp"   // runAot — the strict AOT oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A counter whose `Bump` is the observable: `_n` is exactly the number of times
// the callee's body executed, so a doubled call is a `2` rather than a subtle
// difference in something else.
const char* kCounter =
    "module M;\n"
    "class Counter\n"
    "  public _n: int := 0;\n"
    "  public method Bump() _n := _n + 1; end Bump;\n"
    "  public method BumpGet(): int _n := _n + 1; return _n; end BumpGet;\n"
    "end Counter;\n";

// Runs `kCounter` + `rest` and returns captured stdout, or a sentinel that can
// never be mistaken for a count when the program did not build/run.
std::string out(const std::string& rest) {
    auto r = runAot(std::string(kCounter) + rest);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// ── The reported program, exactly ────────────────────────────────────────────
// A method-level generic (`<T>`) on a NON-generic class, static, unit arrow
// body.  Reaches the `typesMismatch` call site (LlvmBackend_Calls2.cpp), which
// force-specializes because `T` is bound in the active substitution.
// Without the fix: "2\n".
BEZEL_TEST(generic_arrow_unit_body_runs_once_3497, site_d_method_generic) {
    BEZEL_EXPECT_EQ(out(
        "class C\n"
        "  public static method Fwd<T>(c: Counter, x: T) => c.Bump();\n"
        "  public static method Main(): int\n"
        "    using var c := new Counter();\n"
        "    C.Fwd<int>(c, 1);\n"
        "    Console.WriteLine(c._n.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n"), "1\n");
}

// §4.7 makes `: ()` and no return clause the SAME type, and #3434 already made
// the compiler ask that question semantically rather than syntactically.  The
// two spellings must therefore run the body the same number of times.
// Without the fix: "2\n".
BEZEL_TEST(generic_arrow_unit_body_runs_once_3497, site_d_unit_ascription) {
    BEZEL_EXPECT_EQ(out(
        "class C\n"
        "  public static method Fwd<T>(c: Counter, x: T): () => c.Bump();\n"
        "  public static method Main(): int\n"
        "    using var c := new Counter();\n"
        "    C.Fwd<int>(c, 1);\n"
        "    Console.WriteLine(c._n.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n"), "1\n");
}

// The SECOND affected call site (`parentIsGeneric`): an INSTANCE method with no
// generic parameters of its own, on a generic CLASS, inlined because the
// instantiation's layout differs from the erased template's.  A fix applied
// only at the `typesMismatch` site leaves this one doubling.
// Without the fix: "2\n".
BEZEL_TEST(generic_arrow_unit_body_runs_once_3497, site_c_generic_class_instance) {
    BEZEL_EXPECT_EQ(out(
        "class Box<T>\n"
        "  public _c: Counter;\n"
        "  public _v: T;\n"
        "  public constructor(c: Counter, v: T) _c := c; _v := v; end constructor;\n"
        "  public method Ping() => _c.Bump();\n"
        "end Box;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var c := new Counter();\n"
        "    using var b := new Box<int>(c, 7);\n"
        "    b.Ping();\n"
        "    Console.WriteLine(c._n.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n"), "1\n");
}

// The same `parentIsGeneric` site through a STATIC method on a generic class —
// §22.12's per-instantiation monomorphization reaches it with no receiver.
// Without the fix: "2\n".
BEZEL_TEST(generic_arrow_unit_body_runs_once_3497, site_c_generic_class_static) {
    BEZEL_EXPECT_EQ(out(
        "class Box<T>\n"
        "  public static method Ping(c: Counter) => c.Bump();\n"
        "end Box;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var c := new Counter();\n"
        "    Box<int>.Ping(c);\n"
        "    Console.WriteLine(c._n.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n"), "1\n");
}

// Both affected sites on ONE call: a generic METHOD on a generic CLASS falls
// through `parentIsGeneric` AND then `typesMismatch`, so the body was inlined
// TWICE and the compiled function called once — three executions of `Bump` for
// one call expression.  This row is what shows the defect is per-site rather
// than per-call, and that "2" was never the ceiling.
// Without the fix: "3\n".
BEZEL_TEST(generic_arrow_unit_body_runs_once_3497,
           generic_method_on_generic_class) {
    BEZEL_EXPECT_EQ(out(
        "class Box<T>\n"
        "  public _v: T;\n"
        "  public constructor(v: T) _v := v; end constructor;\n"
        "  public method Ping<U>(c: Counter, u: U) => c.Bump();\n"
        "end Box;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var c := new Counter();\n"
        "    using var b := new Box<int>(7);\n"
        "    b.Ping<int>(c, 3);\n"
        "    Console.WriteLine(c._n.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n"), "1\n");
}

// The count must track the number of CALLS, not saturate at one: two calls run
// the body exactly twice.  This is the row that distinguishes the fix from the
// opposite error — suppressing the fall-through for a path that genuinely
// declined, which would drop the call and print "0\n".
// Without the fix: "4\n".
BEZEL_TEST(generic_arrow_unit_body_runs_once_3497, two_calls_run_body_twice) {
    BEZEL_EXPECT_EQ(out(
        "class C\n"
        "  public static method Fwd<T>(c: Counter, x: T) => c.Bump();\n"
        "  public static method Main(): int\n"
        "    using var c := new Counter();\n"
        "    C.Fwd<int>(c, 1);\n"
        "    C.Fwd<int>(c, 2);\n"
        "    Console.WriteLine(c._n.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n"), "2\n");
}

// #3431-style counting probe: the fix changes WHETHER the compiled call is
// emitted, so it must not change how many times the ARGUMENT expressions are
// evaluated.  `P` and `Q` count their own evaluations; each must be 1, and the
// body must still be 1.  Double-evaluation is this family's signature
// regression, and an argument evaluated twice would be invisible to every other
// row here (they pass only already-evaluated locals).
BEZEL_TEST(generic_arrow_unit_body_runs_once_3497, arguments_evaluated_once) {
    BEZEL_EXPECT_EQ(out(
        "class Log\n"
        "  public _n: int := 0;\n"
        "  public _p: int := 0;\n"
        "  public _q: int := 0;\n"
        "  public method Bump() _n := _n + 1; end Bump;\n"
        "  public method P(): int _p := _p + 1; return 1; end P;\n"
        "  public method Q(): int _q := _q + 1; return 2; end Q;\n"
        "end Log;\n"
        "class C\n"
        "  public static method Fwd<T>(lg: Log, a: int, x: T) => lg.Bump();\n"
        "  public static method Main(): int\n"
        "    using var lg := new Log();\n"
        "    C.Fwd<int>(lg, lg.P(), lg.Q());\n"
        "    Console.WriteLine(lg._n.ToString());\n"
        "    Console.WriteLine(lg._p.ToString());\n"
        "    Console.WriteLine(lg._q.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n"), "1\n1\n1\n");
}

// The same probe with a RECEIVER expression that counts: `Get()` is evaluated
// once, the argument once, the body once.  The `parentIsGeneric` site is the
// one with a receiver, so this is where a re-evaluated receiver would show.
BEZEL_TEST(generic_arrow_unit_body_runs_once_3497, receiver_evaluated_once) {
    BEZEL_EXPECT_EQ(out(
        "class Log\n"
        "  public _n: int := 0;\n"
        "  public _i: int := 0;\n"
        "  public _p: int := 0;\n"
        "  public method Bump() _n := _n + 1; end Bump;\n"
        "  public method Recv() _i := _i + 1; end Recv;\n"
        "  public method P(): int _p := _p + 1; return 1; end P;\n"
        "end Log;\n"
        "class Box<T>\n"
        "  public _v: T;\n"
        "  public _log: Log;\n"
        "  public constructor(v: T, l: Log) _v := v; _log := l; end constructor;\n"
        "  public method Ping(a: int) => _log.Bump();\n"
        "end Box;\n"
        "class Holder\n"
        "  public owned _b: Box<int>;\n"
        "  public _log: Log;\n"
        "  public constructor(b: owned Box<int>, l: Log) _b := b; _log := l; end constructor;\n"
        "  public method Get(): Box<int> _log.Recv(); return _b; end Get;\n"
        "end Holder;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var lg := new Log();\n"
        "    using var h := new Holder(new Box<int>(7, lg), lg);\n"
        "    h.Get().Ping(lg.P());\n"
        "    Console.WriteLine(lg._n.ToString());\n"
        "    Console.WriteLine(lg._i.ToString());\n"
        "    Console.WriteLine(lg._p.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n"), "1\n1\n1\n");
}

// The defect COMPOSES. A unit arrow body whose expression is itself a call to a
// unit-arrow generic method is inlined recursively, and each level's
// fall-through multiplies the one below it: `Bump` ran FOUR times for one call
// to `Outer`. Measured directly on the pre-fix compiler (`bezelc -o` on the same
// program, then run): "4\n" without the fix, "1\n" with it. This is also the
// shape where the #2491 arrow-body machinery is live — `Outer`'s own body is the
// call that now lowers to no value — so it pins that the fix does not resurrect
// the degrade note #2963/#3434 exist to suppress (the lit track's
// arrow_body_declared_unit_return_no_degrade_note_3434.bzl is the diagnostic
// oracle for that half; this row is the runtime one).
BEZEL_TEST(generic_arrow_unit_body_runs_once_3497, nested_unit_arrow_composes) {
    BEZEL_EXPECT_EQ(out(
        "class C\n"
        "  public static method Fwd<T>(c: Counter, x: T) => c.Bump();\n"
        "  public static method Outer<T>(c: Counter, x: T): () => C.Fwd<T>(c, x);\n"
        "  public static method Main(): int\n"
        "    using var c := new Counter();\n"
        "    C.Outer<int>(c, 1);\n"
        "    Console.WriteLine(c._n.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n"), "1\n");
}

// ── Controls: unchanged by the fix, measured on BOTH compilers ───────────────

// The non-generic spelling of the same declaration never enters the inline
// path (no active substitution), so it always ran its body once.  It is the
// baseline the generic rows above must now match.
BEZEL_TEST(generic_arrow_unit_body_runs_once_3497, control_non_generic_arrow) {
    BEZEL_EXPECT_EQ(out(
        "class C\n"
        "  public static method Fwd(c: Counter, x: int) => c.Bump();\n"
        "  public static method Main(): int\n"
        "    using var c := new Counter();\n"
        "    C.Fwd(c, 1);\n"
        "    Console.WriteLine(c._n.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n"), "1\n");
}

// A VALUE-returning generic arrow body returns a non-null `r`, so its call site
// always returned and never doubled.  The row also pins that the fix did not
// turn a returned value into a dropped one: both the count and the returned
// value are printed.
BEZEL_TEST(generic_arrow_unit_body_runs_once_3497, control_generic_value_arrow) {
    BEZEL_EXPECT_EQ(out(
        "class C\n"
        "  public static method Fwd<T>(c: Counter, x: T): int => c.BumpGet();\n"
        "  public static method Main(): int\n"
        "    using var c := new Counter();\n"
        "    var v := C.Fwd<int>(c, 1);\n"
        "    Console.WriteLine(c._n.ToString());\n"
        "    Console.WriteLine(v.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n"), "1\n1\n");
}

// A unit-returning generic BLOCK body goes through emitGenericSpecialization,
// which hands back the void-typed but NON-NULL `CallInst` — the codebase's
// existing "handled" sentinel.  It never doubled, and must not start.
BEZEL_TEST(generic_arrow_unit_body_runs_once_3497, control_generic_block_body) {
    BEZEL_EXPECT_EQ(out(
        "class C\n"
        "  public static method Fwd<T>(c: Counter, x: T) c.Bump(); end Fwd;\n"
        "  public static method Main(): int\n"
        "    using var c := new Counter();\n"
        "    C.Fwd<int>(c, 1);\n"
        "    Console.WriteLine(c._n.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n"), "1\n");
}
