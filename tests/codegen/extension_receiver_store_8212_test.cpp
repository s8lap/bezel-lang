// Codegen — #8212 (§26.7 / §28.3, v0.355): an extension member must not store
// its receiver, laundered through a LOCAL ALIAS or through a CALL RESULT.
//
// §26.7 "Ownership in extensions" (docs/Bezel_Language_Spec.md:6191):
//
//   "The receiver is always a borrow; extensions cannot store `this`, return
//    it as `owned`, or dispose it."
//
// fix #7968 (298343ba) added the §26.7 store check. It rejects the store the
// extension body performs itself, `h.o := this`, and the store laundered
// through an INITIALIZER (`var a := this; h.o := a;`). Two more spellings of
// the same store were still admitted.
//
//   * the ALIAS-ASSIGNMENT spelling — the issue's own repro:
//
//         var a: Object? := null; a := this; h.o := a;
//
//     The alias fixpoint read only the body's local DECLARATIONS (an
//     initializer that is the receiver), so a binding that receives the
//     receiver through an ASSIGNMENT was never an alias.
//
//   * the CALL-RESULT spelling — the issue's own repro:
//
//         h.o := this.Self();
//
//     where `Self(): Box => this`. The call RETURNS the receiver, so the
//     stored value IS the receiver; only the spelling differs.
//
// MEASURED on the base r30-c1 (237f78a0), with the issue's own repros
// (`a04_alias_reassign.bzl`, `a15_self_call.bzl`): BOTH compiled clean
// (bezelc rc=0, no diagnostic).
//
// The rows.
//
//   1-3. MUST-REJECT — the §26.7 diagnostic, for the alias-assignment
//        spelling, the call-result spelling, and the two combined (an alias
//        of the receiver whose CALL returns the receiver).
//   4.   ADMITTED — the ADMITTING TWIN of row 1: the same extension storing
//        a PARAMETER (not the receiver) into the same slot. Green on the
//        base and on the tip: the shape is genuinely admitted, so row 1's
//        rejection is the receiver's doing and not an unrelated error.
//   5.   ADMITTED — the ADMITTING TWIN of row 2: a call on the receiver that
//        returns a DIFFERENT object (`this.Child()`, whose body returns a
//        field read — §28.3's receiver-sourced borrow). The stored value is
//        not the receiver, so §26.7 does not forbid it.
//   6.   ADMITTED — the CALL-ARGUMENT shape `h.Keep(this)`: §28.3 clause 3's
//        residual trust, which the conformance corpus pins twice
//        (s28_3_0119 `expect: accept`, s8_6_0011 `expect: run`).
//   7.   MUST-REJECT — the PARAMETER spelling of the alias: a plain parameter
//        that receives the receiver is the same store.
//
// The `const` spelling of the alias (`const a := this; h.o := a;`) is NOT
// pinned here: it is closed by the #7968 follow-up 428b06f8, whose own row
// (extension_const_local_alias_store_of_receiver_rejected) lives in
// tests/codegen/extension_this_stash_7968_test.cpp.

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
// 1. MUST-REJECT — the ALIAS-ASSIGNMENT spelling (the issue's own repro,
// `a04_alias_reassign.bzl`).
//
// `var a: Object? := null; a := this; h.o := a;` — the binding receives the
// receiver through an ASSIGNMENT, not through an initializer.
//
// On the base r30-c1 (237f78a0) this compiled clean.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_receiver_store_8212,
           alias_assignment_store_of_receiver_rejected_spec_26_7) {
    BEZEL_EXPECT_TRUE(rejectedByExtensionReceiverRule(
        "module M8212A; "
        "public class Holder "
        "  public o: Object?; "
        "  public p: Object?; "
        "  public constructor() o := null; p := null; end constructor; "
        "end Holder; "
        "public class Box "
        "  public v: int; "
        "  public constructor() v := 7; end constructor; "
        "  public method Self(): Box => this; "
        "end Box; "
        "public extension Box "
        "  public method M1(h: Holder): int "
        "    var a: Object? := null; a := this; h.o := a; return 0; "
        "  end M1; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    using var b := new Box(); "
        "    var r := b.M1(h); "
        "    Console.WriteLine(r); "
        "    return 0; "
        "  end Main; "
        "end Program;"));
}

