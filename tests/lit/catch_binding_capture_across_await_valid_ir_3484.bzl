// fix #3484 (§27.5 Closures / §30.9 / §31.2, v0.283): a lambda that captures a
// CATCH-ARM BINDING, in an arm that also `await`s, must still produce VALID IR.
//
// #3484 records each read of a capture-cell holder and patches the exposed ones
// at module end, falling back to the PRE-BOXING alloca recorded in
// `cellVarSyncSlot_`. Between the read's emission and that fixup,
// `promoteDemotedSlotsToFrame` (LlvmBackend_Async.cpp) does replaceAllUsesWith +
// eraseFromParent on every `asyncHandlerSpillSlots_` entry — and lowerTry
// registers the catch-arm binding's OWN alloca there when the arm awaits (§30.9
// / §31.2), which is exactly the alloca recorded as the fall-back slot. Holding
// it as a raw pointer made the fixup read freed memory:
//
//   Referring to an instruction in another function!
//     %cell.orslot = select i1 %cell.unset, ptr %2, ptr %cell9
//   bezelc: codegen error: IR verification failed
//
// Deterministic, 20 compiles out of 20. The verifier caught it only because LLVM
// had recycled that address into a DIFFERENT function's instruction; a
// same-function recycle would have verified clean and silently addressed
// whatever now lived there, and the deprecated in-process JIT harness skips
// `verifyModule` entirely — so neither oracle in the tree would have seen it.
//
// The fix holds the fall-back slot in a TRACKING handle and declines any read
// whose slot no longer resolves to an AllocaInst in the read's own function.
// This program therefore compiles; it is NOT asserted to run. Its runtime
// behaviour is the pre-#3484 one — the catch binding's holder gets no frame
// field, so reading it after the in-arm suspension dereferences null — a
// separate, pre-existing defect filed on its own. What is pinned here is only
// that the compiler emits well-formed IR for it.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
// NODIAG-NOT: IR verification failed
// NODIAG-NOT: Referring to an instruction in another function

module M;
import System;
class C
  public static async method Run(n: int): Task<int>
    var acc := 0;
    try
      if n > 0 then throw new InvalidOperationException("boom"); end if;
      acc := 1;
    catch
      var e: Exception => do
        var g := lambda(): int => e.Message.Length;
        await Task.Delay(1);
        acc := g() + e.Message.Length;
      end
    end try;
    return acc;
  end Run;
  public static async method Main(): Task<int>
    Console.WriteLine((await Run(1)).ToString());
    Console.WriteLine((await Run(0)).ToString());
    return 0;
  end Main;
end C;
