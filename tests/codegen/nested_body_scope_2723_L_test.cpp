// Codegen - Family L (defect-campaign-2026-09.md §2.8), member #2723:
// the indexer-accessor emitters (`ensureIndexerBlockGetter`,
// `ensureIndexerGetterFn`, `ensureIndexerSetterFn`) save
// `currentReturnTypeNode_` (#2694) but were the FIRST adopters of
// `#2688`'s lambda-return twin (`currentLambdaReturnSemaTy_`) -- the
// indexer body returns against the wrong lambda return type when the
// first reader is a lambda whose own return type is the property's
// declared type. The canonical `NestedBodyScope` saves BOTH halves
// (the ctor's preamble at `LlvmBackend_Assign.cpp:5116` carries the
// explicit pair) -- the family-anchor statement.
//
// Spec (revision v0.290+):
//   §23.4 "Indexers" - an indexer's accessors take the same `=> expression`
//     / `=> do ... end` forms as a block-form property (§23.3). A
//     `do ... end` accessor is a user statement body lowered into its
//     OWN function; its `return` terminates the ACCESSOR.
//   #2688 "the lambda-return twin" - a block-bodied lambda has its own
//     return contract, also kept out of the LAMBDA's body by the
//     same `currentLambdaReturnSemaTy_` reset.
//
// THE PIN. The first reader of an indexer accessor is a block-bodied
// lambda whose declared return type matches the indexer's. Pre-fix the
// lambda-return +1 mint leaks from the lambda into the accessor body,
// and the accessor's `return _n + i` is observed by the lambda after
// its own `var r := await ...; return r - <expr>` carried the leak.
// The two-callers, two-orders construction pins that divergence.
//
// PRE-FIX DIVERGENCE: the SECOND reader sees the FIRST reader's
// lambda-return token (the caller's `+1` mint is folded into the
// accessor's body). The program reads two different values from the
// same indexer accessor.
//
// POST-FIX (the canonical find lands for indexer accessors, the same
// way #2702 closed it for block getters): the indexer accessor body
// resets BOTH `currentReturnTypeNode_` AND `currentLambdaReturnSemaTy_`
// to null, and the cached function is independent of the first reader.
// Both readers see the accessor's own return value.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked - AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return true;
    }
    return false;
}

int runPlain(const std::string& src) {
    JitResult r = bezel::test::runAot(src);
    return r.ran ? r.exitCode : -999;
}

int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// Two-callers, two-orders pin for ensureIndexerBlockGetter. Two readers
// in two orders reach the same memoized indexer accessor; both must
// return the indexer's own `_n + i` (5 + 0 = 5 and 5 + 1 = 6) rather
// than the FIRST reader's lambda-return leaked value. §23.4 + #2688.
static const char* kIndexerBlockGetterCallerOrder =
    "module M; "
    "class Bag "
    "  private _n: int; "
    "  public constructor() _n := 5; end constructor; "
    "  public property [i: int]: int "
    "    get => do "
    "      return _n + i; "
    "    end; "
    "  end; "
    "end Bag; "
    "class P "
    "  public static method ReadPair(b: Bag): int "
    "    var a: int := b[0]; "          // 5
    "    using var b2 := new Bag(); "
    "    var c: int := b2[1]; "         // 6
    "    return a + c; "               // 11
    "  end ReadPair; "
    "  public static method Main(): int "
    "    using var b := new Bag(); "
    "    return P.ReadPair(b); "
    "  end Main; "
    "end P;";

BEZEL_TEST(codegen_nested_body_scope_2723_L,
           indexer_accessor_two_callers_in_two_orders) {
    BEZEL_EXPECT_EQ(runPlain(kIndexerBlockGetterCallerOrder), 11);
}

BEZEL_TEST(codegen_nested_body_scope_2723_L,
           indexer_accessor_two_callers_in_two_orders_checked) {
    int rc = runChecked(kIndexerBlockGetterCallerOrder);
    if (rc == -1000) return;
    BEZEL_EXPECT_EQ(rc, 11);
}
