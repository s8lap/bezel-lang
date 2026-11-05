// Codegen — #7968 (§26.7 / §28.3 / §28.9, v0.355): an extension member that
// stores its receiver over-releases the box ("refcount over-release (box)") or
// SEGVs.
//
// §26.7 "Ownership in extensions" (docs/Bezel_Language_Spec.md:6193):
//
//   "The receiver is always a borrow; extensions cannot store `this`, return
//    it as `owned`, or dispose it."
//
// §28.3's field-store bullet is the general rule; its clause 2 admits a
// receiver-sourced borrow into a field ("borrows sourced from a parameter
// that is **not `owned`** — a plain or `scoped` parameter — or **from the
// receiver** are admissible"), so `this.o := this` in a DECLARED member's
// body is admitted under that clause's residual trust. §26.7 forbids the same
// store in an EXTENSION body, where `this` is the extension's own
// receiver-parameter borrow.
//
// TWO SYMPTOMS, TWO CAUSES (both MEASURED on the base 671722b8).
//
//   * "refcount over-release (box)" — a CODEGEN defect, not extension-
//     specific in its mechanism: `Emitter::collectBoxStorageFields` decides
//     whether an `Object`/interface-typed class field holds a BOX (and must
//     therefore be released by `bz_box_release` at disposal). Its pre-pass
//     scans the class's own methods, its constructors and its destructor —
//     but an EXTENSION method has a null `parent` (fix #2328), so every store
//     made from an extension body was INVISIBLE to the pre-pass. The field
//     stayed "box-only" and the disposer released a +1 that was never taken —
//     on a CLASS INSTANCE, whose offset 8 is its disposed-flag word, not a
//     box refcount. MEASURED: "bezel: fatal: refcount over-release (box) —
//     two releases for one reference; terminating per §28.9" (rc=134).
//   * the SEGV / OOM of the CALL-ARGUMENT shape (`h.Keep(this)`) — the
//     stored borrow outliving its source. That is §28.3 clause 3's residual
//     trust, not an extension defect: the same program WITHOUT an extension
//     (`h.Keep(b)` from a plain method) crashes identically, and the
//     conformance corpus admits the shape twice (the cases s28_3_0119
//     (`expect: accept`) and s8_6_0011 (`expect: run`)). The extension-side
//     store the rule of §26.7 DOES reject is the one the extension body
//     performs itself: `h.o := this`.
//
// THE ROWS.
//
//   1-4. MUST-REJECT — the §26.7 diagnostic, for each shape of the store the
//        extension body performs itself: a member slot (`h.o := this`), an
//        interface receiver, a LOCAL ALIAS of the receiver
//        (`var a := this; h.o := a;`), and a store inside a LAMBDA in the
//        body (the closure captures the receiver).
//   5.   ADMITTED — an extension that only READS its receiver.
//   6.   ADMITTED — a DECLARED member's `this.o := this`: §28.3 clause 2
//        admits it (the residual trust), and §26.7's rule is the extension-
//        side tightening, so the twin must keep compiling.
//   7.   RUNTIME — the codegen half: an extension that stores a REFCOUNTED
//        value into an `Object?` field must not arm the field's disposer
//        with `bz_box_release`. MEASURED on the base: rc=134 ("refcount
//        over-release (box)"); after the fix: rc=0, exact stdout. This row is
//        also the ADMITTING TWIN of row 1 — the same shape with a legal value
//        (a parameter, not the receiver) — and it is RED on the base arm.
//   8.   RUNTIME — the box half, unchanged: an extension that stores a
//        VALUE (a box) into an `Object?` field must still have the field
//        released as a box at disposal.
//   9-11. NO-LEAK (fix-forward, the first review's REJECT) — a field the
//        constructor sets to `null` (`o := null`, or a `:= null`
//        initializer) and that later receives only BOXES must still be
//        released as a box at disposal. The pre-pass used to count the
//        `null` store as a non-box store and EXCLUDE the field; the old
//        unconditional re-insert in `lowerBoxFieldStore` silently re-admitted
//        it, and gating that re-insert (needed for row 7) exposed the leak of
//        the last box (24 B). A `null` literal is now NEUTRAL in the pre-pass.
//        Row 9 has no extension at all; row 10 is the initializer spelling;
//        row 11 is a legal VALUE-receiver extension (`extension int`).
//   12.  RUNTIME — the same pre-pass hole from a method of ANOTHER CLASS
//        (`Other.Put(h, x)` does `h.o := x`): the pre-pass scanned only the
//        holder class's own bodies, so the disposer released a
//        `StringBuilder` as a box ("refcount over-release (box)").
//
// THE LEAK ORACLE (rows 9-12): `runAotChecked` links the program under ASan
// with LeakSanitizer armed (`detect_leaks=1`), and a leak or a sanitizer
// report turns into `ran == false`. Each body loops 100 times.

