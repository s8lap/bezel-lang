// Codegen + sema tests for issue #1594 — the remaining §22.10 (v0.180)
// "owned primary-constructor parameter" mechanism pieces left after #1543.
// #1543 fixed the core unsound leak for a CONCRETE class/interface-typed
// captured `owned` primary-ctor parameter (disposed via its vtable Dispose).
// This file covers the pieces #1594 tracked as remaining, restricted to the
// ones fixed here (none of these was ever a double-free — each was, at
// worst, the pre-v0.180 leak or a missing diagnostic):
//
//   1. Shared<T>/Weak<T>, owning-collection, Subscription, and builtin-
//      exception-typed captured `owned` primary-ctor parameters — the other
//      owned-field disposal predicates (isOwnedSharedWeakField,
//      ownedCollectionFieldType, isOwnedSubscriptionField,
//      isOwnedBuiltinExceptionField) gated on `hasOwnedModifier(fs)`, which
//      reads `fs->modifiers` (empty for a posParam) instead of
//      `posParam->modifier`, so a captured owned param of these types leaked.
//   2. `ref`/`out`/`scoped` primary-ctor parameter rejection (§22.10: the
//      grammar over-admits them through the shared Param production; they
//      must be rejected semantically) — previously compiled with zero
//      diagnostics.
//   3. The §21.9/§17.13 bare-discard leak check's non-trivial-dispose
//      classification walked only "does a user destructor exist in the
//      chain," not "does the class carry an owned storage member" (declared,
//      or a captured owned primary-ctor parameter) — so a bare-statement
//      `new Holder(...);` discard of such a class compiled with only the
//      generic "unused result" warning instead of the required hard error,
//      and the bare-statement codegen path (no `_ :=`, no `using var`) never
//      calls Dispose() at all, genuinely leaking the owned member.
//
// Remaining out of scope here (tracked in #1594, unchanged by this file):
// generic type-parameter-typed captured owned params (needs the
// specialized-disposer / ownedTypeParamConcreteType-style substitution
// resolution #1543 did not extend — a disposer-specialization change,
// deferred as the highest-risk remaining piece) and the body-reference-plus-
// consuming-initializer compile error (needs precise consume-vs-read
// detection in the ownership/flow layer).

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"      // runAot — AOT oracle (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

using bezel::test::runAot;
using bezel::test::JitResult;

namespace {

// AOT oracle: these programs exercise owned-primary-ctor-param DISPOSAL and the
// §21.9/§22.10 diagnostics, all produced by the ownership pass the JIT skipped.
// The Res._n counter (ctor leaves it, destructor bumps it) is the dispose-
// exactly-once oracle: `_n == k` means the captured owned value was disposed k
// times. A reject surfaces as ran=false with the driver diagnostics in
// `r.diagnostics` (runAot carries the rejection text there).
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// §17.13 explicit-discard (`_ := expr`) disposal is driven by the OWNERSHIP
// pass's disposeOwnedValue classification (src/ownership/Ownership.cpp), which
// runJit does NOT run (it is a surface-marker audit only — see
// codegen_support.hpp::runJitInProcess). So a `_ :=` discard's disposal can
// only be observed through the real Driver pipeline, mirroring
// move_result_owned_724_test.cpp's compileAndRun helper.
namespace fs = std::filesystem;
int compileAndRun(const std::string& program) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_1594");
    std::string src = (dir / (stem + ".bz")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << program; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

bool diagContains(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// A disposable class whose destructor bumps a static counter — the same
// fixture #1543's test file uses.
const char* kRes =
    "class Res "
    "  public static _n: int := 0; "
    "  private _id: int; "
    "  public constructor(id: int) _id := id; end constructor; "
    "  public method Id(): int => _id; "
    "  destructor _n := _n + 1; end destructor; "
    "end Res; ";

} // namespace

// ===========================================================================
// Piece 1: Shared<T>/Weak<T>, owning-collection, Subscription, and builtin-
// exception-typed captured `owned` primary-ctor parameters.
// ===========================================================================

// §22.10 (v0.180) / §28.13 / stdlib §2.16 (#1594): an `owned` primary-ctor
// parameter of type Shared<T>, referenced in a body position, is captured and
// released via bz_shared_release when the LAST handle disposes (counter → 1).
BEZEL_TEST(codegen_owned_primary_ctor_param_1594, shared_t_captured_param_disposed_22_10) {
    // per §22.10 (v0.180) / §28.13
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kRes +
        "class Holder(owned s: Shared<Res>) "
        "  public method Use(): int => s.Get().Id(); "
        "end Holder; "
        "class C public static method Main(): int "
        "  do using var h := new Holder(new Shared<Res>(new Res(7))); end; "
        "  return Res._n; "
        "end Main; end C;"), 1);
}

