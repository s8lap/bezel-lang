// fix #4824 (§22.5 "Optional parameters and named arguments" / §22.6 /
// §12.8 "Constructor chaining", v0.307): a `base(...)` / `this(...)` chain
// call that no candidate constructor of the target type §22.5-binds is a
// compile error, exactly as the `new` form of the same shape is.
//
// §22.5: "Every parameter that declares no default must be bound … leaving
// one unbound … makes the candidate inapplicable ([§22.6]) … and the error is
// reported only where no candidate survives", at "every construction site in
// the language". Before the fix, sema never asked that question of a chain
// call whose target is an explicit constructor: codegen's selector found no
// target, skipped the chain, and the object stayed calloc-zero (the issue's
// two repros printed an empty line and exited 0).
//
// Rows 1-2 are the issue's repros; rows 3-5 are the sibling shapes named in
// the issue's comments (#4824 unknown name; #4854 `base(z := 1)` and
// `base(1, 2)`); row 6 is a multi-candidate target no candidate binds. Rows
// 7-8 are accept controls that assert the constructed VALUE: a chain one
// candidate binds (defaults filled) is not disturbed.

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

void expectRejected(const JitResult& r, const char* diag) {
    if (!hasError(r, diag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, diag));
    BEZEL_EXPECT_FALSE(r.ran);
}

}  // namespace

// Row 1 — the issue's repro 1: `base(y := n)` leaves the base's required `x`
// unbound.
BEZEL_TEST(ctor_chain_unbound_required_param_4824,
           base_chain_required_param_unbound_rejected_spec_22_5) {
    auto r = runAot(
        "module M;\n"
        "public class B\n"
        "  public readonly property Tag: string;\n"
        "  public constructor(x: int, y: int := 0) Tag := $\"B{x},{y}\"; "
        "end constructor;\n"
        "end B;\n"
        "public class K : B\n"
        "  public constructor(n: int) base(y := n); end constructor;\n"
        "end K;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(new K(6).Tag);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    expectRejected(r,
                   "`base(...)` constructor chain call in `K`: required "
                   "parameter `x` is not supplied (§22.5)");
}

// Row 2 — the issue's repro 2: the same shape as `this(y := 6)` inside
// `constructor(s: string, t: string)`.
BEZEL_TEST(ctor_chain_unbound_required_param_4824,
           this_chain_required_param_unbound_rejected_spec_22_5) {
    auto r = runAot(
        "module M;\n"
        "public class K\n"
        "  public readonly property Tag: string;\n"
        "  public constructor(x: int, y: int := 0) Tag := $\"K{x},{y}\"; "
        "end constructor;\n"
        "  public constructor(s: string, t: string) this(y := 6); "
        "end constructor;\n"
        "end K;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(new K(\"a\", \"b\").Tag);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    expectRejected(r,
                   "`this(...)` constructor chain call in `K`: required "
                   "parameter `x` is not supplied (§22.5)");
}

// Row 3 — #4824's comment: a named argument matching no parameter of any
// target constructor (`this(zzz := 99)`).
BEZEL_TEST(ctor_chain_unbound_required_param_4824,
           this_chain_unknown_name_rejected_spec_22_5) {
    auto r = runAot(
        "module M;\n"
        "public class K\n"
        "  public readonly property Tag: string;\n"
        "  public constructor(x: int := 1) Tag := $\"K{x}\"; end constructor;\n"
        "  public constructor(s: string) this(zzz := 99); end constructor;\n"
        "end K;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(new K(\"a\").Tag);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    expectRejected(r,
                   "`this(...)` constructor chain call in `K` has no "
                   "parameter named `zzz` (§22.5)");
}

