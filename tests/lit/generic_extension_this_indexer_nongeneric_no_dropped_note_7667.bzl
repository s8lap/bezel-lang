// fix #7667 (§23.4 Indexers / §26.5 Resolution, v0.355): a NON-GENERIC
// extension on a BUILT-IN collection target whose body uses `this[i]` must
// compile with NO "internal: this `this[i]` indexer access was DROPPED"
// announcement (#2833), and the program's stdout must be exact.
//
// THE DEFECT. `indexAccessReceiverDecl` (src/codegen/LlvmBackend_Assign.cpp)
// announces the drop whenever the receiver TypeSymbol cannot be recovered.
// A built-in collection has NO TypeSymbol — the TypeRef's `declaration` is
// null — so for an extension on one the recovery cannot succeed BY
// CONSTRUCTION; the access is lowered by the COLLECTION indexer path instead
// (the receiver's sema type IS the extension's target type, which sema
// publishes for `this` inside a builtin extension body — #456) and the
// program's value is correct.
//
// #4160 covered the UNSPECIALIZED template body of a GENERIC extension
// (`extension Array<T>`) and #7561 its SPECIALIZATION. The NON-GENERIC block
// matched neither: its target type carries a CONCRETE element type, so
// #4160's "a generic argument no active substitution binds" gate cannot fire
// on it, and #7561's arm needs a specialization this block never forms.
//
// Two element kinds are exercised, because the issue names both:
// `extension Array<int>` (the write + compound write) and
// `extension Array<string>` (the read + the swap). The VALUES are the correct
// ones both before and after the fix — 11 / 7 for the int block (the
// announcement's "the read degrades to a zero/null value and a write is
// silently discarded" is the false part) and b / a for the string block.
//
// The NEGATIVE control — the announcement must STILL fire where the receiver
// genuinely cannot be recovered — is the sibling lit row
// generic_extension_this_indexer_nongeneric_dropped_note_announced_7667.bzl.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
// RUN: %bezelc -o %t.exe %s && %t.exe | FileCheck %s
module M;
extension Array<int>
  public method Bump()
    this[0] := this[0] + 10;
    this[1] += 5;
  end Bump;
end;
extension Array<string>
  public method Swap01()
    var t: string := this[0];
    this[0] := this[1];
    this[1] := t;
  end Swap01;
end;

public static class Program
  public static method Main(): int
    var xs: Array<int> := [1, 2, 3];
    var ys: Array<string> := ["a", "b"];
    xs.Bump();
    ys.Swap01();
    Console.WriteLine(xs[0]);
    Console.WriteLine(xs[1]);
    Console.WriteLine(ys[0]);
    Console.WriteLine(ys[1]);
    return 0;
  end Main;
end;

// CHECK: 11
// CHECK-NEXT: 7
// CHECK-NEXT: b
// CHECK-NEXT: a
// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: internal:
// NODIAG-NOT: DROPPED
