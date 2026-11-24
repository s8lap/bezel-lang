// Codegen / memory-safety — issue #4426 (spec §27.5 Closures / §28.9
// Runtime-managed types / §27.3 Lambda expressions, v0.300):
// assigning a chained call to a local var `var w := Make()();` leaks the
// Widget returned by the chained call (the local's disposal-registration
// slot diverges from CG60's temp slot holding the Widget pointer).
//
// Distinguished from #4239 (closed via 8c36478f predicate-bypass mechanism
// in r18-c7): that issue's verbatim `return MakeClosure()();` double-free is
// a release-counting defect in the same CG60 arm; this #4426 leak is an
// orthogonal slot-routing defect. Same machinery, different symptom, both
// real and reproducible at HEAD pre-fix.
//
// MECHANICAL ROOT CAUSE. `lowerLocalDecl` (LlvmBackend_Setup.cpp) processes
// `var w := <expr>` by calling `lowerExpr(*init)` to evaluate the
// initializer. For a plain call like `var w := M()` returning an owned class
// instance, `lowerExpr` returns the instance pointer, `lowerLocalDecl` stores
// it in `w`'s slot, and registers `w` for scope-end disposal. For a chained
// call `var w := M()()`, `lowerExpr` for the outer CallExpr goes through
// the function-typed call path (LlvmBackend_Calls2.cpp CG60 arm), which calls
// `emitClosureCall` and then `emitClosureRelease` for the closure. However,
// the result of `emitClosureCall` (the Widget from the closure) is used
// DIRECTLY without being stored through `lowerLocalDecl`'s slot —
// `call cont4` shows the Widget stored to a temp, and `lowerLocalDecl`'s
// store goes to a DIFFERENT slot (w's slot). The disposal registration
// is on w's slot but the Widget lives in the temp.
//
// THE LEAK. The Widget is stored to a temp slot by CG60, and w's slot gets
// a SEPARATE (uninitialized or null) value. At scope end, disposal fires on
// w's slot (which is null/garbage), while the temp holding the real Widget
// is never disposed.
//
// THE FIX. The CG60 arm stores the Widget through the local's OWN slot
// (wired from lowerLocalDecl via pendingChainCallLocalSlot_) and registers
// that slot for scope-end disposal. A second store in lowerLocalDecl is
// harmless (same Widget pointer); no double-disposal occurs because the
// pending flag is consumed by CG60.
//
// MEASURED (BEZEL_DUMP_IR=1, HEAD vs fix):
//   pre-fix: main has NO disposal for w — Widget leaks (ASan: 8000 B at ×500)
//   post-fix: main has using.cls.dispose6 block — ASan clean
//
// TEST NOTE. `return Widget4239.n` evaluates n BEFORE scope-end disposal,
// so the return value is unreliable for distinguishing pre/post. The 500x
// row (FAIL-WITHOUT) is the reliable oracle: pre-fix leaks 8000 B and
// ASan exits 244; post-fix is ASan-clean (exit 0).
//
// ─────────────────────────────────────────────────────────────────────────
// THE FLIP (issue #9040, v0.359) — why the rows below no longer pin a
// leak.
//
// The row above pins a CHAIN CALL whose result is bound to a local. The
// program it compiles is:
//
//   public static method Make(): () -> Widget4239
//     return lambda(): Widget4239 => new Widget4239();
//   end Make;
//   public static method Main(): int
//     var w := C4239.Make()();
//     return 0;
//   end Main;
//
// That program is ILL-FORMED, and the #9040 fix made the compiler's verdict
// say so. §28.3.5 *The announcement table* row 4 reads, for a plain
// concrete class or interface return slot, "Borrow … **The result is never
// owned**", and its callee-side admission for a lambda into such a slot
// reads: "`null` (nullable slot), a capture-sourced borrow, a
// plain-parameter-sourced borrow, or static storage; **a fresh owned value
// rejected**". The program above mints a FRESH Widget into a plain class
// slot: the invocation's result is a borrow, so no caller ever disposes the
// minted frame — the frame leaks.
//
// The #1820 callee-side check that enforces that admission admitted a
// lambda's body only when the lambda had NO return-type ascription, and
// this program's lambda HAS one (`lambda(): Widget4239 => new
// Widget4239()`) — the ascription was a way past the check. The ascription
// does not change the slot's kind: the caller reads the TARGET's function
// type (`() -> Widget4239`), and the ascription only constrains the body's
// types against it. #9040's sema companion removes that gate, so the
// program is now REJECTED.
//
// CONSEQUENCE FOR THIS FILE. The leak this file pinned is no longer
// reachable: the only program that could reach it is now rejected at
// compile time, and the legal spellings do not leak. The rows below are
// therefore flipped: row 1 pins the REJECTION (RED on r30-c1, GREEN after
// the fix), and row 2 pins that the legal `owned` spelling is still
// accepted (a green control — the `owned` marker suppresses the
// callee-side check, §28.8's transferring slot).
//
// The runtime behaviour of the `owned` spelling (a chain call whose result
// the caller owns, bound to a local) is a SEPARATE, pre-existing defect —
// the result is disposed twice (the local's scope-end disposal and CG60's
// own temp-slot disposal) — and is filed separately; row 2 is a
// compile-time row for that reason.

