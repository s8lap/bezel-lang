// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/new_choice_construction_rejected_4163_test.cpp
//   (BEZEL_TEST sema_new_choice_construction_rejected_4163,
//    new_choice_with_class_payload_rejected_spec_35_1).
//
// §35.1/§35.2/§35.3 (v0.295, #4163): `new` cannot construct a choice — a
// choice has no constructors (§35.1); construction is case selection,
// spelled `Type.Case(args)` (§35.2/§35.3). `new ChoiceNCL(...)` was
// silently accepted before this fix, with zero diagnostic.

module M;
class WidgetNCL public method Render(): int return 42; end Render; end WidgetNCL;
public choice ChoiceNCL Has(H: WidgetNCL), Empty(); end ChoiceNCL;
class ProgramNCL public static method Main(): int var x := new ChoiceNCL(new WidgetNCL()); return 0; end Main; end ProgramNCL;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `new` cannot construct a choice — a choice has no constructors; a choice's construction is case selection, written `ChoiceNCL.Case(args)` instead of `new`
// CHECK-SAME: (§35.1/§35.2/§35.3)
