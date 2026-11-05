// Codegen — #8217 (§26.7 / §28.3, v0.355): an extension on a BUILTIN CLASS
// must not store its receiver either.
//
// §26.7 "Ownership in extensions" (docs/Bezel_Language_Spec.md:6191):
//
//   "The receiver is always a borrow; extensions cannot store `this`, return
//    it as `owned`, or dispose it."
//
// §26.7's rule is about a REFERENCE receiver. A value-kind receiver (§9.4) is
// COPIED by a store, so §9.4's own-storage rule — not this one — governs it.
//
// The DEFECT (fix #7968, 298343ba) added the §26.7 store check, but its
// receiver classification read a builtin CLASS head as a VALUE receiver:
//
//     case sema::TypeRef::Kind::Named: {
//         if (!t->declaration) return false;      // <-- the hole
//
// A builtin class (`StringBuilder`, `FileStream`, a collection, a task, a
// sync primitive) has NO parsed AST body, so its resolved TypeRef carries
// `declaration == nullptr` (src/sema/TypeResolution_Names.cpp's "nullary
// stdlib classes" row). The early-out therefore classified EVERY builtin-class
// extension's receiver as a value receiver, and the check never fired for one.
//
// MEASURED on the base r30-c1 (237f78a0), with the issue's own repro
// (`h05_ext_otherN.bzl`): `public extension StringBuilder { public method
// Into(h: Holder) h.o := this; end Into; }` COMPILED CLEAN (bezelc rc=0, no
// diagnostic). The same store from a user-class extension is rejected with the
// §26.7 diagnostic.
//
// The rows.
//
//   1-2. MUST-REJECT — the §26.7 diagnostic, for the issue's own repro and
//        its sibling (a holder that also has a `Take(n: int)` method).
//   3.   ADMITTED — the ADMITTING TWIN: the same extension storing a
//        PARAMETER (not the receiver) into the same slot. Green on the base
//        and on the tip: the shape is genuinely admitted, and the row fails if
//        the check widens past §26.7.
//   4.   ADMITTED — a read-only extension on the same builtin class.
//   5.   MUST-REJECT — the same rule for another builtin class head (a
//        collection), so the fix is not a one-name special case.
//   6.   ADMITTED — the VALUE-receiver twin: `extension int` storing `this`
//        is §9.4's own-storage rule, not §26.7's, and stays admitted (the
//        sibling row `value_receiver_extension_store_releases_last_box_no_leak`
//        in tests/codegen/extension_this_stash_7968_test.cpp pins the same
//        rule for the primitive head).

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
// 1. MUST-REJECT — the issue's own repro (`h05_ext_otherN.bzl`): an extension
// on the BUILT-IN `StringBuilder` storing its receiver into an `Object?` field.
//
// On the base r30-c1 (237f78a0) this compiled clean. The user-class twin of
// the same shape is rejected with the §26.7 diagnostic (the row
// `extension_member_slot_store_of_receiver_rejected` in
// tests/codegen/extension_this_stash_7968_test.cpp), so the difference is the
// receiver's KIND, not the store's shape.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_receiver_store_8217,
           builtin_class_receiver_store_of_receiver_rejected_spec_26_7) {
    BEZEL_EXPECT_TRUE(rejectedByExtensionReceiverRule(
        "module M8217A; "
        "public class Holder "
        "  public o: Object?; "
        "  public constructor() end constructor; "
        "end Holder; "
        "public extension StringBuilder "
        "  public method Into(h: Holder) h.o := this; end Into; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(\"k\"); "
        "    for var i in 0..<100 do "
        "      using var h := new Holder(); "
        "      sb.Into(h); "
        "    end for; "
        "    Console.WriteLine(sb.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;"));
}

// ---------------------------------------------------------------------------
// 2. MUST-REJECT — the issue's sibling repro (`h05_ext_otherT.bzl`): the same
// extension, against a holder that also declares a `Take(n: int)` method.
//
// The sibling differs from row 1 only in the holder's own member surface, so
// the row pins that the rejection does not depend on it.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_receiver_store_8217,
           builtin_class_receiver_store_of_receiver_rejected_with_member_spec_26_7) {
    BEZEL_EXPECT_TRUE(rejectedByExtensionReceiverRule(
        "module M8217B; "
        "public class Holder "
        "  public o: Object?; "
        "  public constructor() end constructor; "
        "  public method Take(n: int) o := n; end Take; "
        "end Holder; "
        "public extension StringBuilder "
        "  public method Into(h: Holder) h.o := this; end Into; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(\"k\"); "
        "    for var i in 0..<100 do "
        "      using var h := new Holder(); "
        "      sb.Into(h); "
        "    end for; "
        "    Console.WriteLine(sb.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;"));
}

// ---------------------------------------------------------------------------
// 3. ADMITTED — the ADMITTING TWIN of row 1: the same extension on the same
// builtin class, storing a PARAMETER (not the receiver) into the same slot.
//
// Green on the base AND on the tip: the shape is genuinely admitted, so row 1's
// rejection is the receiver's doing and not an unrelated error. The row fails
// if the check widens past §26.7.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_receiver_store_8217,
           builtin_class_receiver_parameter_store_admitted_spec_26_7) {
    JitResult r = runAotChecked(
        "module M8217C; "
        "public class Holder "
        "  public o: Object?; "
        "  public constructor() end constructor; "
        "end Holder; "
        "public extension StringBuilder "
        "  public method Into(h: Holder, x: Object?) h.o := x; end Into; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(\"k\"); "
        "    using var h := new Holder(); "
        "    sb.Into(h, sb); "
        "    Console.WriteLine(sb.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "k\n");
}

// ---------------------------------------------------------------------------
// 4. ADMITTED — a READ-ONLY extension on the same builtin class.
//
// §26.7's rule is about STORING the receiver; a read is the ordinary
// extension shape and must keep compiling.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_receiver_store_8217,
           builtin_class_receiver_read_admitted_spec_26_7) {
    JitResult r = runAotChecked(
        "module M8217D; "
        "public extension StringBuilder "
        "  public method Describe(): string => \"d:\" + this.ToString(); "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(\"k\"); "
        "    Console.WriteLine(sb.Describe()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "d:k\n");
}

// ---------------------------------------------------------------------------
// 5. MUST-REJECT — the same rule for ANOTHER builtin class head (a
// collection), so the fix reads the builtin CLASS-ness of the head and is not a
// one-name special case.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_receiver_store_8217,
           builtin_collection_receiver_store_of_receiver_rejected_spec_26_7) {
    BEZEL_EXPECT_TRUE(rejectedByExtensionReceiverRule(
        "module M8217E; "
        "public class Holder "
        "  public o: Object?; "
        "  public constructor() end constructor; "
        "end Holder; "
        "public extension Array<int> "
        "  public method Into(h: Holder) h.o := this; end Into; "
        "end extension; "
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    using var h := new Holder(); "
        "    a.Into(h); "
        "    Console.WriteLine(a.Count.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;"));
}

// ---------------------------------------------------------------------------
// 6. ADMITTED — the VALUE-receiver twin: `extension int` storing `this`.
//
// §26.7: "For a **value-kind** receiver, §9.4's value-receiver rule applies in
// the extension body exactly as in a declared member body" — the store copies
// the `int` (a box), so it is admitted. The sibling row
// `value_receiver_extension_store_releases_last_box_no_leak` in
// tests/codegen/extension_this_stash_7968_test.cpp pins the same rule.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_extension_receiver_store_8217,
           value_receiver_extension_store_admitted_spec_26_7) {
    JitResult r = runAotChecked(
        "module M8217F; "
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