#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/sema_support.hpp"        // parseAndCollect
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "test_harness.hpp"

using bezel::test::SemaResult;

namespace {

// Disposal-counted class: `_n` is the live count.
// v0.221 §22.12 rule 2 form (iii) rejects mutable static primitives,
// so AtomicInteger<int> (a class type) is used instead.
const char* kWidget =
    "class Widget4239 "
    "  private static readonly _n: AtomicInteger<int> := new AtomicInteger<int>(0); "
    "  public constructor() _n.Value := _n.Value + 1; end constructor; "
    "  destructor _n.Value := _n.Value - 1; end destructor; "
    "end Widget4239; ";

// The repro from issue #4426: a chained call `C.Make()()` assigned to a local
// where `Make` returns a closure that produces a fresh owned Widget. Row 4's
// callee-side admission rejects it (see the FLIP note above).
std::string chainedCallProgram() {
    return std::string("module M; ") + kWidget +
        "class C4239 "
        "  public static method Make(): () -> Widget4239 "
        "    return lambda(): Widget4239 => new Widget4239(); "
        "  end Make; "
        "  public static method Main(): int "
        "    var w := C4239.Make()(); "
        "    return 0; "
        "  end Main; "
        "end C4239;";
}

// The LEGAL spelling of the same chain call: the TARGET's function type
// carries the `owned` marker, so the announcement table's row 1 applies
// (the caller owns the result) and the callee-side check is suppressed.
std::string ownedTargetProgram() {
    return std::string("module M; ") + kWidget +
        "class C4239 "
        "  public static method Make(): () -> owned Widget4239 "
        "    return lambda(): Widget4239 => new Widget4239(); "
        "  end Make; "
        "  public static method Main(): int "
        "    var w := C4239.Make()(); "
        "    return 0; "
        "  end Main; "
        "end C4239;";
}

// Lex + parse + resolve + typecheck, in the driver's own staging order.
SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasMsg(const SemaResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// Row 1 (FLIPPED — RED on r30-c1): the #4426 program's closure mints a
// FRESH Widget into a plain class/interface slot, which §28.3.5 row 4's
// callee-side admission rejects ("a fresh owned value rejected"): the
// function-value invocation's result is a borrow, so no caller would ever
// dispose the minted frame. Pre-fix the ASCRIPTION on the lambda was a way
// past the check and the program compiled (and leaked).
BEZEL_TEST(chained_call_local_var_leak_4426,
           fresh_owned_into_plain_slot_rejected_4426) {
    auto r = parseSemaAll(chainedCallProgram());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "fresh owned value"));
}

// Row 2 (CONTROL — GREEN on r30-c1 and after the fix): the legal `owned`
// spelling is still accepted — the `owned` marker on the TARGET's function
// type suppresses the callee-side check (§28.8's transferring slot), so the
// #9040 sema companion does not over-reject.
BEZEL_TEST(chained_call_local_var_leak_4426,
           owned_target_spelling_accepted_4426) {
    auto r = parseSemaAll(ownedTargetProgram());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
