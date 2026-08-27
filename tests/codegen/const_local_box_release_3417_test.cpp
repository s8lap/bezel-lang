// Codegen — issue #3417 (§21.1 runtime `const` locals, §8.6 boxing, §28.9
// runtime-managed types, spec v0.283): a §21.1 runtime `const` local of type
// `Object` never released the box or the object-string payload it bound, while
// the `var` spelling of the identical program was ASan+LSan clean. Measured on
// the pre-fix compiler (`bezelc --sanitize=address`, LeakSanitizer on):
//
//   var n := 41;   const o: Object := n;               ->  24 B / 1 alloc
//   the same inside a 500-iteration loop               ->  12000 B / 500
//   var p := "ab" + z;  const o: Object := p;   x500   ->  18500 B / 1000
//   const p := "ab" + z;  var o: Object := p;   x500   ->  18500 B / 1000
//
// ROOT CAUSE — the same pattern as #2802 / #3375 / #3402 / #3416, in the BOX
// and OBJECT-STRING reclaim families. §21.1 gives a runtime `const` its own AST
// node, `LocalConstDecl`, a sibling of `LocalDecl` with differently-named
// fields, so a walker spelled `dynamic_cast<const ast::LocalDecl*>(s)` matches a
// `var` and silently does nothing for a `const`. Four sites were affected:
//
//   1. `collectBoxReleaseEligible` — the candidate scan never admitted a
//      `const`, so it never entered `boxReleaseEligible_` and `lowerLocalDecl`
//      never registered the scope-end `bz_box_release`. The issue's 24 bytes.
//
//   2. `isPlusOneBoxProducer` — took a `const ast::LocalDecl*`, so it could not
//      answer for a `const` node even once the scan admitted one.
//
//   3. `collectObjectStringReleaseEligible`'s TARGET end — `const o: Object
//      := p;` was never a candidate, so no bind-time retain and no scope-end
//      state-bit-gated `bz_string_release` were emitted.
//
//   4. the same collector's SOURCE end — with a `const p := <producer>;` source
//      the cast failed, so the `var o: Object := p;` alias never entered
//      `objectStringBorrowAll_`. #2900's escape walk reads exactly that set to
//      decide whether an Object alias of a string local is a BORROW or an
//      ESCAPE, so the alias made the SOURCE escape and the source's own
//      producer +1 stopped being released. The leak is on the source there.
//
// Plus, as in #3416, the collector widening alone was not sufficient:
// `StringEscapeWalker`'s `LocalConstDecl` arm is an unconditional escape and
// carried neither borrow-alias relaxation its sibling `LocalDecl` arm has.
// #3416 added #2764's; #3417 adds #2900's (`objectStringBorrowAliases`), which
// only becomes reachable once (3) admits an Object-headed `const` to that set.
//
// No new spelling of the LocalDecl/LocalConstDecl split is introduced. Every
// read of the declaration goes through `Emitter::localDeclInitView`, the single
// authority #3402 hoisted into LlvmBackend_impl.hpp; the candidate vectors are
// widened to `const ast::Node*`; `isPlusOneBoxProducer` takes an `ast::Node*`.
// The consumer side is unchanged — `lowerStmt`'s LocalConstDecl arm routes
// through the same `lowerLocalDecl`, whose box and object-string arming read
// `boxReleaseEligible_` / `objectStringReleaseEligible_` by NODE POINTER
// (`const void*`), never by node kind.
//
// WHY IT IS NOT A DOUBLE-FREE (Appendix C). §28.9's second entry is "Boxed
// value types" and its first is `string`; §21.1's *Ownership* paragraph admits
// exactly such runtime-managed values as a runtime `const`'s initializer
// ("Admitted: value-typed expressions ..., `string` and other runtime-managed
// values, `null` ..., and borrows") while rejecting an owned production. So the
// binding takes no disposal obligation and this adds none: what is released is
// the same single fresh box, or the alias's own bind-time retain, that the
// `var` path has always released, at the same point, under the same
// conservative BoxEscapeWalker / StringEscapeWalker. Exactly-once is EASIER to
// establish than for a `var` — §21.1 makes a `const` "not an assignable
// location", so `sawBoxAssignment` / `sawManagedAssignment` are vacuous for it,
// `boxReassignedLocals_` and `objectStringReassignedLocals_` can never hold it,
// and the state-bit reassignment machinery is never armed. An ESCAPING `const`
// is in neither eligibility set and still leaks — unchanged, and the safe
// direction (pinned by the returned-box guard below).
//
// ⚠ ORACLE NOTE (the same one #3416's test records): for the ALIAS shapes a
// SINGLE execution reports clean under LSan on both sides — the one iteration's
// string is still reachable from the live stack at leak-check time — so those
// rows are written as 500-iteration loops, which is the honest oracle. The BOX
// rows do fail without the fix at a single execution and are pinned that way
// too.
//
// FAIL-WITHOUT / PASS-WITH, measured program-by-program on one build of each
// compiler (the only difference being this fix's hunks reverted):
//
//   program                                      pre-fix           post-fix
//   const o: Object := n            (box)        24 B / 1 alloc    clean
//   var   o: Object := n            (control)    clean             clean
//   const o: Object := i    x500    (box)        12000 B / 500     clean
//   var   o: Object := i    x500    (control)    clean             clean
//   const o: Object := p    x500  (obj-string)   18500 B / 1000    clean
//   var   o: Object := p    x500    (control)    clean             clean
//   const p := producer; var o: Object := p x500 18500 B / 1000    clean
//   const p := producer; const o: Object := p    18500 B / 1000    clean
//   const o: Object := <class instance> (guard)  clean             clean
//   returned const box              (guard)      leaks, = `var`    leaks, = `var`
// per §21.1, §8.6, §28.9 (issue #3417)

