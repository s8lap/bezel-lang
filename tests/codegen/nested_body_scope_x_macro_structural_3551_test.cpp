// narrow #3551 (§29.3 / §30.1 / §30.7, v0.286) — STRUCTURAL REGRESSION PIN.
//
// WHAT THIS TEST PINS. `NestedBodyScope` (LlvmBackend_impl.hpp ~11378,
// ctor at LlvmBackend_Assign.cpp ~4802, dtor at ~4862) was historically a
// fourth, hand-maintained copy of the suspend/resume frame list — and the
// hand-maintained copy and the `BEZEL_SM_FRAME_MEMBERS` X-macro authority
// at LlvmBackend_impl.hpp ~11036 drifted: nine members were deliberately
// INHERITED (omitted from the ctor-init/ctor-clear/dtor-restore triple) and
// one member — `iterIsOwningIter_` (#2621) — was MISSING entirely. The fix
// generates NestedBodyScope's decl/ctor-init/ctor-clear/dtor-restore from
// the X-macro so adding a member to the macro cannot drift between the four
// sites. The asymmetry #3505 catalogues is structurally impossible here.
//
// WHY THIS IS A SOURCE-LEVEL TEST. The bug as described in the lane's first
// commit was UNREACHABLE from any real program: every write to
// `iterIsOwningIter_` lives inside `emitIteratorMethod`
// (LlvmBackend_Iterators.cpp ~1760 / ~1929), and every call to
// `emitIteratorMethod` from inside another body is wrapped in an
// `SmFrameSaveScope` (the #3505 fix at LlvmBackend_Calls2.cpp ~12208) that
// already saves/restores the field. A behavioural test of NestedBodyScope's
// own save/restore therefore cannot fail on the pre-fix compiler — no
// program can mutate `iterIsOwningIter_` between NestedBodyScope's entry
// and exit without an intervening `SmFrameSaveScope` that restores it.
// The behavioural cost of the bug is ZERO; the structural cost (drift
// between the hand list and the macro authority) is the whole defect class
// #3505 catalogues, and it is what this test pins.
//
// Specifically: the pre-fix compiler's NestedBodyScope uses 18 hand-written
// init/clear/restore lines and has no `BEZEL_SM_FRAME_NBSCOPE_*` macros;
// the post-fix compiler's NestedBodyScope uses the four
// `BEZEL_SM_FRAME_NBSCOPE_{DECL,INIT,CLEAR,RESTORE}` X-macro invocations
// and (by the construction of `BEZEL_SM_FRAME_MEMBERS`) handles every
// member of the macro in lockstep. The presence of all four
// `BEZEL_SM_FRAME_NBSCOPE_*` symbols in the source is the structural
// fingerprint of the fold.
//
// FAIL-WITHOUT / PASS-WITH. Reverting the patch to `d2957f8` removes all
// four macros from the source — the test then fails on all four rows below.
// Re-applying the patch restores them — the test passes. Measured on
// `d2957f8` (pre-fix): 0/4 GREEN (all four symbols absent).
// Measured on `59d992b` (post-fix): 4/4 GREEN.
//
// RELATED TEST. The behavioural pin for the §29.1 displaced-dispose shape
// that the fold guarantees is `nested_body_scope_iter_frame_clobber_3551`
// (the file adjacent to this one): three AOT rows that document the
// expected behaviour today and guarantee it stays correct when the next
// member is added. Those rows pass on both pre-fix and post-fix — the bug
// is structurally unreachable — and are the load-bearing behaviour pin;
// THIS file is the load-bearing STRUCTURAL pin.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "test_harness.hpp"

namespace fs = std::filesystem;

namespace {

// Read a whole file into a string. Returns empty on missing/unreadable.
std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Locate the file containing `NestedBodyScope`'s decl/ctor/dtor. The ctor and
// dtor live in `LlvmBackend_Assign.cpp`; the struct decl + the X-macro
// authority live in `LlvmBackend_impl.hpp`. We need BOTH for the structural
// pin (the X-macro authority is the LIST the fold is generated from; the
// ctor/dtor bodies are the GENERATED sites).
fs::path implPath() {
    // The test is run from the repo root by the harness.
    return fs::path("src") / "codegen" / "LlvmBackend_impl.hpp";
}
fs::path assignPath() {
    return fs::path("src") / "codegen" / "LlvmBackend_Assign.cpp";
}

}  // namespace

// The post-fix NestedBodyScope generates its member list via
// BEZEL_SM_FRAME_NBSCOPE_DECL — applied to BEZEL_SM_FRAME_MEMBERS. The
// pre-fix NestedBodyScope declared its members by hand. The presence of
// the DECL macro is the structural fingerprint of the fold.
BEZEL_TEST(nested_body_scope_x_macro_structural_3551,
           nested_body_scope_struct_uses_nbscope_decl_x_macro) {
    std::string impl = readFile(implPath());
    BEZEL_EXPECT_TRUE(!impl.empty());
    BEZEL_EXPECT_TRUE(impl.find("BEZEL_SM_FRAME_NBSCOPE_DECL") != std::string::npos);
}

// The post-fix NestedBodyScope ctor generates its init list via
// BEZEL_SM_FRAME_NBSCOPE_INIT. The pre-fix ctor spelled out the init by
// hand.
BEZEL_TEST(nested_body_scope_x_macro_structural_3551,
           nested_body_scope_ctor_uses_nbscope_init_x_macro) {
    std::string assign = readFile(assignPath());
    BEZEL_EXPECT_TRUE(!assign.empty());
    BEZEL_EXPECT_TRUE(assign.find("BEZEL_SM_FRAME_NBSCOPE_INIT") != std::string::npos);
}

// The post-fix NestedBodyScope ctor generates its clear block via
// BEZEL_SM_FRAME_NBSCOPE_CLEAR. The pre-fix ctor spelled out the clear by
// hand.
BEZEL_TEST(nested_body_scope_x_macro_structural_3551,
           nested_body_scope_ctor_uses_nbscope_clear_x_macro) {
    std::string assign = readFile(assignPath());
    BEZEL_EXPECT_TRUE(!assign.empty());
    BEZEL_EXPECT_TRUE(assign.find("BEZEL_SM_FRAME_NBSCOPE_CLEAR") != std::string::npos);
}

// The post-fix NestedBodyScope dtor generates its restore block via
// BEZEL_SM_FRAME_NBSCOPE_RESTORE. The pre-fix dtor spelled out the restore
// by hand.
BEZEL_TEST(nested_body_scope_x_macro_structural_3551,
           nested_body_scope_dtor_uses_nbscope_restore_x_macro) {
    std::string assign = readFile(assignPath());
    BEZEL_EXPECT_TRUE(!assign.empty());
    BEZEL_EXPECT_TRUE(assign.find("BEZEL_SM_FRAME_NBSCOPE_RESTORE") != std::string::npos);
}