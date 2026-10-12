// #5315/#5339 SIBLING, not a repeat: the two issues' own fixes are already in
// the tree (measured at 684d6125 — `ensurePropertyBlockGetter` and
// `ensureIndexerBlockGetter` both key their memo on `propertyThunkSubstKey`,
// and the block-bodied INDEXER carries the direct-receiver substitution branch
// caff8947 installed at LlvmBackend_Assign.cpp:2519). The PROPERTY read face
// was missed for exactly the same reason and by exactly the same mechanism, so
// these rows pin the sibling, not the original.
//
// Two independent omissions, both in the qualified/bare inline READ paths of
// LlvmBackend_Exprs.cpp (the §27.6 property-REFERENCE face, which #5315 did
// cover, already passes `ps` at LlvmBackend_Layout.cpp:7131):
//
//   1. NO SUBSTITUTION INSTALLED for a DIRECT generic receiver. The gate was
//      `recvTy2->declaration != ps2->parent` — the ANCESTOR shape only. For
//      `h.bv` where `h: Holder<string>` and `bv` is declared on `Holder<T>`,
//      the receiver's own declaration IS `ps2->parent`, so the branch was
//      skipped and the standalone getter lowered against the erased T->i32
//      template: it names its return type `lowerType(T)` == i32 and reads a
//      T=string field's first four bytes. MEASURED on base 684d6125: the
//      programs below print `0` instead of `z`.
//   2. NO `ps` PASSED, so the memo key stayed (pd, tag, {}, {}) — the
//      coarse key #5315 was filed about, still on these two call sites.
//      This is what makes the BARE-this row (`return bv;` inside `Holder<T>`'s
//      own `Show(): T`) not merely wrong but unbuildable: the enclosing
//      `Show()` is specialized to return `ptr` while the getter it calls was
//      emitted as `ret i32 %1`, and the backend aborts with "Function return
//      type does not match operand type of return inst! ... IR verification
//      failed".
//
// The direct-receiver install is `setupReceiverGenericSubst` — the same
// helper, and the same gate, the block-bodied indexer twin already used.
//
// Oracles: runAot only. This file adds NO runAotChecked call site:
// `scripts/toolchain_skip_surface.py` is red (baseline 2774, tree 2776) before
// this change, and raising the denominator further keeps that gate blocked.
// tools/toolchain_skip_surface.baseline is coordinator sole-writer.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// Omission 1, qualified read, T=string. One instantiation, so this row cannot
// be explained by memo cross-talk between two instantiations — it isolates the
// missing substitution install on its own.
constexpr const char* k_direct_qualified =
    "module M;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public property bv: T get => do return _v; end; end;\n"
    "  public constructor(x: T) _v := x; end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hs := new Holder<string>(\"z\");\n"
    "    Console.WriteLine(hs.bv);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The same read through a TYPED local, so the row also pins the §5.1
// conversion face and not just the Console.WriteLine overload.
constexpr const char* k_direct_typed_local =
    "module M;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public property bv: T get => do return _v; end; end;\n"
    "  public constructor(x: T) _v := x; end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hs := new Holder<string>(\"z\");\n"
    "    var s: string := hs.bv;\n"
    "    Console.WriteLine(s);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// Two instantiations of ONE declaration, qualified read. Both arms matter: the
// string arm catches the missing install, the int arm is the control that
// must keep printing its own value (a fix that made every instantiation share
// one getter would print 3, 3 here).
constexpr const char* k_two_instantiations =
    "module M;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public property bv: T get => do return _v; end; end;\n"
    "  public constructor(x: T) _v := x; end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hs := new Holder<string>(\"z\");\n"
    "    var hi := new Holder<int>(3);\n"
    "    Console.WriteLine(hs.bv);\n"
    "    Console.WriteLine(hi.bv);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// Omission 1 with the int arm first, so the row is not satisfied by a fix that
// merely happens to key off "first instantiation wins".
constexpr const char* k_int_first =
    "module M;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public property bv: T get => do return _v; end; end;\n"
    "  public constructor(x: T) _v := x; end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3);\n"
    "    var hs := new Holder<string>(\"z\");\n"
    "    Console.WriteLine(hi.bv);\n"
    "    Console.WriteLine(hs.bv);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// Two strings: distinct values, so the row pins that the memo did not freeze
// the first object's field onto the second.
constexpr const char* k_two_strings =
    "module M;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public property bv: T get => do return _v; end; end;\n"
    "  public constructor(x: T) _v := x; end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var h1 := new Holder<string>(\"z\");\n"
    "    var h2 := new Holder<string>(\"w\");\n"
    "    Console.WriteLine(h1.bv);\n"
    "    Console.WriteLine(h2.bv);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// Omission 2: the BARE-this read (`return bv;` with implicit `this`), the
// #5315 file's own stated gap ("The INLINE read face ... is NOT covered
// here"). On base this does not miscompile, it ABORTS the backend with an IR
// verification failure — `r.ran` is false and the diagnostic is the specific
// one, so the row pins a build failure rather than a silent wrong value.
constexpr const char* k_bare_this =
    "module M;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public property bv: T get => do return _v; end; end;\n"
    "  public constructor(x: T) _v := x; end constructor;\n"
    "  public method Show(): T\n"
    "    return bv;\n"
    "  end Show;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hs := new Holder<string>(\"z\");\n"
    "    Console.WriteLine(hs.Show());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// Control: the NON-generic twin of the bare-this row. It was already correct