// ---------------------------------------------------------------------------
// 2. MUST-REJECT — the CALL-RESULT spelling (the issue's own repro,
// `a15_self_call.bzl`).
//
// `h.o := this.Self();` where `Self(): Box => this` — the call RETURNS the
// receiver, so the stored value IS the receiver.
//
// On the base r30-c1 (237f78a0) this compiled clean.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_receiver_store_8212,
           call_result_store_of_receiver_rejected_spec_26_7) {
    BEZEL_EXPECT_TRUE(rejectedByExtensionReceiverRule(
        "module M8212B; "
        "public class Holder "
        "  public o: Object?; "
        "  public p: Object?; "
        "  public constructor() o := null; p := null; end constructor; "
        "end Holder; "
        "public class Box "
        "  public v: int; "
        "  public constructor() v := 7; end constructor; "
        "  public method Self(): Box => this; "
        "end Box; "
        "public extension Box "
        "  public method M1(h: Holder): int "
        "    h.o := this.Self(); return 0; "
        "  end M1; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    using var b := new Box(); "
        "    var r := b.M1(h); "
        "    Console.WriteLine(r); "
        "    return 0; "
        "  end Main; "
        "end Program;"));
}

// ---------------------------------------------------------------------------
// 3. MUST-REJECT — the two spellings COMBINED: an alias of the receiver whose
// CALL returns the receiver (`var a := this; h.o := a.Self();`).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_receiver_store_8212,
           alias_call_result_store_of_receiver_rejected_spec_26_7) {
    BEZEL_EXPECT_TRUE(rejectedByExtensionReceiverRule(
        "module M8212C; "
        "public class Holder "
        "  public o: Object?; "
        "  public constructor() end constructor; "
        "end Holder; "
        "public class Box "
        "  public v: int; "
        "  public constructor() v := 7; end constructor; "
        "  public method Self(): Box => this; "
        "end Box; "
        "public extension Box "
        "  public method M1(h: Holder): int "
        "    var a := this; h.o := a.Self(); return 0; "
        "  end M1; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    using var b := new Box(); "
        "    var r := b.M1(h); "
        "    Console.WriteLine(r); "
        "    return 0; "
        "  end Main; "
        "end Program;"));
}

// ---------------------------------------------------------------------------
// 4. ADMITTED — the ADMITTING TWIN of row 1: the same extension storing a
// PARAMETER (not the receiver) into the same slot.
//
// Green on the base AND on the tip: the shape is genuinely admitted, so row 1's
// rejection is the receiver's doing and not an unrelated error. The row fails
// if the check widens past §26.7.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_receiver_store_8212,
           alias_assignment_parameter_store_admitted_spec_26_7) {
    JitResult r = runAotChecked(
        "module M8212D; "
        "public class Holder "
        "  public o: Object?; "
        "  public constructor() end constructor; "
        "end Holder; "
        "public class Box "
        "  public v: int; "
        "  public constructor() v := 7; end constructor; "
        "end Box; "
        "public extension Box "
        "  public method M1(h: Holder, x: Object?): int "
        "    var a: Object? := null; a := x; h.o := a; return 0; "
        "  end M1; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    using var b := new Box(); "
        "    var r := b.M1(h, b); "
        "    Console.WriteLine(r); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n");
}

