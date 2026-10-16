// Spec-derived regression tests for issue #6309 — an `Object`-local holding a
// BOX leaked when it was bound into a second same-scope `Object`/interface
// local.
//
// WHAT §28.9 AND §8.6 SAY. §8.6 makes a boxed value "an immutable shared
// reference"; §28.9 makes a boxed value runtime-managed and "exempt from
// manual disposal", and explicitly leaves its lifecycle to the runtime
// ("How the runtime manages their lifecycle ... is not part of this
// specification"). The RUNTIME decides by reference count:
// `src/runtime/bezel_runtime_refcounting.cpp` lays a box out as
// `{ void* vtable, int64_t rc, value-bytes }`, `boxValue` initializes rc to 1
// (the producer's single reference), and `bz_box_release` decrements and frees
// at 0 — reporting a loud `bz_refcount_overrelease_fail` below 0. No bind-time
// retain is emitted for an `Object`-slot alias (only a lambda capture and a
// class-field store retain), so the source and the alias hold ONE reference
// between them and exactly ONE `bz_box_release` is the correct discharge.
//
// THE DEFECT. `BoxEscapeWalker`'s `LocalDecl` arm (src/codegen/
// LlvmBackend_Setup.cpp) classified the binding `var oj: Object := oi;` as an
// ESCAPE of `oi`, so `oi` never joined `boxReleaseEligible_`. `oj` is not a
// +1-box producer, so it never joined either. Result: the box was released by
// nobody, once per binding. MEASURED on 389ca74b, at 200 iterations:
// `SUMMARY: AddressSanitizer: 4800 byte(s) leaked in 200 allocation(s)` —
// exactly one 24-byte box per iteration — for `Object -> Object`, for a
// three-link `oi -> oj -> ol` chain (also one box: it is the ALIAS, not the
// length), and for the issue's own `Object -> IComparable<int>` spelling.
//
// NOTE ON §5.1 AND THE ISSUE'S OWN ADMISSION. §5.1 makes `Object -> Object` the
// IDENTITY conversion, and the rows below need no implicit-conversion residual
// at all: the defect is reachable in the plainest spelling in the language.
// `Object -> IComparable<int>` is admitted today only as a #6073 residual
// (`src/sema/ExprTypecheck.cpp`, the `isSynthesizedBuiltinInterface` early
// return inside the #6023 arm); row 3 pins that spelling so the box release
// stays correct under the EXPLICIT spelling that #6073 will require, but the
// fix does not depend on that admission and changed no sema predicate.
//
// THE FIX. The LocalDecl arm now admits a bare-identifier binding into a
// destination that is itself a proven same-lifetime borrow — the sound
// condition fix #3729 already states for the record-field twin: the destination
// must be PROVABLY non-escaping AND not itself release-eligible, computed by the
// SAME conservative walk over the WHOLE enclosing body. `collectBoxReleaseEligible`
// gained a first pass that computes that verdict for every candidate (as a
// bounded fixpoint, so an alias chain settles one link per round) and the
// arming pass reads it. Withholding the relaxation is the SAFE direction: the
// box keeps leaking and is never double-freed.
//
// Rows 4, 5 and 7 are the over-release guards. A leak fix that releases too
// much is a use-after-free, which is worse than the bug it replaced; each of
// them reads the box AFTER the scope that owns it has ended, which is exactly
// the access a premature `bz_box_release` corrupts.
//
// Spec: §5.1 (implicit conversions / identity), §8.6 (boxing), §28.3 (the
// full-expression boundary and borrows), §28.9 (runtime-managed boxed values),
// §27.3 (lambda capture). Revision v0.338.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// A leak / double-free / use-after-free flips r.ran to false -> -999, distinct
// from any legal exit value. Each helper is a single call site and neither
// absorbs a toolchain skip: a skip makes the harness return `ran = false`
// (aot_detail::markInfra), which the `BEZEL_EXPECT_TRUE(r.ran)` below turns
// into a row failure rather than a silent pass.
// Both halves are asserted: `ran` says the program ran to normal termination
// under ASan (no memory error of any kind), and the exit code says it computed
// the spec-derived value. Either half alone is satisfiable vacuously — a
// `didNotCrash`-only row passes on a program that computes the wrong answer,
// and a value-only row can be satisfied by a sentinel.
void expectRanWith(const std::string& src, int expected) {
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, expected);
}

// The over-release guards. `runAotCheckedLenient` turns LSan OFF at runtime
// but keeps the generated-code AddressSanitizer fully active, which is exactly
// the oracle these rows want: the shape they exercise DOES leak on purpose
// (§28.3 lets an escaping borrow extend nothing, and the walker is
// deliberately conservative there), so a leak verdict would say nothing about
// the thing under test — while a premature release of that box is a
// heap-use-after-free at the very read these rows perform, and that IS caught.
// Crash and generated-code-ASan detection are unchanged.
void expectRanNoLeakVerdictWith(const std::string& src, int expected) {
    auto r = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, expected);
}

