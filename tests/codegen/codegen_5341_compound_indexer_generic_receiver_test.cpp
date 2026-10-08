// fix #5341 (§14.1 "Generic declarations" / §23.4 "Indexers", v0.315):
// compound assignment through an indexer on a DIRECT generic receiver
// (`hs[0] += 1`, `hs: Holder<string>`) inlines the setter body
// (lowerIndexerCompoundCore) WITHOUT installing the receiver's §14.1
// substitution. The block-bodied indexer getter twin at #5339 was already
// fixed (and its comment explicitly noted the compound setter was NOT yet
// covered). This is that gap: the setter path at LlvmBackend_Assign.cpp
// lines ~4534-4560 saves `currentThis_`, `locals_`, `paramSlots_` before
// inlining the setter body but has NO call to `setupReceiverGenericSubst`
// (the #585/#902/#3347 mechanism). When the setter body accesses a T-typed
// field (`this._v: T`), `lowerFieldAddr` reads it as i32 (the generic
// template's erasure) instead of the concrete substituted layout, so the
// subsequent `do _w := value; end;` writes the `int` result of the
// compound op into the wrong field offset.
//
// Measured repro (r23-c28 67bab85c):
//   Holder<string>(_, 22)[0] += 1  →  prints 22 (expected 23) then
//   ASan SEGV in bz_string_release. Holder<long>(_, 22) → prints 22
//   (expected 23), exit 0. Holder<int>(_, 22) is correct (T->i32
//   is the substituted layout).
//
// THE FIX. Insert `setupReceiverGenericSubst(ix.target.get(), ps->parent)`
// before inlining the setter body, mirroring lowerIndexerSet's line 3500.
// Restore `genericSubst_` / `genericSubstSemaTy_` after the body, mirroring
// lowerIndexerSet's save/restore around line 2615.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Issue repro A: Holder<string>, block setter. Pre-fix: prints 22 (wrong,
// _w unchanged), then ASan SEGV in bz_string_release on program exit.
// Post-fix: prints 23.
constexpr const char* k_string_block_setter =
    "module M;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public _w: int;\n"
    "  public property [i: int]: int\n"
    "    get => _w + i;\n"
    "    set => do _w := value; end;\n"
    "  end;\n"
    "  public constructor(x: T, y: int) _v := x; _w := y; end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hs := new Holder<string>(\"z\", 22);\n"
    "    hs[0] += 1;\n"
    "    Console.WriteLine(hs._w);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// Issue repro B: Holder<long>, block setter. Pre-fix: prints 22 (wrong,
// _w unchanged), exit 0. Post-fix: prints 23.
constexpr const char* k_long_block_setter =
    "module M;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public _w: int;\n"
    "  public property [i: int]: int\n"
    "    get => _w + i;\n"
    "    set => do _w := value; end;\n"
    "  end;\n"
    "  public constructor(x: T, y: int) _v := x; _w := y; end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hl := new Holder<long>(9L, 22);\n"
    "    hl[0] += 1;\n"
    "    Console.WriteLine(hl._w);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// Issue repro C: Holder<int>, block setter. Pre-fix: correct (T->i32
// matches the substituted layout). Post-fix: unchanged.
constexpr const char* k_int_block_setter =
    "module M;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public _w: int;\n"
    "  public property [i: int]: int\n"
    "    get => _w + i;\n"
    "    set => do _w := value; end;\n"
    "  end;\n"
    "  public constructor(x: T, y: int) _v := x; _w := y; end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3, 22);\n"
    "    hi[0] += 1;\n"
    "    Console.WriteLine(hi._w);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// Control: expression-bodied setter. Pre-fix: ALSO wrong for the same reason
// (lowerIndexerCompoundCore inlines both block and expression setters without
// subst). Post-fix: prints 23.
constexpr const char* k_expr_setter =
    "module M;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public _w: int;\n"
    "  public property [i: int]: int\n"
    "    get => _w + i;\n"
    "    set => _w := value;\n"
    "  end;\n"
    "  public constructor(x: T, y: int) _v := x; _w := y; end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hs := new Holder<string>(\"z\", 22);\n"
    "    hs[0] += 1;\n"
    "    Console.WriteLine(hs._w);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

}  // namespace

// FIX ROW. FAIL-WITHOUT: 22 (wrong, _w unchanged) then ASan SEGV.
// Post-fix: 23.
BEZEL_TEST(codegen_5341_compound_indexer_generic_receiver,
           holder_string_block_setter) {
    JitResult r = runAot(k_string_block_setter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("23\n"));
    // ASan: no SEGV — the string field is not corrupted by the wrong-offset
    // write when the substitution is installed before the setter body.
    JitResult c = runAotChecked(k_string_block_setter);
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, 0);
    BEZEL_EXPECT_EQ(c.stdoutText, std::string("23\n"));
}

// FIX ROW. FAIL-WITHOUT: 22 (wrong, _w unchanged). Post-fix: 23.
BEZEL_TEST(codegen_5341_compound_indexer_generic_receiver,
           holder_long_block_setter) {
    JitResult r = runAot(k_long_block_setter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("23\n"));
    JitResult c = runAotChecked(k_long_block_setter);
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, 0);
    BEZEL_EXPECT_EQ(c.stdoutText, std::string("23\n"));
}

// CONTROL ROW (passes in every leg): T=int means T->i32 matches the
// substituted layout, so the absence of setupReceiverGenericSubst is
// harmless. Post-fix: unchanged (still 23).
BEZEL_TEST(codegen_5341_compound_indexer_generic_receiver,
           holder_int_block_setter_control) {
    JitResult r = runAot(k_int_block_setter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("23\n"));
    JitResult c = runAotChecked(k_int_block_setter);
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, 0);
    BEZEL_EXPECT_EQ(c.stdoutText, std::string("23\n"));
}

// FIX ROW. FAIL-WITHOUT: 22 (wrong, _w unchanged) then ASan SEGV.
// Post-fix: 23.
BEZEL_TEST(codegen_5341_compound_indexer_generic_receiver,
           holder_string_expr_setter) {
    JitResult r = runAot(k_expr_setter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("23\n"));
    JitResult c = runAotChecked(k_expr_setter);
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, 0);
    BEZEL_EXPECT_EQ(c.stdoutText, std::string("23\n"));
}
