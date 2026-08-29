// fix #3574 (§11.6 "Nested types" / §12.8 / §22.10 / §30.1 / §12.1, v0.286):
// the nesting-DEPTH ladder for the module-type walk that thirteen
// `Resolver::check*` passes share.
//
// §11.6, verbatim: "Nested types may themselves contain nested types." — with
// no depth bound. Every one of those passes used to hand-copy a walk that
// visited a module child and then exactly ONE level of its NestedTypeSymbol
// members, so each rule it enforces silently stopped applying from nesting
// depth 2 downwards. Measured on 907ce0f, the same class body at three depths:
//
//   depth 0 (module level)          1 diagnostic
//   depth 1 (`Outer.Probe`)         1 diagnostic
//   depth 2 (`Outer.Mid.Probe`)     0 diagnostics   <- the defect
//
// For §22.10 that escape is memory-unsafe, not merely permissive: the `new`
// site pairs arguments with primary-constructor field slots by POSITION, so a
// convenience constructor that never chains leaves an `int` sitting in a
// `string` slot. Issue 3574's repro compiled clean and SIGSEGVed (exit 139).
//
// Each row below is a LADDER over depths 0..3 rather than a single deep case,
// so the test pins the walk (a rule that fires at depth 0 and 1 must fire at 2
// and 3) rather than one caller. Four different callers are covered — §22.10 +
// §12.8 (`checkConstructorChains`), §30.1 (`checkAsyncModifiers`) and §12.1
// (`checkStaticNotVirtual`) — so the assertion is about the shared walker.
// The last row is the false-reject guard: a LEGAL type at depth 3 must stay
// clean, since widening a check's reach is only sound if it admits what the
// spec admits.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/TypeResolution.hpp"

#include <string>

namespace {

bezel::test::SemaResult resolveSrc(const std::string& src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

int countDiags(const bezel::test::SemaResult& r, const char* needle) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(needle) != std::string::npos) ++n;
    }
    return n;
}

// `inner` wrapped in `depth` levels of enclosing classes. depth 0 puts it at
// module level; depth 2 is the first shape the pre-fix walk never reached.
std::string atDepth(int depth, const std::string& inner,
                    const std::string& prelude = "") {
    std::string open, close;
    for (int i = 0; i < depth; ++i) {
        open += "public class W" + std::to_string(i) + " ";
        close = "end W" + std::to_string(i) + "; " + close;
    }
    return "module M; " + prelude + open + inner + " " + close;
}

// §22.10 "Primary constructors": a class with a primary constructor whose
// convenience constructor does not chain via `this(...)`.
const char* kUnchainedConvenienceCtor =
    "public class Probe(n: int, s: string) "
    "  public constructor(v: int, w: int) "
    "  end constructor; "
    "end Probe; ";

// §12.8 "Constructor chaining": two `base(...)` calls, the second out of the
// chaining position — violates both halves of the rule.
const char* kDoubleMisplacedChain =
    "public class Probe: Base "
    "  public constructor(a: int, b: int) "
    "    base(a); "
    "    base(b); "
    "  end constructor; "
    "end Probe; ";
const char* kBasePrelude =
    "class Base "
    "  public _x: int; "
    "  public constructor(x: int) _x := x; end constructor; "
    "end Base; ";

// §30.1 "`async` methods": an `async` method with a non-async return type.
const char* kAsyncNonTaskReturn =
    "public class Probe "
    "  public async method Go(): int "
    "    return 1; "
    "  end Go; "
    "end Probe; ";

// §12.1 "Members are non-virtual by default": `static virtual`.
const char* kStaticVirtual =
    "public class Probe "
    "  public static virtual method M(): int "
    "    return 1; "
    "  end M; "
    "end Probe; ";

} // namespace

// §22.10, #3574: the convenience-constructor rule fires at every depth. Before
// the fix depths 2 and 3 reported 0 — and the accepted program SIGSEGVed.
BEZEL_TEST(sema_nested_type_walk_depth_3574,
           primary_ctor_convenience_rule_22_10_fires_at_every_depth) {
    for (int depth = 0; depth <= 3; ++depth) {
        auto r = resolveSrc(atDepth(depth, kUnchainedConvenienceCtor));
        BEZEL_EXPECT_EQ(
            countDiags(r, "must chain to another constructor via"), 1);
    }
}

// §12.8, #3574: both halves of the chaining rule fire at every depth.
BEZEL_TEST(sema_nested_type_walk_depth_3574,
           chain_call_rules_12_8_fire_at_every_depth) {
    for (int depth = 0; depth <= 3; ++depth) {
        auto r = resolveSrc(
            atDepth(depth, kDoubleMisplacedChain, kBasePrelude));
        BEZEL_EXPECT_EQ(
            countDiags(r, "chain call may appear only as the first statement"),
            1);
        BEZEL_EXPECT_EQ(countDiags(r, "at most one `base(...)` or `this(...)`"),
                        1);
    }
}

// §30.1, #3574: a SECOND caller of the shared walk — `checkAsyncModifiers`.
BEZEL_TEST(sema_nested_type_walk_depth_3574,
           async_return_type_rule_30_1_fires_at_every_depth) {
    for (int depth = 0; depth <= 3; ++depth) {
        auto r = resolveSrc(atDepth(depth, kAsyncNonTaskReturn));
        BEZEL_EXPECT_EQ(
            countDiags(r,
                       "must declare a `Task`, `Task<T>`, or `OwningTask<T>` "
                       "return type"),
            1);
    }
}

// §12.1, #3574: a THIRD caller — `checkStaticNotVirtual`.
BEZEL_TEST(sema_nested_type_walk_depth_3574,
           static_not_virtual_rule_12_1_fires_at_every_depth) {
    for (int depth = 0; depth <= 3; ++depth) {
        auto r = resolveSrc(atDepth(depth, kStaticVirtual));
        BEZEL_EXPECT_EQ(
            countDiags(r, "may not be virtual, abstract, or override"), 1);
    }
}

// The false-reject guard (§11.6): a LEGAL type nested three levels deep — a
// conforming `this(...)` chain, an admitted async return type, an
// expression-bodied property — must produce no diagnostic at all. Widening a
// check's reach is sound only if the newly-reached programs it ADMITS are
// exactly the ones the spec admits.
BEZEL_TEST(sema_nested_type_walk_depth_3574,
           legal_type_nested_three_deep_is_not_newly_rejected) {
    auto r = resolveSrc(atDepth(
        3,
        "public class Probe(n: int, s: string) "
        "  public constructor(v: int) "
        "    this(v, \"z\"); "
        "  end constructor; "
        "  public async method Go(): Task<int> "
        "    return 1; "
        "  end Go; "
        "  public property Name: string => s; "
        "end Probe; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
