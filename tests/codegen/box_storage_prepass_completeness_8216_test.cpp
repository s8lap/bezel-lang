// Codegen — #8216 (§28.9, v0.355): a mixed `Object` field releases a class
// instance as a box on overwrite, and the box-storage pre-pass skips lambda
// and destructor bodies.
//
// TWO DEFECTS, ONE MECHANISM.
//
// (1) THE STORE-SIDE RELEASE. `Emitter::lowerBoxFieldStore`
//     (src/codegen/LlvmBackend_Setup.cpp) lowers a store of a boxable value
//     into a class `Object`/interface field as
//
//         load-old → transfer/retain-new → store-new → release-old
//
//     and the `release-old` step was UNGATED: the DISPLACED value's kind was
//     never checked. §8.6 (docs/Bezel_Language_Spec.md:1228) makes that a
//     SPEC violation, not merely a crash: "a class instance, a string, a heap
//     array, and a function type are reference types whose values are
//     already heap-allocated" and "reach `Object` directly through the
//     inheritance chain" — a CLASS INSTANCE IN AN `Object` SLOT IS NOT A
//     BOX. §28.9 (docs/Bezel_Language_Spec.md:8112) adds that neither a
//     `string` nor a boxed value type is released by `bz_box_release`.
//
//     A class instance's bytes are { vtable, i8 disposed-flag@1, fields }, so
//     the word at offset 8 is the instance's FIRST FIELD, not a refcount:
//     releasing it as a box DECREMENTED that field. MEASURED on the base
//     (r30-c1, 8c8aef19): the issue's h13 shape prints `6` where `7` is
//     expected.
//
// (2) THE PRE-PASS'S BLIND SPOTS. `Emitter::collectBoxStorageFields` decides
//     whether a field holds a BOX (and may therefore be released by
//     `bz_box_release` at disposal). Its verdict is a JOIN over every write
//     to the field, so a write it cannot see is a write whose kind it cannot
//     account for. Two families were invisible to it:
//
//       * a LAMBDA BODY — a store made from a closure's body is a store the
//         program makes (the closure runs);
//       * ANOTHER CLASS'S DESTRUCTOR — a destructor body stores into a field
//         just as well as the field's own class's members do.
//
//     MEASURED on the base: a `Holder.o` that a lambda body (h01) or another
//     class's destructor (h17) filled with a class instance stayed "box-only",
//     so the holder's disposer called `bz_box_release` on the class instance:
//     h01 → "bezel: fatal: refcount over-release (box) — two releases for one
//     reference; terminating per §28.9" (rc=134); h17 → an ASan
//     heap-use-after-free in the generated `DisposeCore` (the instance was
//     FREED as if it were a box).
//
//     The destructor half of that blind spot was total: the pre-pass asked for
//     a `sema::MethodSymbol` and then dynamic_cast its `decl` to a
//     `ast::DestructorDecl`, but a destructor is a `sema::DestructorSymbol`
//     (src/sema/Symbol.hpp: `struct DestructorSymbol : MemberSymbol {}`), so
//     the cast NEVER MATCHED — NO destructor body was scanned at all, not even
//     the field's own class's.
//
// THE FIX.
//
//   * The store-side release is emitted only when the field's slot provably
//     held a box — the `collectBoxStorageFields` verdict, which is a join
//     over every write to the field (its initializer + every store). A join
//     over all writes implies the per-store fact the site needs. This is the
//     gate #8344 landed for the displaced-STRING shape; it covers the
//     displaced-CLASS-INSTANCE shape here for the same reason.
//   * The pre-pass now reaches every body the module compiles: it descends
//     into the expression forms (so a lambda's body is reached wherever the
//     lambda appears) and it scans the destructor bodies of EVERY local class.
//
// WHAT IS NOT CLAIMED HERE. The mixed field's LAST value is still not
// released at disposal (a field the pre-pass excludes is not a box-storage
// field, so its final box leaks). That is the pre-existing #8215 leak — a
// field that also stores an `Object`-typed value leaks every box it holds —
// and it is NOT closed by this change. What this change closes is the
// OVER-RELEASE: nothing is released as a box that is not a box.
//
// THE ORACLE. `runAotChecked` links the generated program under ASan with
// LeakSanitizer armed (`detect_leaks=1`), so a leak OR a sanitizer report
// turns into `ran == false`. Rows 1, 2 and 4 use it and are leak-clean: the
// values those rows put in the mixed field are a `StringBuilder` and a class
// instance, neither of which is refcounted, and each is released by its own
// owner.
//
// Row 3 uses `runAot` (LeakSanitizer off): the field it exercises ends up
// holding a BOX, and a mixed field's final box is not released at disposal.
// That is the pre-existing #8215 leak, which this issue does NOT close (see
// the RESIDUAL note above); it is not a leak this change introduces (the
// base leaks the same 24 B on the equivalent shape — it prints "6" instead of
// "7" and leaks the same box). `runAot` still fails the row on the crash: on
// the base the class instance is released as a box, so the program prints "6"
// (the row's expected "7" fails), and under the ASan sweep
// (`BEZEL_AOT_FORCE_SANITIZE=address`, which the gate runs) the generated
// program is AddressSanitizer-instrumented too.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// ---------------------------------------------------------------------------
// Row 1 — h01: the store is made from a LAMBDA BODY.
//
// The closure stores a `StringBuilder` (a CLASS INSTANCE — §8.6: a reference
// type whose value is already heap-allocated) into an `Object?` field. The
// field's owner is disposed inside the loop, so the holder's disposer sees
// the class instance in the slot.
//
// MEASURED on the base: "bezel: fatal: refcount over-release (box) — two
// releases for one reference; terminating per §28.9" (rc=134) — the disposer
// released the StringBuilder as a box. With the fix: rc=0 and the exact
// stdout below.
// ---------------------------------------------------------------------------
const char* kLambdaBodyStore =
    "module M8216A; "
    "public class Holder "
    "  public o: Object?; "
    "  public constructor() end constructor; "
    "end Holder; "
    "public class L "
    "  public static method Put(h: Holder, x: Object) "
    "    var f := lambda() => do h.o := x; end; "
    "    f(); "
    "  end Put; "
    "end L; "
    "public class Program "
    "  public static method Main(): int "
    "    using var sb := new StringBuilder(\"k\"); "
    "    for var i in 0..<100 do "
    "      using var h := new Holder(); "
    "      L.Put(h, sb); "
    "    end for; "
    "    Console.WriteLine(sb.ToString()); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// ---------------------------------------------------------------------------