// §22.10 (v0.180) / stdlib §4.5 (#1594): an `owned` primary-ctor parameter of
// a concrete owning-collection type (OwningArray<Res>), referenced in a body
// position, is captured and disposed with its elements (3 elements → counter
// → 3). Built via a fresh-producer static factory so the call-site `owned`
// marker (§22.3) is not implicated — the marker-enforcement gap for named
// owned locals passed to primary-ctor positional args is a separate,
// pre-existing bug (filed #1627), unrelated to #1594.
BEZEL_TEST(codegen_owned_primary_ctor_param_1594, owning_collection_captured_param_disposed_22_10) {
    // per §22.10 (v0.180) / stdlib §4.5
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kRes +
        "class Holder(owned arr: OwningArray<Res>) "
        "  public method Count(): int => arr.Count; "
        "end Holder; "
        "class C "
        "  public static method Build(): owned OwningArray<Res> "
        "    var a := new OwningArray<Res>(); "
        "    a.Add(new Res(1)); a.Add(new Res(2)); a.Add(new Res(3)); "
        "    return a; "
        "  end Build; "
        "  public static method Main(): int "
        "    do using var h := new Holder(Build()); end; "
        "    return Res._n; "
        "  end Main; "
        "end C;"), 3);
}

// §22.10 (v0.180) / stdlib §2.11 (#1594): an `owned` primary-ctor parameter of
// type Subscription, referenced in a body position, is captured and disposed
// via bz_subscription_dispose. Subscription carries no observable destructor
// counter, so this asserts (a) the program runs to completion (no crash — a
// double-free would abort the whole JIT process, not just fail an
// assertion), and (b) the generated Dispose is idempotent (calling it twice
// is a no-op, §28.2), rather than the double-release that would result if
// the capture predicate matched some OTHER disposal path too.
//
// The body reference is a BORROW read of `sub` (`var x := sub;`) — enough to
// trigger the §22.10 capture — not an explicit `sub.Dispose()`: a captured owned
// field may be disposed only through `this` (the synthesized Holder.Dispose),
// never through a field read (§28.6 "Who may call Dispose()", v0.184/#1653). The
// disposal under test is Holder's synthesized field disposal, driven by the two
// `h.Dispose()` calls below.
BEZEL_TEST(codegen_owned_primary_ctor_param_1594, subscription_captured_param_disposed_22_10) {
    // per §22.10 (v0.180) / stdlib §2.11 / §28.6
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Holder(owned sub: Subscription) "
        "  public method Keep(): bool var x := sub; return x = null; end Keep; "
        "end Holder; "
        "class C public static method Main(): int "
        "  do "
        "    using var cts := new CancellationTokenSource(); "
        "    var h := new Holder(cts.Token.Register(lambda() => ())); "
        "    h.Dispose(); "
        "    h.Dispose(); "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §22.10 (v0.180) / §31.1 (#1594): an `owned` primary-ctor parameter of a
// BUILTIN exception type (Exception itself — no user TypeSymbol), referenced
// in a body position, is captured and disposed through the shared no-op
// exception vtable Dispose (§28.5). Verifies the captured value is readable
// (Message survives) and disposal is idempotent.
BEZEL_TEST(codegen_owned_primary_ctor_param_1594, builtin_exception_captured_param_disposed_22_10) {
    // per §22.10 (v0.180) / §31.1
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Holder(owned e: Exception) "
        "  public method Msg(): string => e.Message; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var h := new Holder(new Exception(\"boom\")); "
        "  var m := h.Msg(); "
        "  h.Dispose(); "
        "  h.Dispose(); "
        "  return m.Length; "
        "end Main; end C;"), 4);
}

// ===========================================================================
// Piece 2: `ref` / `out` / `scoped` primary-ctor parameter rejection.
// ===========================================================================

// §22.10 (v0.160): a primary-ctor parameter list admits plain and `owned`
// parameters only; `ref` is rejected semantically (the grammar over-admits it
// through the shared Param production, Appendix A.4's over-admit-and-reject-
// semantically pattern).
BEZEL_TEST(codegen_owned_primary_ctor_param_1594, ref_primary_ctor_param_rejected_22_10) {
    // per §22.10 (v0.160)
    auto r = runAot(
        "module M; "
        "class Holder(ref x: int) "
        "  public method Get(): int => x; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var v := 5; "
        "  var h := new Holder(ref v); "
        "  return h.Get(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagContains(r, "not permitted on a primary-constructor parameter"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.10"));
}

// §22.10 (v0.160): `out` is likewise rejected on a primary-ctor parameter.
BEZEL_TEST(codegen_owned_primary_ctor_param_1594, out_primary_ctor_param_rejected_22_10) {
    // per §22.10 (v0.160)
    auto r = runAot(
        "module M; "
        "class Holder(out x: int) "
        "  public method Get(): int => 0; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagContains(r, "not permitted on a primary-constructor parameter"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.10"));
}

// §22.10 (v0.180): a `scoped` primary-constructor parameter is rejected in
// this revision — the §28.14 scoped-storage contract needs an explicitly
// declared field.
BEZEL_TEST(codegen_owned_primary_ctor_param_1594, scoped_primary_ctor_param_rejected_22_10) {
    // per §22.10 (v0.180)
    auto r = runAot(
        "module M; "
        "class Res destructor end destructor; end Res; "
        "class Holder(scoped r: Res) "
        "  public method Get(): int => 0; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagContains(r, "not permitted on a primary-constructor parameter"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.10"));
}