// The #4854 Parent/Child program, parameterised on the chain call.
static std::string parentChild(const char* chain) {
    return std::string(
               "module M;\n"
               "class Parent\n"
               "  public readonly property P: int;\n"
               "  public constructor(y: int) P := y; end constructor;\n"
               "end Parent;\n"
               "class Child : Parent\n"
               "  private _s: string;\n"
               "  public constructor(s: string)\n    ") +
           chain +
           "\n    _s := s;\n"
           "  end constructor;\n"
           "  public method S(): string return $\"{_s}/{P}\"; end S;\n"
           "end Child;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var c := new Child(\"hello\");\n"
           "    Console.WriteLine(c.S());\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// Row 4 — #4854: `base(z := 1)`, `z` a parameter of no base constructor
// (printed `hello/0` before the fix).
BEZEL_TEST(ctor_chain_unbound_required_param_4824,
           base_chain_unknown_name_rejected_spec_22_5) {
    auto r = runAot(parentChild("base(z := 1);"));
    expectRejected(r,
                   "`base(...)` constructor chain call in `Child` has no "
                   "parameter named `z` (§22.5)");
}

// Row 5 — #4854: `base(1, 2)` with no arity-2 base constructor.
BEZEL_TEST(ctor_chain_unbound_required_param_4824,
           base_chain_too_many_args_rejected_spec_22_5) {
    auto r = runAot(parentChild("base(1, 2);"));
    expectRejected(r,
                   "`base(...)` constructor chain call in `Child` supplies 2 "
                   "argument(s) but the parameter list has 1 (§22.5)");
}

// Row 6 — an overloaded base where no candidate binds `base(n)`: each
// candidate has a required parameter the call leaves unbound.
BEZEL_TEST(ctor_chain_unbound_required_param_4824,
           base_chain_no_overload_binds_rejected_spec_22_6) {
    auto r = runAot(
        "module M;\n"
        "public class B\n"
        "  public readonly property Tag: string;\n"
        "  public constructor(x: int, y: int) Tag := $\"B{x},{y}\"; "
        "end constructor;\n"
        "  public constructor(s: string, t: string, u: string) Tag := s; "
        "end constructor;\n"
        "end B;\n"
        "public class K : B\n"
        "  public constructor(n: int) base(n); end constructor;\n"
        "end K;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(new K(6).Tag);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    expectRejected(r,
                   "`base(...)` constructor chain call in `K`: no "
                   "constructor of `B` can bind these arguments");
}

// Row 7 — accept control: the repro with the required parameter supplied by
// name; the defaulted `y` is filled.
BEZEL_TEST(ctor_chain_unbound_required_param_4824,
           base_chain_required_param_bound_by_name_accepted_spec_22_5) {
    auto r = runAot(
        "module M;\n"
        "public class B\n"
        "  public readonly property Tag: string;\n"
        "  public constructor(x: int, y: int := 0) Tag := $\"B{x},{y}\"; "
        "end constructor;\n"
        "  public constructor(s: string) Tag := s; end constructor;\n"
        "end B;\n"
        "public class K : B\n"
        "  public constructor(n: int) base(x := n); end constructor;\n"
        "  public constructor(a: string, b: string) base(a); end constructor;\n"
        "end K;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(new K(6).Tag);\n"
        "    Console.WriteLine(new K(\"p\", \"q\").Tag);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    if (!r.ran) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "B6,0\np\n");
}

// Row 8 — accept control: `this(...)` binding a sibling by name with a
// default filled.
BEZEL_TEST(ctor_chain_unbound_required_param_4824,
           this_chain_required_param_bound_accepted_spec_22_5) {
    auto r = runAot(
        "module M;\n"
        "public class K\n"
        "  public readonly property Tag: string;\n"
        "  public constructor(x: int, y: int := 0) Tag := $\"K{x},{y}\"; "
        "end constructor;\n"
        "  public constructor(s: string, t: string) this(y := 6, x := 2); "
        "end constructor;\n"
        "end K;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(new K(\"a\", \"b\").Tag);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    if (!r.ran) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "K2,6\n");
}