// Row 2 — h17: the store is made from ANOTHER CLASS'S DESTRUCTOR BODY.
//
// `Other`'s destructor stores a class instance (the `Holder` itself) into the
// holder's `Object?` field; the holder is disposed right after, and its
// disposer must not release the class instance as a box.
//
// MEASURED on the base: an ASan heap-use-after-free in the generated
// `bz$M.Holder$DisposeCore` — the class instance was FREED as if it were a
// box, and the disposer then wrote to it. With the fix: rc=0 and the exact
// stdout below.
// ---------------------------------------------------------------------------
const char* kOtherClassDestructorStore =
    "module M8216B; "
    "public class Holder "
    "  public o: Object?; "
    "  public constructor() end constructor; "
    "end Holder; "
    "public class Other "
    "  public h: Holder; "
    "  public constructor(h: Holder) this.h := h; end constructor; "
    "  destructor "
    "    h.o := h; "
    "  end destructor; "
    "end Other; "
    "public class Program "
    "  public static method Main(): int "
    "    for var i in 0..<100 do "
    "      using var h := new Holder(); "
    "      using var o := new Other(h); "
    "    end for; "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// ---------------------------------------------------------------------------
// Row 3 — h13: a class instance in a mixed `Object?` field, then a BOX
// overwrites it.
//
// This row is the one that needs BOTH halves of the fix: the store that put
// the class instance there is made from a LAMBDA BODY (so the pre-pass has
// to see it — the field is excluded), and the box store that displaces it
// has to consult that verdict (so the class instance is not released as a
// box).
//
// MEASURED on the base: the program prints `6` where `7` is expected (the
// class instance's first field decremented as if it were a refcount).
// MEASURED with only the store-side gate (and the pre-pass's lambda blind
// spot still open): still `6` — the pre-pass never saw the lambda's store,
// so the field stayed "box-only" and the release fired. With both halves:
// `7`.
// ---------------------------------------------------------------------------
const char* kClassInstanceOverwrittenByBox =
    "module M8216C; "
    "public class Holder "
    "  public o: Object?; "
    "  public constructor() end constructor; "
    "end Holder; "
    "public class Widget "
    "  public v: int; "
    "  public constructor(v: int) this.v := v; end constructor; "
    "  public method Get(): int => v; "
    "end Widget; "
    "public class L "
    "  public static method Put(h: Holder, w: Widget) "
    "    var f := lambda() => do h.o := w; end; "
    "    f(); "
    "  end Put; "
    "end L; "
    "public class Program "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    using var w := new Widget(7); "
    "    L.Put(h, w); "
    "    h.o := 3; "
    "    Console.WriteLine(w.Get()); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// ---------------------------------------------------------------------------
