// §5.1 / §12.8 / §22.5 / §22.6 (issue #4923, v0.307): a named `this(...)`
// chain call that `checkDeferredNamedThisChain` decides (the deferred class
// #4791 defined — a chain whose argument count matches only ITS OWN
// constructor but which names a parameter that constructor does not have)
// must apply §5.1 constant narrowing to its arguments before judging
// applicability, exactly like every other construction site.
//
// WHAT THE DEFECT WAS. Fix #4854 (ctor_chain_applicability_4854_test.cpp)
// taught `checkChainCallApplicability` to narrow an int-constant argument via
// `narrowIntConstantToTarget` before `classifyArg`/`resolveOverload` judge
// it — both the single-candidate (`checkArgsAgainstParams`) and
// multi-candidate (`resolveOverload`) arms. `checkDeferredNamedThisChain`'s
// OWN applicability loop (condition (2) in its header comment) never got
// that treatment: its comment states plainly that "§5.1 constant narrowing
// (`narrowIntConstantToTarget`) is not attempted" there, reasoning that the
// omission only ever keeps an already-rejected program rejected. That
// reasoning is wrong for a program whose named candidate binds an in-range
// integer constant to a narrower-integer formal (`byte`/`short`/etc.): the
// argument's un-narrowed `int` static type against e.g. `short` is judged a
// NARROWING by `classifyArg` (not in §5.1's widening table) and the whole
// candidate is marked NotApplicable, so the constructor call is rejected
// with "binds no other constructor" even though the constant fits the
// target and §5.1's constant-narrowing carve-out admits it.
//
// MEASURED on origin/main e9eac9cc (pre-fix): row 1 below — `this(y := 42,
// x := 1)` naming a sibling's `y: short` with an in-range constant — was
// REJECTED with exactly that diagnostic. Row 2 (out-of-range, `y := 70000`)
// was correctly rejected for the same reason both before and after the fix,
// pinning that the fix does not admit anything §5.1 does not allow. Row 3 is
// a `byte`-target sibling analog of row 1. Row 4 is a control: the
// undeferred (arity-disambiguated) path is unaffected.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

void dumpDiagnostics(const JitResult& r) {
    std::fprintf(stderr, "  diagnostics (count=%zu):\n",
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%s]\n", d.message.c_str());
}

void expectAccepted(const JitResult& r, const char* out) {
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

void expectRejected(const JitResult& r, const char* diag) {
    if (!hasError(r, diag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, diag));
    BEZEL_EXPECT_FALSE(r.ran);
}

}  // namespace

// Row 1: `this(y := 42, x := 1)` inside `constructor(s: string, y: int)`
// (arity 2, matching only itself — no other ctor has arity 2 — and `x` names
// no parameter of the containing constructor, so `checkDeferredNamedThisChain`
// decides it). It names-binds the sibling `constructor(x: int := 1,
// y: short := 0, z: int := 0)`; `y := 42` is in short range. Must ACCEPT and
// construct with x=1, y=42.
BEZEL_TEST(ctor_chain_named_const_narrow_4923,
           deferred_named_chain_in_range_short_accepted_spec_5_1) {
    auto r = runAot(R"BZ(
module M;
class C
  public S: int;
  public Y: int;
  public constructor(s: string, y: int) this(y := 42, x := 1); end constructor;
  public constructor(x: int := 1, y: short := 0, z: int := 0)
    S := x;
    Y := y;
  end constructor;
  public static method Main(): int
    var c := new C("a", 5);
    Console.WriteLine(c.S.ToString());
    Console.WriteLine(c.Y.ToString());
    return 0;
  end Main;
end C;
)BZ");
    expectAccepted(r, "1\n42\n");
}

// Row 2: same shape, but `y := 70000` is OUT of short range (max 32767).
// Must REJECT both before and after the fix — pins that narrowing does not
// widen acceptance beyond what §5.1 allows.
BEZEL_TEST(ctor_chain_named_const_narrow_4923,
           deferred_named_chain_out_of_range_short_rejected_spec_5_1) {
    auto r = runAot(R"BZ(
module M;
class C
  public S: int;
  public Y: int;
  public constructor(s: string, y: int) this(y := 70000, x := 1); end constructor;
  public constructor(x: int := 1, y: short := 0, z: int := 0)
    S := x;
    Y := y;
  end constructor;
  public static method Main(): int
    var c := new C("a", 5);
    return 0;
  end Main;
end C;
)BZ");
    expectRejected(r, "binds no other constructor");
}

// Row 3: `byte`-target analog of row 1 — `this(y := 200, x := 1)` naming a
// sibling's `y: byte` (0..255). In range. Must ACCEPT.
BEZEL_TEST(ctor_chain_named_const_narrow_4923,
           deferred_named_chain_in_range_byte_accepted_spec_5_1) {
    auto r = runAot(R"BZ(
module M;
class C
  public S: int;
  public Y: int;
  public constructor(s: string, y: int) this(y := 200, x := 1); end constructor;
  public constructor(x: int := 1, y: byte := 0, z: int := 0)
    S := x;
    Y := y;
  end constructor;
  public static method Main(): int
    var c := new C("a", 5);
    Console.WriteLine(c.S.ToString());
    Console.WriteLine(c.Y.ToString());
    return 0;
  end Main;
end C;
)BZ");
    expectAccepted(r, "1\n200\n");
}

// Row 4: control — a `base(...)` chain resolved through
// `checkChainCallApplicability`'s MULTI-candidate `resolveOverload` arm
// (already narrowing-aware since fix #4854, not the `checkDeferredNamedThis
// Chain` path this test targets) still picks the narrower-integer candidate
// for an in-range constant.
BEZEL_TEST(ctor_chain_named_const_narrow_4923,
           control_multi_candidate_chain_narrowing_still_works) {
    auto r = runAot(R"BZ(
module M;
class Base
  public V: int;
  public constructor(v: byte)
    V := v;
  end constructor;
  public constructor(v: string)
    V := -1;
  end constructor;
end Base;
class Child : Base
  public constructor() base(200); end constructor;
end Child;
class C
  public static method Main(): int
    var c := new Child();
    Console.WriteLine(c.V.ToString());
    return 0;
  end Main;
end C;
)BZ");
    expectAccepted(r, "200\n");
}