#include <cstdlib>
#include <string>
#include <string_view>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
std::string runCheckedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
// runAot with generated-code ASan on and LeakSanitizer OFF — the oracle for the
// SAFE-DIRECTION guard whose expected behavior is a bounded residual leak. ASan
// still aborts on a use-after-free or double-free, which is what that guard
// asserts. Mirrors choice_value_like_string_payload_2115_test.cpp's helper.
bezel::test::JitResult runAotAsanNoLeak(std::string_view src) {
    static const bool toolchainOk =
        std::system("clang --version > /dev/null 2>&1") == 0;
    if (!toolchainOk) return runAotChecked(src);  // SKIP diagnostic
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    return runAot(src, opts);
}
}  // namespace

// ── THE ISSUE'S FIRST REPRO: a §21.1 runtime `const` declared at the `Object`
// head over an `int` local. §8.6 boxes the value into a fresh heap box that the
// slot uniquely holds; pre-fix nothing released it — 24 bytes / 1 allocation.
// per §21.1, §8.6, §28.9 (issue #3417)
BEZEL_TEST(const_local_box_release_3417,
           const_object_local_releases_its_box_spec_21_1_8_6_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var n := 41; "
        "    const o: Object := n; "
        "    Console.WriteLine(o.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "41\n");
}

// ── PARITY CONTROL: the `var` spelling was clean pre-fix and must stay clean.
// per §8.6, §28.9 (issue #3417)
BEZEL_TEST(const_local_box_release_3417,
           var_object_local_releases_its_box_parity_spec_8_6_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var n := 41; "
        "    var o: Object := n; "
        "    Console.WriteLine(o.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "41\n");
}

// ── UNBOUNDEDNESS axis for the box: one box per EXECUTION of the declaration,
// so a loop multiplies it (12000 bytes / 500 allocations pre-fix). Also proves
// the widened scan RECURSES into a `while` body for a `const` node.
// per §21.1, §8.6, §28.9 (issue #3417)
BEZEL_TEST(const_local_box_release_3417,
           const_object_local_box_released_each_iteration_spec_21_1_8_6) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      const o: Object := i; "
        "      acc := acc + 1; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "500\n");
}

// ── PARITY CONTROL for the looped box.
// per §8.6, §28.9 (issue #3417)
BEZEL_TEST(const_local_box_release_3417,
           var_object_local_box_released_each_iteration_parity_spec_8_6) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      var o: Object := i; "
        "      acc := acc + 1; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "500\n");
}