// Row 4 — CONTROL: the pre-pass sees a field's INITIALIZER.
//
// An initializer is a WRITE to the field like any other, and it is the write
// that happens before any body runs. This row pins that the pre-pass's
// initializer check still excludes a field whose initializer is a STRING, so
// the disposer does not release the string as a box.
//
// This row is unchanged by the fix (the pre-pass already read the
// initializer) and is here to keep the initializer half of the join visible.
// ---------------------------------------------------------------------------
const char* kStringInitializerNotReleasedAsBox =
    "module M8216D; "
    "public class Holder "
    "  public O: Object := \"\"; "
    "end Holder; "
    "public class Program "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    Console.WriteLine(\"ok\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

}  // namespace

// ---------------------------------------------------------------------------
// Row 1 — h01: a lambda body's store is seen by the pre-pass.
//
// FAIL-WITHOUT: on the base the holder's disposer released the
// StringBuilder as a box — "bezel: fatal: refcount over-release (box)",
// rc=134.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_box_storage_prepass_completeness_8216,
           spec_28_9_lambda_body_store_is_not_over_released) {
    JitResult r = runAotChecked(kLambdaBodyStore);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "k\n");
}

// ---------------------------------------------------------------------------
// Row 2 — h17: another class's destructor body's store is seen by the
// pre-pass.
//
// FAIL-WITHOUT: on the base the holder's disposer released the class
// instance as a box — an ASan heap-use-after-free in the generated
// `DisposeCore`.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_box_storage_prepass_completeness_8216,
           spec_28_9_other_class_destructor_store_is_not_over_released) {
    JitResult r = runAotChecked(kOtherClassDestructorStore);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "done\n");
}

// ---------------------------------------------------------------------------
// Row 3 — h13: a class instance displaced by a box store is not released as
// a box.
//
// FAIL-WITHOUT: the base (and the store-side gate alone) print "6" where
// "7" is expected.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_box_storage_prepass_completeness_8216,
           spec_8_6_class_instance_overwritten_by_box_is_not_released_as_box) {
    JitResult r = runAot(kClassInstanceOverwrittenByBox);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// ---------------------------------------------------------------------------
// Row 4 — CONTROL: the pre-pass's initializer check is unchanged (a field
// initialized with a string is not a box-storage field).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_box_storage_prepass_completeness_8216,
           spec_28_9_string_initializer_field_is_not_a_box_storage_field) {
    JitResult r = runAotChecked(kStringInitializerNotReleasedAsBox);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}