#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The §26.7 rejection. Asserted on the diagnostic TEXT, not on `!r.ran`:
// `runAot`'s WIFSIGNALED path synthesizes an Error diagnostic for a runtime
// crash too, so "it did not run" alone cannot distinguish a genuine rejection
// from a segfault (tests/README.md).
bool rejectedByExtensionReceiverRule(const std::string& program) {
    JitResult r = runAotChecked(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§26.7") == std::string_view::npos) continue;
        if (d.message.find("must not store its receiver") != std::string_view::npos)
            return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. MUST-REJECT — the member-slot store: `h.o := this`.
//
// This is the issue's headline shape (the R37 probe `c08_ext_stash_objext`).
// On the base it compiled clean and died at run time with "bezel: fatal:
// refcount over-release (box) — two releases for one reference; terminating
// per §28.9" (rc=134).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_this_stash_7968,
           extension_member_slot_store_of_receiver_rejected) {
    BEZEL_EXPECT_TRUE(rejectedByExtensionReceiverRule(
        "module M7968A; "
        "public class Holder "
        "  public o: Object?; "
        "  public constructor() end constructor; "
        "end Holder; "
        "public class Box "
        "  public constructor() end constructor; "
        "end Box; "
        "public extension Object "
        "  public method Stash(h: Holder) h.o := this; end Stash; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    using var b := new Box(); "
        "    b.Stash(h); "
        "    Console.WriteLine(\"never\"); "
        "    return 0; "
        "  end Main; "
        "end Program;"));
}

// ---------------------------------------------------------------------------
// 2. MUST-REJECT — an INTERFACE receiver.
//
// §8.6: "The receiver of an extension-block call whose head is `Object` or an
// interface type is not admitted: that body reads `this` at the head type,
// which announces no borrow, and may pass it to a parameter that stores it
// (§28.3's field-store bullet), so the receiver position is borrow-erasing
// like a parameter of that type."
//
// The extension's receiver is the extension's own receiver-parameter borrow
// (§28.3.8 clause 1: "the receiver-parameter borrow of an extension method
// (§26.7)"), so §26.7's prohibition holds for an interface head too.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_this_stash_7968,
           extension_interface_receiver_store_rejected) {
    BEZEL_EXPECT_TRUE(rejectedByExtensionReceiverRule(
        "module M7968C; "
        "public interface IFace "
        "  method Get(): int; "
        "end IFace; "
        "public class Holder "
        "  public o: Object?; "
        "  public constructor() end constructor; "
        "end Holder; "
        "public record R(x: int): IFace "
        "  public method Get(): int => x; "
        "end R; "
        "public extension IFace "
        "  public method Stash(h: Holder) h.o := this; end Stash; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    using var r := new R(1); "
        "    r.Stash(h); "
        "    Console.WriteLine(\"never\"); "
        "    return 0; "
        "  end Main; "
        "end Program;"));
}

