// §9.1 / §9.3 / §12.8 / §22.10 (issue #4999, v0.307): a combined-form
// record's body constructor may chain with `this(...)` to the record's
// auto-generated PRIMARY constructor.
//
// WHAT THE DEFECT WAS. The #3793 self-chain check
// (`Resolver::checkTypeConstructorChains`, TypeResolution_Checks.cpp) and its
// #4791 named deferral (`deferredNamedThisChain`, SymbolCollection.cpp) judge
// a leading `this(...)` by arity against the type's ConstructorSymbols only.
// Both exempted a CLASS with a §22.10 primary list, whose primary has no
// ConstructorSymbol, but not a RECORD with positional fields, whose §9.1
// primary has none either. So when the calling body constructor's arity
// equalled the chain call's argument count, the chain to the primary was
// read as a self-chain and rejected:
//
//   record R(X: int, Y: int)
//     public constructor(a: bool, b: bool) this(1, 2); end constructor;
//   end R;
//   -> "`this(...)` constructor chain call in `R` chains to the constructor
//       itself; ..." (positional) / "binds no other constructor of `R`: ..."
//       (named, the #4791 deferral)
//
// SPEC. §9.1: a positional record gets "A primary constructor whose
// parameters are the positional fields". §9.3: "Chaining with `this(...)` to
// the primary remains available and is governed by §12.8 unchanged". §12.8:
// "`this(...)` chains to another constructor in the same type"; §22.10
// forbids only chaining to ITSELF. In every accept row below the calling
// constructor's parameter list differs from the primary's by type, so the
// chain can only mean the primary.
//
// The accept rows assert the constructed VALUE, not just acceptance: before
// #4722 (a) the admitted chain wrote no positional field and printed
// `R(X: 0, Y: 0)`. The reject rows pin that the #3793 check still fires on a
// genuine self-chain (a record with NO primary list) and that a record chain
// to the primary is still checked by §22.5 / §5.1.

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

constexpr const char* kSelfChain3793 =
    "chains to the constructor itself; a constructor cannot chain to its own "
    "constructor (§22.10)";

void expectAcceptedPrinting(const JitResult& r, const char* out) {
    if (!r.ran) dumpDiagnostics(r);
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

// Row 1 (the issue's d01): positional chain, calling arity 2 == chain arity 2
// == primary arity 2, no other body constructor.
BEZEL_TEST(record_this_chain_primary_4999,
           accepts_positional_chain_to_primary_spec_9_3) {
    auto r = runAot(
        "module M; "
        "record R(X: int, Y: int) "
        "  public constructor(a: bool, b: bool) this(1, 2); end constructor; "
        "end R; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new R(true, false).ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectAcceptedPrinting(r, "R(X: 1, Y: 2)\n");
}

// Row 2 (the issue's d02): the named form, which the #4791 deferral used to
// reject ("binds no other constructor of `R`"). §22.5 binds `Y := 2` and
// `X := s.Length` by name against the primary's positional fields.
BEZEL_TEST(record_this_chain_primary_4999,
           accepts_named_chain_to_primary_spec_9_3_22_5) {
    auto r = runAot(
        "module M; "
        "record R(X: int, Y: int) "
        "  public constructor(s: string, t: string) "
        "    this(Y := 2, X := s.Length); "
        "  end constructor; "
        "end R; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new R(\"abc\", \"d\").ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectAcceptedPrinting(r, "R(X: 3, Y: 2)\n");
}

// Row 3 (the issue's "other shapes"): a body constructor with a defaulted
// trailing parameter, chaining with its own arity.
BEZEL_TEST(record_this_chain_primary_4999,
           accepts_defaulted_ctor_chain_to_primary_spec_9_3_22_5) {
    auto r = runAot(
        "module M; "
        "record R(X: int, Y: int) "
        "  public constructor(s: string, t: int := 9) "
        "    this(s.Length, t); "
        "  end constructor; "
        "end R; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new R(\"abcd\").ToString()); "
        "    Console.WriteLine(new R(\"ab\", 5).ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectAcceptedPrinting(r, "R(X: 4, Y: 9)\nR(X: 2, Y: 5)\n");
}

// Row 4 (the issue's d03 control): the class twin was already accepted; it
// must stay accepted with the same values.
BEZEL_TEST(record_this_chain_primary_4999,
           control_class_with_primary_list_spec_22_10) {
    auto r = runAot(
        "module M; "
        "class C(x: int, y: int) "
        "  public constructor(a: bool, b: bool) this(1, 2); end constructor; "
        "  public method Show(): string => x.ToString() + \",\" + y.ToString(); "
        "end C; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new C(true, false).Show()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectAcceptedPrinting(r, "1,2\n");
}

// Row 5: a genuine self-chain in a record WITHOUT a primary list. The only
// constructor of `Q` is the one containing the chain, so `this(n)` can only
// bind itself (§22.10) — the #3793 diagnostic must still fire for records.
BEZEL_TEST(record_this_chain_primary_4999,
           rejects_self_chain_in_body_record_spec_22_10) {
    auto r = runAot(
        "module M; "
        "record Q "
        "  public readonly property N: int; "
        "  public constructor(n: int) this(n); end constructor; "
        "end Q; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new Q(3).N); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRejected(r, kSelfChain3793);
}

// Row 6: a record chain to the primary is still bound by §22.5 — a name no
// positional field carries is rejected, now by the primary-chain binder
// rather than by the self-chain heuristic.
BEZEL_TEST(record_this_chain_primary_4999,
           rejects_named_chain_naming_no_field_spec_22_5) {
    auto r = runAot(
        "module M; "
        "record R(X: int, Y: int) "
        "  public constructor(a: bool, b: bool) this(X := 1, Z := 2); "
        "  end constructor; "
        "end R; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new R(true, true).ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRejected(r, "has no parameter named `Z` (§22.5)");
}

// Row 7: ... and by §5.1 — a `string` bound to `X: int` is rejected.
BEZEL_TEST(record_this_chain_primary_4999,
           rejects_chain_arg_not_convertible_spec_5_1) {
    auto r = runAot(
        "module M; "
        "record R(X: int, Y: int) "
        "  public constructor(a: bool) this(\"bad\", 1); end constructor; "
        "end R; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new R(true).ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRejected(r,
                   "chain to the primary constructor of `R` has a type that "
                   "is not implicitly convertible to the target type (§5.1)");
}