// on base (printed 5), so this row is expected to pass WITHOUT the fix — it
// is here to fail loudly if the new `ps` argument ever perturbs the
// instantiation-agnostic key, not as a fail-without pin.
constexpr const char* k_bare_this_nongeneric =
    "module M;\n"
    "class Plain\n"
    "  public _v: int;\n"
    "  public property bv: int get => do return _v; end; end;\n"
    "  public constructor(x: int) _v := x; end constructor;\n"
    "  public method Show(): int\n"
    "    return bv;\n"
    "  end Show;\n"
    "end Plain;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var p := new Plain(5);\n"
    "    Console.WriteLine(p.Show());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// Control: the two sibling FORMS of the same property. Both were already
// correct on base (printed z, z), so these are blast-radius guards — they fail
// if the fix reached the expression-bodied or auto-property paths it must not
// have touched.
constexpr const char* k_autoprop_control =
    "module M;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public property bv: T;\n"
    "  public constructor(x: T) _v := x; bv := x; end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hs := new Holder<string>(\"z\");\n"
    "    var hi := new Holder<int>(3);\n"
    "    Console.WriteLine(hs.bv);\n"
    "    Console.WriteLine(hi.bv);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

constexpr const char* k_exprbody_control =
    "module M;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public property bv: T get => _v; end;\n"
    "  public constructor(x: T) _v := x; end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hs := new Holder<string>(\"z\");\n"
    "    var hi := new Holder<int>(3);\n"
    "    Console.WriteLine(hs.bv);\n"
    "    Console.WriteLine(hi.bv);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// Control: a plain METHOD reading the same T-typed field. Already correct on
// base (printed z) — the substitution a method body gets, and the one the
// standalone getter body was not getting.
constexpr const char* k_method_control =
    "module M;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public constructor(x: T) _v := x; end constructor;\n"
    "  public method Peek(): T\n"
    "    return _v;\n"
    "  end Peek;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hs := new Holder<string>(\"z\");\n"
    "    Console.WriteLine(hs.Peek());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

}  // namespace

// FAIL-WITHOUT on 684d6125: printed `0` (expected `z`).
BEZEL_TEST(generic_property_block_getter_inline_read_5315_sibling,
           direct_generic_receiver_qualified_read_spec_14_1) {
    JitResult r = runAot(k_direct_qualified);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("z\n"));
}

// FAIL-WITHOUT on 684d6125: printed `0` (expected `z`).
BEZEL_TEST(generic_property_block_getter_inline_read_5315_sibling,
           direct_generic_receiver_through_typed_local_spec_5_1) {
    JitResult r = runAot(k_direct_typed_local);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("z\n"));
}

// FAIL-WITHOUT on 684d6125: printed `0 3` (expected `z 3`).
BEZEL_TEST(generic_property_block_getter_inline_read_5315_sibling,
           two_instantiations_each_read_its_own_field_spec_14_1) {
    JitResult r = runAot(k_two_instantiations);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("z\n3\n"));
}

// FAIL-WITHOUT on 684d6125: printed `3 0` (expected `3 z`).
BEZEL_TEST(generic_property_block_getter_inline_read_5315_sibling,
           int_instantiation_first_does_not_starve_string_spec_14_1) {
    JitResult r = runAot(k_int_first);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nz\n"));
}

// FAIL-WITHOUT on 684d6125: printed `0 0` (expected `z w`).
BEZEL_TEST(generic_property_block_getter_inline_read_5315_sibling,
           two_string_instantiations_are_not_frozen_together_spec_14_1) {
    JitResult r = runAot(k_two_strings);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("z\nw\n"));
}

// FAIL-WITHOUT on 684d6125: did not run — "IR verification failed
// (malformed IR emitted)" / "Function return type does not match operand type
// of return inst!". Asserts the SPECIFIC diagnostic, not merely "did not run",
// so the row cannot pass on some unrelated rejection.
BEZEL_TEST(generic_property_block_getter_inline_read_5315_sibling,
           bare_this_read_emits_a_verifiable_getter_call_spec_23_3) {
    JitResult r = runAot(k_bare_this);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("z\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

// Blast-radius guard. GREEN ON BASE (printed 5) — not a fail-without pin.
BEZEL_TEST(generic_property_block_getter_inline_read_5315_sibling,
           nongeneric_bare_this_control_unaffected_spec_23_3) {
    JitResult r = runAot(k_bare_this_nongeneric);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

// Blast-radius guard. GREEN ON BASE (printed `z 3`) — the auto-property form
// never took this path and must keep working.
BEZEL_TEST(generic_property_block_getter_inline_read_5315_sibling,
           auto_property_form_control_unaffected_spec_23_2) {
    JitResult r = runAot(k_autoprop_control);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("z\n3\n"));
}

// Blast-radius guard. GREEN ON BASE (printed `z 3`) — the expression-bodied
// form is inlined, not memoised, and must keep working.
BEZEL_TEST(generic_property_block_getter_inline_read_5315_sibling,
           expression_bodied_form_control_unaffected_spec_23_2) {
    JitResult r = runAot(k_exprbody_control);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("z\n3\n"));
}

// Blast-radius guard. GREEN ON BASE (printed `z`) — a method body already got
// its substitution; this pins that the fix did not change that.
BEZEL_TEST(generic_property_block_getter_inline_read_5315_sibling,
           plain_method_reading_same_field_control_unaffected_spec_23) {
    JitResult r = runAot(k_method_control);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("z\n"));
}