// ---------------------------------------------------------------------------
// 3. MUST-REJECT — a LOCAL ALIAS of the receiver.
//
// `var a := this; h.o := a;` is the same store, laundered through a binding.
// The alias set is tracked to a fixpoint over the body's local declarations,
// so `var b := a;` is caught too.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_this_stash_7968,
           extension_local_alias_store_of_receiver_rejected) {
    BEZEL_EXPECT_TRUE(rejectedByExtensionReceiverRule(
        "module M7968D; "
        "public class Holder "
        "  public o: Object?; "
        "  public constructor() end constructor; "
        "end Holder; "
        "public class Box "
        "  public constructor() end constructor; "
        "end Box; "
        "public extension Object "
        "  public method Stash(h: Holder) "
        "    var a := this; "
        "    var b := a; "
        "    h.o := b; "
        "  end Stash; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    using var b := new Box(); "
        "    b.Stash(h); "
        "    Console.WriteLine(\"never\"); "
        "    return 0; "
        "  end Main; "
        "end Program;"));
}

// ---------------------------------------------------------------------------
// 3b. MUST-REJECT — a §21.1 `const` LOCAL alias of the receiver (#3591 class).
//
// §26.7 (v0.355): extensions cannot store `this`. `const a := this; h.o := a;`
// is the same store as row 3's `var` spelling, laundered through a `const`
// binding (§21.1 local `const`). The alias walk must hold the `const` form as
// a candidate too, or the store compiles clean and over-releases at run time.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_this_stash_7968,
           extension_const_local_alias_store_of_receiver_rejected) {
    BEZEL_EXPECT_TRUE(rejectedByExtensionReceiverRule(
        "module M7968D2; "
        "public class Holder "
        "  public o: Object?; "
        "  public constructor() end constructor; "
        "end Holder; "
        "public class Box "
        "  public constructor() end constructor; "
        "end Box; "
        "public extension Object "
        "  public method Stash(h: Holder) "
        "    const a := this; "
        "    h.o := a; "
        "  end Stash; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    using var b := new Box(); "
        "    b.Stash(h); "
        "    Console.WriteLine(\"never\"); "
        "    return 0; "
        "  end Main; "
        "end Program;"));
}

// ---------------------------------------------------------------------------
// 4. MUST-REJECT — a store of the receiver inside a LAMBDA in the extension
// body.
//
// The closure captures the receiver, so the store it makes is the same store,
// one activation later — and the closure may outlive the extension's
// activation entirely. The walk descends into a lambda body's statement list
// (and into a `case` arm's block body) for that reason.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_this_stash_7968,
           extension_lambda_body_store_of_receiver_rejected) {
    BEZEL_EXPECT_TRUE(rejectedByExtensionReceiverRule(
        "module M7968H1; "
        "public class Holder "
        "  public o: Object?; "
        "  public constructor() end constructor; "
        "end Holder; "
        "public class Box "
        "  public constructor() end constructor; "
        "end Box; "
        "public extension Object "
        "  public method Stash(h: Holder) "
        "    var f := lambda() => do h.o := this; end; "
        "    f(); "
        "  end Stash; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    using var b := new Box(); "
        "    b.Stash(h); "
        "    Console.WriteLine(\"never\"); "
        "    return 0; "
        "  end Main; "
        "end Program;"));
}