// §5.1 / §8.6 / §28.9 (#6309) — the CORE row, and the plainest spelling in the
// language. `Object -> Object` is the identity conversion; nothing here depends
// on an implicit-conversion residual. `oi` is the box producer (`var oi: Object
// := k` boxes the int); `oj` borrows that box; both die at the same scope end,
// so `oi`'s single `bz_box_release` discharges the one reference and `oj` is
// clean when it reads it.
//
// 200 iterations; the loop body returns `i.GetHashCode() mod 3`, whose sum is
// 199 over 0..<200. 199 mod 251 = 199.
// Pre-fix: LSan `4776 byte(s) leaked in 199 allocation(s)`, exit 99 -> -999.
// Post-fix: LSan-clean, 199.
BEZEL_TEST(codegen_object_alias_box_release_6309,
           object_identity_alias_releases_box_once_spec_5_1) {
    std::string src =
        "module M; "
        "class P "
        "  public static method Alias(k: int): int "
        "    var oi: Object := k; "
        "    var oj: Object := oi; "
        "    return oj.GetHashCode() mod 3; "
        "  end Alias; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<200 do t := t + Alias(i); end for; "
        "    return t mod 251; "
        "  end Main; "
        "end P;";
    expectRanWith(src, 199);
}

// §5.1 / §8.6 / §28.9 (#6309) — a three-link alias CHAIN, one reference, one
// release. `oi` produces the box; `oj` and `ol` are both plain same-scope
// borrows of the SAME box, so the whole chain still carries a single rc and a
// single `bz_box_release`. This is the row that requires the first pass to be a
// fixpoint rather than one sweep: `oj`'s own verdict depends on whether `ol`
// can outlive it, so `oi`'s cannot be decided until `oj`'s is — one link per
// round.
//
// MEASURED that the pre-fix leak is one box and not one per link: the chain
// leaked exactly the same count as the two-link form (`4776 byte(s) in 199
// allocation(s)` through the harness, `4800 byte(s) in 200 allocation(s)` from
// a direct compile). So without the fixpoint the middle link is still tainted
// and the chain still leaks; this row is what distinguishes the two designs.
// Post-fix: LSan-clean, 199.
BEZEL_TEST(codegen_object_alias_box_release_6309,
           three_link_alias_chain_releases_box_once_spec_5_1) {
    std::string src =
        "module M; "
        "class P "
        "  public static method Alias(k: int): int "
        "    var oi: Object := k; "
        "    var oj: Object := oi; "
        "    var ol: Object := oj; "
        "    return ol.GetHashCode() mod 3; "
        "  end Alias; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<200 do t := t + Alias(i); end for; "
        "    return t mod 251; "
        "  end Main; "
        "end P;";
    expectRanWith(src, 199);
}

// §5.1 / §8.6 / §21.1 / §28.9 (#6309) — the §21.1 `const` SPELLING of the
// same alias. `const oj: Object := oi;` is the same borrow of the same box; the
// walker's LocalConstDecl arm was the twin the fix had to reach too, exactly
// as #3417 reached the twin when it first admitted a `const` to the box
// candidate scan, and as #3416 did for the string borrow set.
//
// §21.1 makes this row's soundness STRONGER than the `var` form's: a runtime
// `const` is "not an assignable location", so the destination can never later
// receive a fresh box — the one way a `var` alias could become release-eligible
// itself after the verdict map was built.
//
// MEASURED fail-without: the `var` twin leaked `4776 byte(s) in 199
// allocation(s)` and this row leaked the same count on the pre-fix tree.
// Post-fix: LSan-clean, 199.
BEZEL_TEST(codegen_object_alias_box_release_6309,
           const_spelled_alias_releases_box_once_spec_21_1) {
    std::string src =
        "module M; "
        "class P "
        "  public static method Alias(k: int): int "
        "    var oi: Object := k; "
        "    const oj: Object := oi; "
        "    return oj.GetHashCode() mod 3; "
        "  end Alias; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<200 do t := t + Alias(i); end for; "
        "    return t mod 251; "
        "  end Main; "
        "end P;";
    expectRanWith(src, 199);
}

// §5.1 / §8.6 / §28.3 / §28.9 (#6309) — THE ISSUE'S OWN REPRO. `oi` is an
// `Object` local holding a boxed int; `c` is an interface-typed local bound from
// it and the box is read through the interface (`c.CompareTo(3)`). The leak
// measured on 389ca74b is the issue's 24 B box once per conversion.
//
// §5.1 does not list `Object -> IComparable<int>`; the issue records that it
// survives today only as a #6073 residual, and that once #6073 lands the
// program should be REJECTED and the leak re-checked through the explicit
// spelling. This row pins the current admission's runtime correctness, so the
// residual keeps leaking no box while it exists; it deliberately asserts
// nothing about whether that admission is right, which is #6073's call and
// lives in sema.
//
// 200 iterations of `i.CompareTo(3)` sum to 193; 193 mod 251 = 193.
// Pre-fix: LSan `4776 byte(s) leaked in 199 allocation(s)` -> -999.
// Post-fix: LSan-clean, 193.
BEZEL_TEST(codegen_object_alias_box_release_6309,
           interface_typed_alias_releases_box_once_spec_5_1) {
    std::string src =
        "module M; "
        "class P "
        "  public static method Conv(k: int): int "
        "    var oi: Object := k; "
        "    var c: IComparable<int> := oi; "
        "    return c.CompareTo(3); "
        "  end Conv; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<200 do t := t + Conv(i); end for; "
        "    return t mod 251; "
        "  end Main; "
        "end P;";
    expectRanWith(src, 193);
}

