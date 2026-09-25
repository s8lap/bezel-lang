// narrow #4791 codegen half (§22.5 "Optional parameters and named arguments" /
// §12.8 / §22.8, v0.307): a `base(...)` / `this(...)` constructor chain call
// whose only argument is NAMED and skips a leading defaulted parameter
// silently zero-filled the object. This is independent of #4790/#4779 —
// no constructor has the chain call's ARITY, so `ctorForArgs`'s equal-arity
// arms are never reached at all.
//
// Root cause: `Emitter::emitChainCall` (src/codegen/LlvmBackend_Setup.cpp)
// used the general resolver `ctorForArgs` only when every chain argument was
// POSITIONAL; a chain call with any named argument instead used
// `ctorByArity(targetCls, call->args.size(), ...)` — arity-count matching
// with NO awareness of names or defaults. `base(y := n)` against
// `constructor(x: int := 1, y: int := 0)` has a call arity of 1, which
// matches no constructor's PARAMETER COUNT (the only constructor has arity
// 2), so `ctorByArity` returned null, the chain fell through to the §22.10
// primary-constructor arm (wrong — the target is not a primary-ctor class),
// and the object's fields stayed calloc-zero.
//
// The fix removes the arity-only fallback and always calls `ctorForArgs`,
// which performs the §22.5 binding-with-defaults `ctorByArity` cannot: bind
// `y := n` by name, admit the omitted `x` because it has a default, and
// return the constructor whose OTHER parameter also has a default.
//
// Found in §17 review of #4779 (0ef817e2). Repros are the issue's own probe
// files verbatim (/var/tmp/bezel-scratch/r20/review-4779/
// pF3_base_only_nosibling.bzl, pF4_base_only_sibling.bzl,
// pH_this_chain_named.bzl). The related SEMA false self-chain reject
// (`this(y := 42)` inside a bare `constructor(s: string)`) is explicitly
// OUT OF SCOPE for this commit (owned by a different lane) — `pH` below uses
// `constructor(s: string, t: string)`, a shape sema already accepts, so this
// test exercises the codegen defect in isolation.
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// pF3_base_only_nosibling.bzl, plus the issue's own controls run against
// sibling classes with the SAME base `base(...)` shape called differently:
// K1 is the broken repro; K2/K3/K4 are the issue's stated controls
// (`base(n, 3)` -> XY6,3, `base(x := n, y := 3)` -> XY6,3, `base(n)` ->
// XY6,0) — all already worked pre-fix (no named arg, or every parameter
// named) and must keep working post-fix.
BEZEL_TEST(codegen_ctor_chain_named_arg_skip_default_4791,
           base_chain_named_skip_leading_default_no_sibling_spec_22_5) {
    auto r = runAot(
        "module M; "
        "public class B "
        "  public readonly property Tag: string; "
        "  public constructor(x: int := 1, y: int := 0) "
        "    Tag := $\"XY{x},{y}\"; "
        "  end constructor; "
        "end B; "
        "public class K1 : B "
        "  public constructor(n: int) "
        "    base(y := n); "
        "  end constructor; "
        "end K1; "
        "public class K2 : B "
        "  public constructor(n: int) "
        "    base(n, 3); "
        "  end constructor; "
        "end K2; "
        "public class K3 : B "
        "  public constructor(n: int) "
        "    base(x := n, y := 3); "
        "  end constructor; "
        "end K3; "
        "public class K4 : B "
        "  public constructor(n: int) "
        "    base(n); "
        "  end constructor; "
        "end K4; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new K1(6).Tag); "
        "    Console.WriteLine(new K2(6).Tag); "
        "    Console.WriteLine(new K3(6).Tag); "
        "    Console.WriteLine(new K4(6).Tag); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "XY1,6\nXY6,3\nXY6,3\nXY6,0\n");
}

// pF4_base_only_sibling.bzl: `B` now also has a same-ARITY sibling
// `(z: int)`. Pre-fix this was the sharper failure mode: `ctorByArity(B, 1,
// ...)` found `(z: int)` (arity 1, ignoring the argument's NAME entirely)
// and bound the chain's argument to it POSITIONALLY — `base(y := n)` was
// silently reinterpreted as `base(n)` against `(z: int)`, giving `Tag =
// "Z6"` instead of a zero-fill. `ctorForArgs` correctly rejects `(z: int)`
// (the name `y` does not bind any of its parameters) and falls through to
// the defaulted `(x, y)` constructor.
BEZEL_TEST(codegen_ctor_chain_named_arg_skip_default_4791,
           base_chain_named_skip_leading_default_same_arity_sibling_spec_22_5_22_6) {
    auto r = runAot(
        "module M; "
        "public class B "
        "  public readonly property Tag: string; "
        "  public constructor(x: int := 1, y: int := 0) "
        "    Tag := $\"XY{x},{y}\"; "
        "  end constructor; "
        "  public constructor(z: int) "
        "    Tag := $\"Z{z}\"; "
        "  end constructor; "
        "end B; "
        "public class K: B "
        "  public constructor(n: int) "
        "    base(y := n); "
        "  end constructor; "
        "end K; "
        "public class Program "
        "  public static method Main(): int "
        "    var k := new K(6); "
        "    Console.WriteLine(k.Tag); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "XY1,6\n");
}

// pH_this_chain_named.bzl: a `this(...)` self-chain (not `base(...)`),
// deliberately using `constructor(s: string, t: string)` — a TWO-parameter
// convenience constructor, which sema already accepts as the `this(y :=
// 42)` chain's enclosing constructor. (The issue's related sema defect is a
// bare one-parameter `constructor(s: string)`; that shape is explicitly out
// of scope here — a different lane owns it.) `exclude` (§22.10 "no
// self-chain") still prevents `ctorForArgs` from selecting the `(s, t)`
// constructor itself; the defaulted `(x, y)` constructor is the only other
// candidate and both binds `y` by name and admits `x`'s default.
BEZEL_TEST(codegen_ctor_chain_named_arg_skip_default_4791,
           this_chain_named_skip_leading_default_spec_22_5_12_8) {
    auto r = runAot(
        "module M; "
        "public class J "
        "  public readonly property Tag: string; "
        "  public constructor(x: int := 1, y: int := 0) "
        "    Tag := $\"XY{x},{y}\"; "
        "  end constructor; "
        "  public constructor(s: string, t: string) "
        "    this(y := 42); "
        "  end constructor; "
        "end J; "
        "public class Program "
        "  public static method Main(): int "
        "    var j := new J(\"s\", \"t\"); "
        "    Console.WriteLine(j.Tag); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "XY1,42\n");
}

// Loop-amplified (50 iterations): the chain-call target is resolved once at
// compile time for this call site, so a wrong (or absent) target produces a
// wrong SUM across every iteration rather than a possibly-coincidentally-
// right one-shot value. Pre-fix every `k.V` read the calloc-zero default (0);
// post-fix `V = x * 100 + y` with `x` defaulted to 1 and `y := i`.
BEZEL_TEST(codegen_ctor_chain_named_arg_skip_default_4791,
           loop_50_iterations_spec_22_5) {
    auto r = runAot(
        "module M; "
        "public class B "
        "  public readonly property V: int; "
        "  public constructor(x: int := 1, y: int := 0) "
        "    V := x * 100 + y; "
        "  end constructor; "
        "end B; "
        "public class K : B "
        "  public constructor(n: int) "
        "    base(y := n); "
        "  end constructor; "
        "end K; "
        "public class Program "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var i in 0..<50 do "
        "      var k := new K(i); "
        "      s := s + k.V; "
        "    end; "
        "    Console.WriteLine(s.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // V = 1 * 100 + i = 100 + i for each i in 0..49.
    // sum = 50 * 100 + sum(0..49) = 5000 + 1225 = 6225.
    BEZEL_EXPECT_EQ(r.stdoutText, "6225\n");
}

// End-to-end (§17 review of #4779/#4791, non-author reviewer's repro): a
// `this(...)` chain call whose sole argument is named and skips a leading
// defaulted parameter (`this(y := s.Length)` against a sibling `(x: int :=
// 1, y: int := 0)`), exercised alongside a same-arity DIRECT call
// (`new J(y := 9)`, no chain involved at all — plain §22.5 named-arg
// binding on the constructor overload set) and the loop-amplified form, all
// in one program so the three call shapes are proven consistent with each
// other rather than merely each self-consistent.
//   new J("abcd")  -> constructor(s: string) -> this(y := 4)  -> "XY1,4"
//   new J(y := 9)  -> constructor(x, y)       directly         -> "XY1,9"
//   50x new J("ab").Tag.Length -> this(y := 2) -> "XY1,2" (len 5) -> 250
BEZEL_TEST(codegen_ctor_chain_named_arg_skip_default_4791,
           this_chain_named_bare_one_param_ctor_end_to_end_spec_22_5_12_8) {
    auto r = runAot(
        "module M; "
        "public class J "
        "  public readonly property Tag: string; "
        "  public constructor(x: int := 1, y: int := 0) "
        "    Tag := $\"XY{x},{y}\"; "
        "  end constructor; "
        "  public constructor(s: string) "
        "    this(y := s.Length); "
        "  end constructor; "
        "end J; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new J(\"abcd\").Tag); "
        "    Console.WriteLine(new J(y := 9).Tag); "
        "    var s := 0; "
        "    for var i in 0..<50 do "
        "      s := s + new J(\"ab\").Tag.Length; "
        "    end; "
        "    Console.WriteLine(s.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "XY1,4\nXY1,9\n250\n");
}
