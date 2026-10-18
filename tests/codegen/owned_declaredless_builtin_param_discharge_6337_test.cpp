// Spec-derived regression tests for issue #6337 — an `owned` PARAMETER whose
// type is a DECLARATION-LESS language built-in (the §31.4 exception hierarchy:
// `Exception`, `IOException`, `InvalidOperationException`, …) was never
// discharged at the callee boundary, so the object the callee owns leaked.
//
// ROOT CAUSE (src/codegen/LlvmBackend_Setup.cpp, `registerUnwindOwnedParam`).
// §28.8 makes the holder of an owned reference responsible for disposing the
// referenced object exactly once, and for a PARAMETER the callee is that
// holder: the caller transfers ownership into the callee on entry and the
// callee discharges it (explicit `Dispose()`, `move`, `return`, or §28.3's
// must-use auto-drop). `registerUnwindOwnedParam` is where that discharge is
// registered, and it gated on
//
//     isClassOrInterfaceRef(rt)   // Named && declaration && (Class||Interface)
//
// A §31.4 language-built-in exception carries NO user `TypeSymbol`, so its
// `sema::TypeRef::declaration` is `nullptr` and the predicate answered "no" for
// every `owned x: Exception`. No `Disposal` entry was ever pushed onto
// `disposeStack_.front()`, so neither the auto-drop's normal-exit dispose nor
// its unwind-edge dispose existed: the value was owned by nobody after entry
// and leaked.
//
// This is the CALLEE-side mirror of the caller-side defect #6311 fixed in
// `buildOrderedArgs` (LlvmBackend_Assign.cpp), which gated the ARGUMENT-side
// temporary disposal on the same `->declaration` read. The two are different
// sites and different directions: #6311 disposed a temporary the CALLER owned
// at the call boundary; this disposes the value the CALLEE takes ownership of.
// A fix on either side alone leaves the other end leaking, which is why both
// halves need the same widening.
//
// THE FIX. The gate now also admits `isBuiltinExceptionTypeRef(rt)` (and its
// `resolveTypeParamRef(rt)` twin) — the named recognizer the owned-field /
// using-var auto-dispose paths and #6311's caller-side gate already use for this
// same population. §31.1 makes a built-in exception an ordinary owned heap
// class instance, and `setupBuiltinExceptions` mints every §31.4 member a real
// vtable whose slot 2 is `bz$exDisposeMsg` — precisely what the
// `dynamicDispose` this registration drives dispatches through, so the dispose
// reaches the message buffer as well as the object.
//
// SCOPE — deliberately narrow, the same predicate as the sibling sites:
// `isBuiltinExceptionTypeRef` requires a declaration-less Named `TypeRef` whose
// astNode NAME resolves in `builtinVtable_`, i.e. exactly the §31.4 hierarchy.
// A vtable-less built-in (a collection, a `Task`, a `FileStream`, a `Mutex`) is
// not in `builtinVtable_` and stays excluded, which is what keeps the
// word-0-as-vtable read in `dynamicDispose` away from a plain C struct.
//
// MEASURED fail-without (this tree with the fix reverted, `bezelc` rebuilt from
// the reverted source, LSan `detect_leaks=1:exitcode=99:abort_on_error=0`, the
// recipe `runAotChecked` runs):
//   * `owned_Exception_param_dropped_autodispose_spec_28_3` and
//     `owned_IOException_param_dropped_autodispose_spec_31_4`:
//     `SUMMARY: AddressSanitizer: 19960 byte(s) leaked in 499 allocation(s).`,
//     exit 99 -> `ran == false` -> -999.
//   * `owned_Exception_param_callee_throws_unwind_edge_spec_28_3` (the row the
//     #6311 review found missing): `SUMMARY: AddressSanitizer: 80000 byte(s)
//     leaked in 2000 allocation(s).`, exit 99 -> -999.
// With the fix all three are LSan-clean and return their spec-derived value.
//
// BOTH DIRECTIONS. Three rows pin the OVER-release side, i.e. that the widened
// gate did not make the callee dispose a value it does not own:
//   * `owned_param_explicit_dispose_then_return_no_double_free` — an explicit
//     `x.Dispose()` discharges the parameter, so the scope-end auto-drop must
//     find a nulled slot rather than free again (a second free is an ASan
//     `heap-use-after-free` / double-free).
//   * `owned_param_forwarded_move_out_no_double_free` — the callee `move`s the
//     parameter into its RETURN value, transferring ownership to the caller's
//     `using var`. Both edges are live here; a callee that also auto-dropped
//     would free the caller's live object.
//   * `plain_param_Exception_borrowed_is_never_disposed` — a plain (borrow)
//     `x: Exception` parameter has the same declaration-less type, so widening
//     the gate without keeping the `owned`-modifier precondition would dispose a
//     value the CALLEE DOES NOT OWN: the caller's `using var` is still the
//     holder (§28.8) and would free the object a second time.
//
// Spec: §28.3 *Unbound owned temporaries* and the must-use auto-drop, §28.8
// (only an owned reference carries the disposal obligation; `ref`/`out` do
// not), §28.9 (runtime-managed types), §31.1 (built-in types are ordinary
// owned heap class instances), §31.4 (the standard exception hierarchy),
// §22.3 (`ref`/`out` argument locations). Revision v0.338.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A leak / double-free / use-after-free flips r.ran to false -> -999, distinct
// from any legal exit value. `expectRanWith` is the ONLY runAotChecked call
// site in this file, and it does not absorb a toolchain skip: a skip makes the
// harness return `ran = false` (aot_detail::markInfra), which the
// `BEZEL_EXPECT_TRUE(r.ran)` below turns into a row failure rather than a
// silent pass.
// The rows assert BOTH halves: `ran` says the program ran to normal
// termination under ASan/LSan (no leak, no memory error), and the exit code
// says it computed the spec-derived value. Either half alone is satisfiable
// vacuously — a `didNotCrash`-only row passes on a program that computes the
// wrong answer, and a value-only row can be satisfied by a sentinel.
void expectRanWith(const std::string& src, int expected) {
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, expected);
}

}  // namespace