// ── OBJECT-STRING collector, TARGET end (#2900's family): an `Object`-headed
// `const` whose initializer is a bare `string`-typed identifier. Pre-fix the
// `const` was never a candidate, so no bind-time retain and no scope-end
// release; and the walker's `LocalConstDecl` arm additionally kept tainting the
// SOURCE, so `p`'s producer +1 was never released either. 500 iterations:
// 18500 bytes / 1000 allocations. The loop is load-bearing (see the ORACLE NOTE
// at the top of this file). // per §21.1, §28.9, §8.6 (issue #3417)
BEZEL_TEST(const_local_box_release_3417,
           const_object_alias_of_string_local_spec_21_1_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      var z := \"cd\"; "
        "      var p := \"ab\" + z; "
        "      const o: Object := p; "
        "      acc := acc + 1; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "500\n");
}

// ── PARITY CONTROL for the Object alias: the `var` spelling was already a
// recognized #2900 borrow and stays clean.
// per §28.9, §8.6 (issue #3417)
BEZEL_TEST(const_local_box_release_3417,
           var_object_alias_of_string_local_parity_spec_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      var z := \"cd\"; "
        "      var p := \"ab\" + z; "
        "      var o: Object := p; "
        "      acc := acc + 1; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "500\n");
}

// ── OBJECT-STRING collector, SOURCE end. Here the `const` is the string
// PRODUCER and the alias is an ordinary `var`: the source-side
// `dynamic_cast<const ast::LocalDecl*>(loc->decl)` failed, so the alias never
// entered `objectStringBorrowAll_`, #2900's relaxation never fired, and the
// alias tainted the `const` source — whose own producer +1 (armed by #3416)
// then went unreleased. The leaking reference belongs to the `const`, and the
// binding at fault is the `var`. 500 iterations: 18500 B / 1000 allocations.
// per §21.1, §28.9 (issue #3417)
BEZEL_TEST(const_local_box_release_3417,
           var_object_alias_of_const_string_source_spec_21_1_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      var z := \"cd\"; "
        "      const p := \"ab\" + z; "
        "      var o: Object := p; "
        "      acc := acc + 1; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "500\n");
}

// ── BOTH ends `const` — the combination of the two widenings above, which is
// the shape a body written entirely in §21.1 `const` bindings produces.
// per §21.1, §28.9 (issue #3417)
BEZEL_TEST(const_local_box_release_3417,
           const_object_alias_of_const_string_source_spec_21_1_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      var z := \"cd\"; "
        "      const p := \"ab\" + z; "
        "      const o: Object := p; "
        "      acc := acc + 1; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "500\n");
}

// ── SAFETY GUARD (clean before AND after): a CLASS instance bound to an
// `Object`-headed `const` is NOT a box. The state-bit / `boxLocalAssignmentSourceKind`
// discriminator must keep answering "not a box" for it, or `bz_box_release`
// would run on a class pointer — under ASan that is a wild free, not a leak.
// The `using var` holder keeps its own single §28.3 disposal.
// per §21.1, §8.6, §28.3, §28.9 (issue #3417)
BEZEL_TEST(const_local_box_release_3417,
           const_object_local_over_class_instance_not_box_released_spec_8_6) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class Holder "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    using var h := new Holder(7); "
        "    const o: Object := h; "
        "    Console.WriteLine(\"ok\"); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "ok\n");
}

// ── SAFETY GUARD (unchanged before AND after, and IDENTICAL to the `var`
// twin): a RETURNED `const`-bound box ESCAPES, so the BoxEscapeWalker excludes
// it from `boxReleaseEligible_` and no scope-end release is armed. The caller
// still reads a live box — a spurious release would surface under ASan as a
// use-after-free at the `.ToString()`, not as a leak, which is why this runs
// with leak detection deliberately off. The residual 24-byte leak is the
// pre-existing, safe direction and is byte-identical for the `var` spelling.
// per §21.1, §8.6, §28.3, §28.9 (issue #3417)
BEZEL_TEST(const_local_box_release_3417,
           returned_const_box_not_released_at_scope_end_no_uaf_spec_28_3) {
    auto r = runAotAsanNoLeak(
        "module M; import System; "
        "class C "
        "  public static method Box(): Object "
        "    var n := 41; "
        "    const o: Object := n; "
        "    return o; "
        "  end Box; "
        "  public static method Main(): int "
        "    Console.WriteLine(Box().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (!r.ran) return;  // SKIP: no ASan toolchain
    BEZEL_EXPECT_EQ(r.stdoutText, "41\n");
}
