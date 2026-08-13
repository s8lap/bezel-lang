// RUN: %bezelc -c -o %t.lib.o %S/xmod_builtin_iface_3139_lib.bzl
// RUN: %not %bezelc -c %s --import %t.lib.o 2>&1 | FileCheck %s
//
// (`%S` = this test file's directory, so the first RUN line builds the
// dependency artifact the second imports. `%not` = LLVM's exit-status
// inverter; the compile is expected to fail — see tests/lit/README.md.)
//
// Issue #3139 / spec §18.5 (Interpolated strings) / §14.2 (Generic
// constraints) / §9.8 (Serialization) / §2.5 (imported declarations), v0.254 —
// the DIAGNOSTIC half of family F1 of #3152. The ACCEPT half is pinned with
// its VALUES in tests/module/xmod_builtin_iface_conformance_3139_test.cpp.
//
// WHY THIS FILE EXISTS, over and above the gtest arms. The gtest reject arms
// can only observe "the importer did not compile". The rejects below are ones
// where the WHICH matters and an exit code cannot say it:
//
//   * §14.2 `Min()` — #3139 moves the reject from a downstream #2770 backstop
//     to the #1198 gate that is supposed to catch it. That gate initialised its
//     verdict to `true` and only cleared it inside `if (td)`, so for an
//     IMPORTED element it was an unconditional ACCEPT — the over-accept whose
//     codegen consequence is the IR-verifier failure the gate exists to
//     prevent. MEASURED on this exact dependency, `arr.Min().V()` over the
//     imported non-comparable `Plain3139`:
//
//       in ISOLATION      before: `...(#2770); `Plain3139` does not — its
//                                 `CompareTo` could not be resolved for
//                                 dispatch.`      (exit 1)
//                          after: `...(#1198); Plain3139 does not implement
//                                 IComparable<T>.`  (exit 1)
//
//       IN THIS FILE      before: NO DIAGNOSTIC AT ALL for the Min case.
//                          after: the #1198 line, alongside the other two.
//
//     The second row is why this case lives here rather than alone. #2770's
//     backstop is a CODEGEN-time check, and codegen never runs once sema has
//     reported an error — so in any program that has some other sema error, the
//     pre-#3139 over-accept was covered by nothing whatsoever. In isolation the
//     two spellings both exit 1 and only the TEXT separates "fixed at the
//     intended gate" from "still caught by luck downstream"; in company, the
//     backstop is not even there to be lucky.
//
// ⚠ THE §9.8 ARM THAT USED TO BE THIRD HERE IS GONE, and its removal is the
// point rather than an omission. It pinned `no member `Serialize` on type
// `Pt3139`` — #3139's temporary `serializationSurfaceIsEmittable` interlock
// firing — with the note that it "must be REMOVED WITH the emitters". #3140
// (family F2) supplied the emitters' symbol-side component walk and the
// `serialized` clause's wire channel, deleted the interlock, and that call now
// COMPILES and round-trips. Its accept, with values, is
// tests/module/xmod_record_serialization_3140_test.cpp; the sibling gtest arm
// `imported_serialization_surface_still_gated_spec_9_8` flipped from a reject
// to 34 in the same commit. The §9.8 surface that is still closed is the CLASS
// one (#3179), pinned in that same #3140 file.
module AppF1Lit3139;
import LibF1Lit3139;

class C3139
  // §18.5 — a format spec on an imported class with NO conformance. This must
  // still be rejected after #3139: the fix must not blanket-accept imports.
  public static method BadFormat(): int
    using var p := new Plain3139(5);
    return (if $"{p:X}" = "x" then 42 else 91);
  end BadFormat;

  // §14.2 / #1198 — `Min()` without a selector over a NON-comparable imported
  // element. See above for why the text, not the exit code, is the assertion.
  public static method BadMin(): int
    using var a := new Plain3139(7);
    using var b := new Plain3139(3);
    var arr: Array<Plain3139> := [a, b];
    return arr.Min().V();
  end BadMin;

  public static method Main(): int
    return 0;
  end Main;
end C3139;

// bezelc emits diagnostics as `file:line:col: message` with no "error:"
// severity word, so anchor on the `:line:col:` shape plus the message text.

// §18.5 — the imported plain class is still neither intrinsically formattable
// nor IFormattable, and says so naming the type.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a format spec requires an intrinsically-formattable operand
// CHECK-SAME: or one implementing `IFormattable`; `Plain3139` is neither
// CHECK-SAME: 18.5

// §14.2 — the reject comes from the #1198 gate. If #3139's conversion of that
// gate regressed, this same program would still fail, but with the #2770
// backstop's "its `CompareTo` could not be resolved for dispatch" wording —
// which is why the issue number and the "does not implement" phrasing are both
// pinned here.
// CHECK: Min() without a selector requires the element type to implement IComparable<T>
// CHECK-SAME: #1198
// CHECK-SAME: Plain3139 does not implement IComparable<T>