// OVER-RELEASE GUARD — the alias RETURNS. `oj` outlives the scope that owns
// the box, so §28.3 admits the escaping borrow and the walker must keep
// `oi` tainted: releasing at `oi`'s scope end would hand the caller a dangling
// box. The row reads the returned box in the CALLER, one scope later, which is
// precisely the heap-use-after-free an over-broad release produces; generated-
// code ASan is active here (LSan is off, because this shape deliberately keeps
// the conservative leak), so an over-release fails the row rather than passing
// it quietly.
//
// Green on the pre-fix tree and must STAY green: it pins the direction the fix
// must not move in. 199.
BEZEL_TEST(codegen_object_alias_box_release_6309,
           guard_returned_alias_is_not_released_spec_28_3) {
    std::string src =
        "module M; "
        "class P "
        "  public static method Alias(k: int): Object "
        "    var oi: Object := k; "
        "    var oj: Object := oi; "
        "    return oj; "
        "  end Alias; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<200 do "
        "      var z: Object := Alias(i); "
        "      t := t + z.GetHashCode() mod 3; "
        "    end for; "
        "    return t mod 251; "
        "  end Main; "
        "end P;";
    expectRanNoLeakVerdictWith(src, 199);
}

// OVER-RELEASE GUARD — the alias is passed to a CALL as an argument. §28.3
// does not promise the callee will not retain a borrow (§22.7 forbids a method
// body from retaining or transferring its borrowed receiver, but a non-method
// callee is a different question), so the walker must keep `oi` tainted here
// too. The callee reads the box, so a premature release is a use-after-free at
// that read.
//
// Green on the pre-fix tree and must STAY green. 199.
BEZEL_TEST(codegen_object_alias_box_release_6309,
           guard_alias_passed_as_call_argument_spec_28_3) {
    std::string src =
        "module M; "
        "class Q "
        "  public method Take(x: Object): int return x.GetHashCode() mod 3; end Take; "
        "end Q; "
        "class P "
        "  public static method Alias(k: int): int "
        "    var oi: Object := k; "
        "    var oj: Object := oi; "
        "    var q := new Q(); "
        "    return q.Take(oj); "
        "  end Alias; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<200 do t := t + Alias(i); end for; "
        "    return t mod 251; "
        "  end Main; "
        "end P;";
    expectRanNoLeakVerdictWith(src, 199);
}

// OVER-RELEASE GUARD, opposite direction — the alias is CAPTURED by a lambda.
// Here the box genuinely escapes the scope, and §27.3 makes the capture ENV
// hold its own reference (codegen emits `bz_box_retain` for a lambda capture),
// so the fix must NOT withhold the release: withholding it would leave the
// producer's +1 with no owner even though the env's own reference would be
// released on time.
//
// MEASURED: this row is RED on the pre-fix tree too (`4776 byte(s) leaked in
// 199 allocation(s)`), because the alias tainted `oi` and so the producer's
// +1 was never released while the env released only its own. So it is a
// fail-without row AND an over-release guard at once: a fix that released `oi`
// at its scope end while the env holds a retained reference would be sound
// here, but a fix that ALSO armed the env's reference without a matching
// retain would double-free — and either defect shows up as an ASan error, so
// the full LSan+ASan oracle applies. Post-fix: LSan-clean, 199.
BEZEL_TEST(codegen_object_alias_box_release_6309,
           guard_lambda_capture_of_alias_retains_independently_spec_27_3) {
    std::string src =
        "module M; "
        "class P "
        "  public static method Alias(k: int): int "
        "    var oi: Object := k; "
        "    var oj: Object := oi; "
        "    var f := lambda() => oj.GetHashCode() mod 3; "
        "    return f(); "
        "  end Alias; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<200 do t := t + Alias(i); end for; "
        "    return t mod 251; "
        "  end Main; "
        "end P;";
    expectRanWith(src, 199);
}

// ACCEPT CONTROL — the SINGLE-binding form the fix must leave exactly as it
// found it. `var oi: Object := k;` with no alias was already releasable, and
// this row is green on the pre-fix tree; it is the discriminator that proves
// the relaxation ADDED the aliased population rather than changing what an
// unaliased boxing local does.
//
// 200 iterations of `i.GetHashCode() mod 7` sum to 594; 594 mod 251 = 92.
BEZEL_TEST(codegen_object_alias_box_release_6309,
           accept_control_single_binding_still_released_spec_8_6) {
    std::string src =
        "module M; "
        "class P "
        "  public static method Boxed(k: int): int "
        "    var oi: Object := k; "
        "    return oi.GetHashCode() mod 7; "
        "  end Boxed; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<200 do t := t + Boxed(i); end for; "
        "    return t mod 251; "
        "  end Main; "
        "end P;";
    expectRanWith(src, 92);
}

}  // namespace