// ---------------------------------------------------------------------------
// 5. ADMITTED — the ADMITTING TWIN of row 2: a call on the receiver that
// returns a DIFFERENT object.
//
// `Child(): Holder => this.other` returns a field read — §28.3's
// receiver-sourced borrow — and the stored value is NOT the receiver, so
// §26.7 does not forbid it. The row fails if the check widens to "any call
// on the receiver".
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_receiver_store_8212,
           call_result_of_other_object_admitted_spec_26_7) {
    JitResult r = runAotChecked(
        "module M8212E; "
        "public class Holder "
        "  public o: Object?; "
        "  public constructor() end constructor; "
        "end Holder; "
        "public class Box "
        "  public v: int; "
        "  public owned other: Holder; "
        "  public constructor(v: int) "
        "    this.v := v; this.other := new Holder(); "
        "  end constructor; "
        "  public method Self(): Box => this; "
        "  public method Child(): Holder => this.other; "
        "end Box; "
        "public extension Box "
        "  public method M1(h: Holder): int "
        "    h.o := this.Child(); return 0; "
        "  end M1; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    using var b := new Box(7); "
        "    var r := b.M1(h); "
        "    Console.WriteLine(r); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n");
}

// ---------------------------------------------------------------------------
// 6. ADMITTED — the CALL-ARGUMENT shape `h.Keep(this)`.
//
// §28.3 clause 3 (the residual): "an unbound owned temporary or a borrow of a
// scope-bound local, passed to a plain parameter that the callee stores ...
// is admitted at the call". The extension body is that CALLER, so its `this`
// is admitted at the call exactly as any other plain parameter's value is.
//
// The conformance corpus pins this verdict TWICE, for the very shape:
// `tests/conformance/rules/cases/s28_3_0119.bzl` (`expect: accept`) and
// `tests/conformance/rules/cases/s8_6_0011.bzl` (`expect: run`, stdout `1`).
// §26.7's sentence names a store, a return-as-`owned` and a dispose — a CALL
// is not one of them: the callee performs the store, and §28.3 clause 3 makes
// that the callee's business.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_receiver_store_8212,
           call_argument_shape_admitted_spec_26_7) {
    JitResult r = runAotChecked(
        "module M8212F; "
        "public class Holder "
        "  public o: Object?; "
        "  public constructor() end constructor; "
        "  public method Keep(x: Object?): int o := x; return 1; end Keep; "
        "end Holder; "
        "public class Box "
        "  public constructor() end constructor; "
        "end Box; "
        "public extension Box "
        "  public method M1(h: Holder): int "
        "    var r := h.Keep(this); return r; "
        "  end M1; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    using var b := new Box(); "
        "    var r := b.M1(h); "
        "    Console.WriteLine(r); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

// ---------------------------------------------------------------------------
// 7. MUST-REJECT — the PARAMETER spelling of the alias.
//
// A plain parameter that receives the receiver is the same store:
// `x := this; h.o := x;` is `h.o := this`, laundered through a binding.
//
// (The `const` spelling — `const a := this; h.o := a;` — is closed by the
// #7968 follow-up 428b06f8 on the staging branch, with its own row in
// tests/codegen/extension_this_stash_7968_test.cpp. It is NOT pinned here:
// this commit does not implement it, so a row for it would not be red on this
// commit's base.)
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_receiver_store_8212,
           parameter_alias_store_of_receiver_rejected_spec_26_7) {
    BEZEL_EXPECT_TRUE(rejectedByExtensionReceiverRule(
        "module M8212H; "
        "public class Holder "
        "  public o: Object?; "
        "  public constructor() end constructor; "
        "end Holder; "
        "public class Box "
        "  public v: int; "
        "  public constructor() v := 7; end constructor; "
        "end Box; "
        "public extension Box "
        "  public method M1(h: Holder, x: Object?): int "
        "    x := this; h.o := x; return 0; "
        "  end M1; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    using var b := new Box(); "
        "    var r := b.M1(h, null); "
        "    Console.WriteLine(r); "
        "    return 0; "
        "  end Main; "
        "end Program;"));
}