// §28.3 must-use auto-drop / §31.4 (#6337) — the headline shape. `Take`'s
// `owned x: Exception` is never moved / returned / disposed, so §28.3's
// must-use rule auto-drops it at the end of the method — on BOTH the normal and
// the unwind edge. `Exception` is the ROOT of the §31.4 hierarchy, so this row
// also pins that the widening is not limited to the derived names.
//
// 500 iterations x 1 = 500; 500 mod 251 = 249.
// Pre-fix: LSan `19960 byte(s) leaked in 499 allocation(s)`, exit 99 -> -999.
// Post-fix: LSan-clean, 249.
BEZEL_TEST(codegen_owned_declaredless_builtin_param_discharge_6337,
           owned_Exception_param_dropped_autodispose_spec_28_3) {
    std::string src =
        "module M; "
        "class Q "
        "  public static method Take(owned x: Exception): int return 1; end Take; "
        "end Q; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do t := t + Q.Take(new Exception(\"abc\")); end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 249);
}

// §31.4 (#6337) — a DERIVED hierarchy member, and the callee READS the message
// through the parameter before the auto-drop. That read pins the disposal is
// established at METHOD END, not at parameter binding: an over-eager dispose on
// entry would null the message slot and read a freed buffer.
//
// 500 iterations x 3 = 1500; 1500 mod 251 = 245.
// Pre-fix: LSan `19960 byte(s) leaked in 499 allocation(s)`, exit 99 -> -999.
// Post-fix: LSan-clean, 245.
BEZEL_TEST(codegen_owned_declaredless_builtin_param_discharge_6337,
           owned_IOException_param_dropped_autodispose_spec_31_4) {
    std::string src =
        "module M; "
        "class Q "
        "  public static method Take(owned e: IOException): int "
        "    return e.Message.Length; "
        "  end Take; "
        "end Q; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    t := t + Q.Take(new IOException(\"abc\")); "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 245);
}

// §28.3 auto-drop / §31.4 (#6337) — THE UNWIND EDGE. `Take` throws BEFORE it
// ever touches `x`, so the only path that can release the owned parameter is
// the §28.3 auto-drop's unwind-edge disposal. This is the row the #6311 review
// found missing from the shipped caller-side tests: a caller-side-only reading
// of "the callee discharges it" would not have covered the callee throwing
// before consuming, and neither edge would be pinned.
//
// The catch is by the SPECIFIC type (`InvalidOperationException`, the one
// `Take` raises), so the row also pins that the disposal did not corrupt the
// exception machinery on the way out — the catch still has to fire 2000 times
// for the counter to reach its expected value.
//
// 2000 iterations x 1 = 2000; 2000 mod 251 = 243.
// Pre-fix: LSan `80000 byte(s) leaked in 2000 allocation(s)`, exit 99 -> -999.
// Post-fix: LSan-clean, 199.
BEZEL_TEST(codegen_owned_declaredless_builtin_param_discharge_6337,
           owned_Exception_param_callee_throws_unwind_edge_spec_28_3) {
    std::string src =
        "module M; "
        "class Q "
        "  public static method Take(owned x: Exception): int "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Take; "
        "end Q; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<2000 do "
        "    try "
        "      Q.Take(new Exception(\"abc\")); "
        "    catch "
        "      InvalidOperationException => t := t + 1; "
        "    end try; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 243);
}

