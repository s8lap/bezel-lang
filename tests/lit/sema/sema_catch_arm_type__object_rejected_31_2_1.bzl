// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail —
// see tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// §31.2.1 (v0.243, proposal 0080, #3046), verbatim: "`Object` itself is
// rejected by the same rule for the opposite reason — as the universal root
// ([§8.1]) it matches every throwable, and the spelling for that is the bare
// `Exception` arm ([§31.2.3]); the remedy is to delete the arm or fix the
// type." Appendix C.7: "an arm naming any other class type (`Object`
// included) is a compile error".
//
// The OPPOSITE reason matters, and is why this is its own file with its own
// text: `Dog` is rejected because NO throwable can ever match it (see
// sema_catch_arm_type__non_exception_class_rejected_31_2_1.bzl), `Object`
// because EVERY throwable does. A predicate that gave both the same wording
// would be stating something false about one of them.

module M3046B;

class C
  public static method Main(): int
    try
      throw new ArgumentException("x");
    catch
      Object => do return 1; end
    end;
    return 0;
  end Main;
end C;

// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: `Object` is not admitted as a `catch` arm type
// CHECK-SAME: (§31.2.1)
// CHECK-SAME: matches every throwable
// CHECK-SAME: bare `Exception` arm