// §22.10: a plain (borrow) parameter alongside an `owned` one is unaffected
// by the ref/out/scoped rejection — both compile and run correctly. No
// regression guard for the reject check above.
BEZEL_TEST(codegen_owned_primary_ctor_param_1594, plain_and_owned_params_still_accepted_22_10) {
    // per §22.10
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kRes +
        "class Holder(owned r: Res, tag: int) "
        "  public method Use(): int => tag + r.Id(); "
        "end Holder; "
        "class C public static method Main(): int "
        "  do using var h := new Holder(new Res(3), 4); end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// ===========================================================================
// Piece 3: §21.9/§17.13 non-trivial-dispose classification by owned-storage
// presence, not just user-destructor presence.
// ===========================================================================

// §21.9 / §3.6 (#1594): a bare-statement discard (`new Holder(...);`, no
// `_ :=`, no `using var`) of a class whose ONLY disposal obligation is a
// captured `owned` primary-ctor parameter (no user-written destructor) is now
// correctly rejected as a compile error — previously this compiled with only
// the generic "unused result" warning, and the bare-statement codegen path
// never called Dispose(), genuinely leaking the captured Res.
BEZEL_TEST(codegen_owned_primary_ctor_param_1594, bare_discard_of_captured_owned_param_rejected_21_9) {
    // per §21.9 / §3.6
    auto r = runAot(
        std::string("module M; ") + kRes +
        "class Holder(owned r: Res) "
        "  public method Use(): int => r.Id(); "
        "end Holder; "
        "class C public static method Main(): int "
        "  new Holder(new Res(1)); "
        "  return Res._n; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagContains(r, "§21.9"));
}

// §21.9 / §3.6 (#1594): the SAME classification gap affects a regular
// DECLARED `owned` field with no user destructor (not just the #1594 posParam
// capture case) — a strict superset fix, since both are "non-trivial-dispose"
// per §28.9's derived-property computation.
BEZEL_TEST(codegen_owned_primary_ctor_param_1594, bare_discard_of_declared_owned_field_rejected_21_9) {
    // per §21.9 / §3.6
    auto r = runAot(
        std::string("module M; ") + kRes +
        "class Holder(owned r: Res) "
        "  private owned _s: Res := r; "
        "end Holder; "
        "class C public static method Main(): int "
        "  new Holder(new Res(1)); "
        "  return Res._n; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagContains(r, "§21.9"));
}

// §17.13 (v0.150, #1009) (#1594): the explicit discard form `_ := new
// Holder(...);` is an unbound owned temporary whose end-of-statement disposal
// is classified by the ownership pass (DiscardStmt::disposeOwnedValue,
// src/ownership/Ownership.cpp). That classification runs in the real Driver
// pipeline, NOT in runJit (runJit runs only collect/resolve/typecheck — see
// codegen_support.hpp::runJitInProcess), so this case is driven through
// compileAndRun (the same Driver path move_result_owned_724_test.cpp uses for
// §17.13 `_ :=` disposal). Verifies the piece-3 fix didn't regress the always-
// permitted explicit-discard path: the captured owned Res is disposed exactly
// once (counter → 1).
BEZEL_TEST(codegen_owned_primary_ctor_param_1594, explicit_discard_of_captured_owned_param_still_disposes_17_13) {
    // per §17.13 (v0.150) / §21.9
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string("module M; ") + kRes +
        "class Holder(owned r: Res) "
        "  public method Use(): int => r.Id(); "
        "end Holder; "
        "class C public static method Main(): int "
        "  _ := new Holder(new Res(1)); "
        "  return Res._n; "
        "end Main; end C;"), 1);
}

// §21.9 / §22.12 rule 4 (#1594): a class whose ONLY `owned` member is a
// `static readonly owned` field remains trivial-dispose for THIS check — a
// static owned field is disposed at process-exit order (§22.13), not by the
// per-instance Dispose(), so a bare-statement `new Holder();` must NOT be
// rejected on that basis. Regression guard for the static-field exclusion in
// the piece-3 fix.
BEZEL_TEST(codegen_owned_primary_ctor_param_1594, bare_discard_of_static_owned_only_field_still_accepted_21_9) {
    // per §21.9 / §22.12 rule 4
    auto r = runAot(
        std::string("module M; ") + kRes +
        "class Holder "
        "  private static owned readonly _shared: Res := new Res(1); "
        "  public method Get(): int => 5; "
        "end Holder; "
        "class C public static method Main(): int "
        "  new Holder(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
}

// §21.9: a genuinely trivial-dispose class (no owned member at all, no
// destructor) is unaffected — bare-statement discard compiles cleanly. No
// regression guard for the piece-3 fix's general case.
BEZEL_TEST(codegen_owned_primary_ctor_param_1594, bare_discard_of_trivial_class_still_accepted_21_9) {
    // per §21.9
    auto r = runAot(
        "module M; "
        "class Plain public method Get(): int => 5; end Plain; "
        "class C public static method Main(): int "
        "  new Plain(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
}