// §28.3 / §28.8 (#6337) — OVER-RELEASE GUARD, the discharge half. The callee
// disposes the parameter explicitly and returns normally. The auto-drop must
// then find the nulled slot; disposing again is a double free, which ASan
// reports as `heap-use-after-free` on the second free.
//
// Pre-fix this row is ALREADY clean (the explicit Dispose is what released it),
// so it is a control, not a fail-without row. That is the point: it is what
// would turn RED if the widened gate double-fired.
//
// 2000 iterations x 1 = 2000; 2000 mod 251 = 243.
BEZEL_TEST(codegen_owned_declaredless_builtin_param_discharge_6337,
           owned_param_explicit_dispose_then_return_no_double_free) {
    std::string src =
        "module M; "
        "class Q "
        "  public static method Take(owned x: Exception): int "
        "    x.Dispose(); "
        "    return 1; "
        "  end Take; "
        "end Q; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<2000 do t := t + Q.Take(new Exception(\"abc\")); end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 243);
}

// §22.3 / §28.3 (#6337) — OVER-RELEASE GUARD, the FORWARDING half. `Take`
// `move`s the owned parameter into its RETURN value, so ownership leaves the
// callee entirely and the caller's `using var` becomes the sole owner. A
// callee that also ran the auto-drop would free the object the caller is still
// holding — a use-after-free the moment the caller touches it, which is why
// this row READS the returned exception's message inside the same full
// expression.
//
// Pre-fix this row is ALREADY clean (the caller disposed the moved value), so
// it is a control, not a fail-without row.
//
// 2000 iterations x 3 = 6000; 6000 mod 251 = 227.
BEZEL_TEST(codegen_owned_declaredless_builtin_param_discharge_6337,
           owned_param_forwarded_move_out_no_double_free) {
    std::string src =
        "module M; "
        "class Q "
        "  public static method Take(owned x: Exception): Exception return move x; end Take; "
        "end Q; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<2000 do "
        "    using var e := Q.Take(new IOException(\"abc\")); "
        "    t := t + e.Message.Length; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 227);
}

// §22.3 / §28.8 (#6337) — OVER-RELEASE GUARD, the BORROW half, and the one
// that most directly attacks a too-wide fix. `x: Exception` — a plain, non-
// `owned`, non-`ref` parameter — has the SAME declaration-less type as the rows
// above, so a fix that widened the gate without keeping the `owned`-modifier
// precondition would dispose a value the CALLEE DOES NOT OWN: §28.8 gives the
// obligation to the holder, and at a borrow parameter the holder is still the
// CALLER. Disposing it would free the caller's still-live object, which its own
// `using var` scope-end dispose then frees again — a double free.
//
// (§22.3 forbids passing an owned or `using var` binding to a `ref` parameter
// outright, so a `ref e: Exception` receiver cannot be spelled from a local
// owner at all; the plain borrow parameter is the borrow shape that reaches
// this gate, and it is the one a widened gate would wrongly admit.)
//
// Pre-fix this row is ALREADY clean (no disposal was ever registered for a
// non-`owned` parameter), so it is a control, not a fail-without row.
//
// 2000 iterations x 3 = 6000; 6000 mod 251 = 227.
BEZEL_TEST(codegen_owned_declaredless_builtin_param_discharge_6337,
           plain_param_Exception_borrowed_is_never_disposed) {
    std::string src =
        "module M; "
        "class Q "
        "  public static method Take(x: Exception): int return x.Message.Length; end Take; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<2000 do "
        "      using var e := new IOException(\"abc\"); "
        "      t := t + Take(e); "
        "    end for; "
        "    return t mod 251; "
        "  end Main; "
        "end Q; "
        "class P end P;";
    expectRanWith(src, 227);
}