// ---------------------------------------------------------------------------
// 5. ADMITTED — an extension that only READS its receiver.
//
// The rule is about STORING the receiver; a read is the ordinary extension
// shape and must keep compiling. 100 iterations, exact stdout.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_this_stash_7968,
           extension_receiver_read_admitted) {
    JitResult r = runAotChecked(
        "module M7968F; "
        "public class Box "
        "  public v: int; "
        "  public constructor(v: int) this.v := v; end constructor; "
        "  public override method ToString(): string => \"box\" + v.ToString(); "
        "end Box; "
        "public extension Object "
        "  public method Describe(): string => \"d:\" + this.ToString(); "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 0..<100 do "
        "      using var b := new Box(i); "
        "      if b.Describe() = \"d:box\" + i.ToString() then n := n + 1; end if; "
        "    end for; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

// ---------------------------------------------------------------------------
// 6. ADMITTED — the DECLARED-member twin.
//
// §28.3 clause 2 admits a receiver-sourced borrow into a field in a declared
// member's body (under the residual trust), and §26.7's rule is the
// EXTENSION-side tightening. The twin must keep compiling — if this row ever
// starts rejecting, the check has widened past §26.7.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_this_stash_7968,
           declared_member_receiver_store_admitted) {
    JitResult r = runAotChecked(
        "module M7968G; "
        "public class Holder "
        "  public o: Object?; "
        "  public constructor() end constructor; "
        "  public method Stash() this.o := this; end Stash; "
        "end Holder; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    h.Stash(); "
        "    Console.WriteLine(\"ok\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// ---------------------------------------------------------------------------
// 7. RUNTIME — the codegen half: an extension that stores a REFCOUNTED VALUE
// (not the receiver) into an `Object?` field.
//
// The field's disposer must not be armed with `bz_box_release` for it: the
// pre-pass `collectBoxStorageFields` has to see the store made from the
// extension body, or the field stays "box-only" and the disposer releases a
// class instance as a box.
//
// MEASURED on the base 671722b8 (the R37 probe `g02_stash_sb_bound` and the
// t3d control): "bezel: fatal: refcount over-release (box) — two releases for
// one reference; terminating per §28.9" (rc=134). After the fix: rc=0 and the
// exact stdout below.
//
// The stored value is a refcounted BUILT-IN (`StringBuilder`), owned by the
// caller's `using var`, and the holder's disposal must not free it a second
// time; the read happens while the owner is still live. A user-defined class
// would be the wrong probe: its offset-8 word is the disposed flag, not a box
// refcount, so the same corruption is silent instead of loud.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_this_stash_7968,
           extension_stores_refcounted_value_without_box_over_release) {
    JitResult r = runAotChecked(
        "module M7968H; "
        "public class Sink "
        "  public _saved: Object?; "
        "  public constructor() end constructor; "
        "end Sink; "
        "public class Box "
        "  public v: int; "
        "  public constructor(v: int) this.v := v; end constructor; "
        "  public override method ToString(): string => \"box\"; "
        "end Box; "
        "public extension Object "
        "  public method StashSb(s: Sink, x: Object) s._saved := x; end StashSb; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var s := new Sink(); "
        "    using var sb := new StringBuilder(\"k\"); "
        "    using var b := new Box(7); "
        "    b.StashSb(s, sb); "
        "    Console.WriteLine(s._saved!.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "k\n");
}

// ---------------------------------------------------------------------------
// 8. RUNTIME — the box half, unchanged: an extension that stores a VALUE (a
// box) into an `Object?` field.
//
// The pre-pass change must not break the box case: a field that only ever
// receives a box is still a box-storage field, and its disposer still
// releases the box (a leak otherwise). The value is read back before the
// holder's disposal, so the stdout is the same either way — the row pins
// that the box path is untouched.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_this_stash_7968,
           extension_stores_box_value_still_released) {
    JitResult r = runAotChecked(
        "module M7968I; "
        "public class Sink "
        "  public _saved: Object?; "
        "  public constructor() end constructor; "
        "end Sink; "
        "public extension Object "
        "  public method StashInt(s: Sink, n: int) s._saved := n; end StashInt; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var s := new Sink(); "
        "    using var b := new Box(); "
        "    b.StashInt(s, 42); "
        "    Console.WriteLine(s._saved!.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program; "
        "public class Box "
        "  public constructor() end constructor; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// ---------------------------------------------------------------------------
// 9. NO-LEAK — no extension: the constructor nulls an `Object?` field that a
// method later fills with boxes (§8.6 boxing; §28.9: a field's box is
// released when its owner is disposed).
//
// MEASURED on 87dc31b2 (the first attempt): LeakSanitizer "24 byte(s) leaked
// in 1 allocation(s)" — the last box. The constructor's `o := null` excluded
// the field from the box-storage set. On the base 671722b8 it was clean.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_this_stash_7968,
           ctor_null_then_box_field_releases_last_box_no_leak) {
    JitResult r = runAotChecked(
        "module M7968J; "
        "public class Holder "
        "  public o: Object?; "
        "  public constructor() o := null; end constructor; "
        "  public method Take(n: int) o := n; end Take; "
        "end Holder; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    for var i in 0..<100 do h.Take(i); end for; "
        "    Console.WriteLine(h.o!.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "99\n");
}

// ---------------------------------------------------------------------------
// 10. NO-LEAK — the initializer spelling: `public o: Object? := null;`.
//
// MEASURED on 87dc31b2: the same 24 B leak. (This is also the shape of the
// `box_object_slot_declared_equals_9_2_5924` rows, whose holder declares
// `public property O: Object? := null;` — 11 of them failed on 87dc31b2.)
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_this_stash_7968,
           null_initializer_then_box_field_releases_last_box_no_leak) {
    JitResult r = runAotChecked(
        "module M7968K; "
        "public class Holder "
        "  public o: Object? := null; "
        "  public constructor() end constructor; "
        "  public method Take(n: int) o := n; end Take; "
        "end Holder; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    for var i in 0..<100 do h.Take(i); end for; "
        "    Console.WriteLine(h.o!.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "99\n");
}

// ---------------------------------------------------------------------------
// 11. NO-LEAK — a legal VALUE-receiver extension stores its receiver.
//
// §26.7: "For a **value-kind** receiver, §9.4's value-receiver rule applies
// in the extension body exactly as in a declared member body" — the store
// copies the `int` (a box), so it is admitted, and the box must be released.
// MEASURED on 87dc31b2: the 24 B leak of the last box.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_this_stash_7968,
           value_receiver_extension_store_releases_last_box_no_leak) {
    JitResult r = runAotChecked(
        "module M7968L; "
        "public class Holder "
        "  public o: Object?; "
        "  public constructor() o := null; end constructor; "
        "end Holder; "
        "public extension int "
        "  public method Stash(h: Holder) h.o := this; end Stash; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    for var i in 0..<100 do i.Stash(h); end for; "
        "    Console.WriteLine(h.o!.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "99\n");
}

// ---------------------------------------------------------------------------
// 12. RUNTIME — the pre-pass hole from ANOTHER CLASS's method.
//
// `Other.Put` stores a refcounted built-in into `Holder`'s `Object?` field.
// The pre-pass scanned only `Holder`'s own bodies (and, at 87dc31b2,
// extension bodies), so it never saw this store, the field stayed
// "box-only", and the holder's disposer called `bz_box_release` on the
// `StringBuilder`. MEASURED on 87dc31b2 and on the base: "bezel: fatal:
// refcount over-release (box) — two releases for one reference; terminating
// per §28.9". The stored value is a plain-parameter borrow (§28.3 clause 2),
// and its owner outlives every holder.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_this_stash_7968,
           other_class_method_store_no_box_over_release) {
    JitResult r = runAotChecked(
        "module M7968M; "
        "public class Holder "
        "  public o: Object?; "
        "  public constructor() end constructor; "
        "end Holder; "
        "public class Other "
        "  public static method Put(h: Holder, x: Object) h.o := x; end Put; "
        "end Other; "
        "public class Program "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(\"k\"); "
        "    for var i in 0..<100 do "
        "      using var h := new Holder(); "
        "      Other.Put(h, sb); "
        "    end for; "
        "    Console.WriteLine(sb.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "k\n");
}
