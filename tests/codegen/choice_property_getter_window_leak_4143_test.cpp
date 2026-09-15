// fix #4143 — `currentChoiceMethod_` leaked across a property-getter
// evaluation window into an UNRELATED nested accessor's own `case this of`
// body, silently reclassifying its scrutinee and picking the WRONG arm
// (spec §35.2/§35.5, v0.295). Filed by the R17 review of `fix #4113` (which
// added `currentChoiceMethod_` without protecting it from this leak).
//
// ── ROOT CAUSE (measured, not the issue's original hypothesis) ───────────
// The filed issue (#4143) hypothesized the leak reached through
// `NestedBodyScope` failing to clear `currentChoiceMethod_` around
// `ensurePropertyBlockGetter`. Traced with `BEZDBG`-style instrumentation
// (removed before this commit) against the issue's own repro: `Animal.Sound`
// there is `get => (case this of ...)` — an EXPRESSION-bodied accessor
// inside a block-form property declaration, which `computedGetterOf`
// classifies as an ordinary INLINE getter read (LlvmBackend_Exprs.cpp),
// NOT a block getter (`ensurePropertyBlockGetter` is reached only by a
// `get => do ... end;` STATEMENT body — LlvmBackend_Assign.cpp
// `blockGetterOf`). So the issue's own repro reproduces the bug through a
// DIFFERENT one of #4113's "three sites" than the one it named.
//
// The real defect at all three of #4113's sites (LlvmBackend_Exprs.cpp's
// inline getter read, LlvmBackend_Layout.cpp's `makePropertyThunk`,
// LlvmBackend_Assign.cpp's `ensurePropertyAccessorFn`) is the same shape:
// each does `if (owner->kind == Choice) currentChoiceMethod_ = owner;` with
// NO ELSE — so evaluating ANY property's getter (regardless of the
// property's OWN owning type) leaves a caller's already-set
// `currentChoiceMethod_` untouched rather than establishing its own
// identity (null, if not a choice property). A choice property's getter
// window (site 1 or site 3) that first-touches an UNRELATED class/record
// property nested inside it hands that nested body the OUTER choice type.
// `lowerCaseExpr`'s scrutinee recovery (LlvmBackend_Exprs2.cpp) then
// destructively OVERWRITES the nested body's own (correctly null, since
// sema attaches no exprType to any named-type `this`) scrutinee TypeRef
// with the leaked choice type whenever `choiceTypeOf` returns non-null —
// silently reclassifying the nested `this` and sending `patternMatches`
// hunting for the WRONG choice's cases.
//
// A structurally identical defect exists in `makePropertyThunk`
// (LlvmBackend_Layout.cpp): it is a genuinely separate, memoized function
// (like the six `NestedBodyScope`-guarded builders) but predates that guard
// and hand-rolls its own save/restore list, so `NestedBodyScope`'s fix does
// not reach it — it needed the identical SET-FROM-OWNER treatment
// independently (T4 below measures this in isolation).
//
// `NestedBodyScope` itself (which DOES guard `ensurePropertyBlockGetter`,
// `ensureIndexerBlockGetter`, `ensurePropertyAccessorFn`,
// `ensureIndexerGetterFn`, `ensureIndexerSetterFn`, `ensureExprGetterThunk`)
// was ALSO missing `currentChoiceMethod_` from its save/clear/restore set —
// exactly as #4143 filed — and that omission is independently measurable
// through a genuinely STATEMENT-bodied (`get => do ... end;`) property or
// indexer (T6/T7 below): those two builders have no "set-from-owner" logic
// of their own at all (a CLASS/record can never BE Choice or Enum, so
// nothing there conditionally sets the field either way), so they rely
// entirely on `NestedBodyScope` establishing a clean (null) baseline.
//
// ── THE FIX ────────────────────────────────────────────────────────────
// 1. `currentChoiceMethod_` (LlvmBackend_impl.hpp) is now saved, cleared to
//    null, and restored by `NestedBodyScope` (LlvmBackend_Assign.cpp),
//    exactly as #4143 asked, as `choiceMethod_`.
// 2. All three of #4113's property-accessor sites (LlvmBackend_Exprs.cpp,
//    LlvmBackend_Layout.cpp, LlvmBackend_Assign.cpp) now SET-FROM-OWNER
//    unconditionally: `currentChoiceMethod_ = (owner->kind == Choice) ?
//    owner : nullptr;` instead of conditionally-set-else-inherit.
//
// `currentEnumMethod_` is DELIBERATELY left untouched (declined, not
// forgotten) — see the `NestedBodyScope` struct's `choiceMethod_` member
// comment and the "DELIBERATELY INHERITED" list in LlvmBackend_Assign.cpp
// for the full reasoning: unlike choice's leak, an enum's leaked value does
// not destructively overwrite the CaseExpr's own scrutinee TypeRef (there
// is no enum-side analogue of choice's "scrutTy not already a Choice"
// overwrite guard) — it only feeds a bare enum-constant NAME lookup
// (`enumConstantByName`) alongside the (correctly resolved or correctly
// null) scrutinee TypeRef, so a wrong value there is a soft miss, not a
// silent misclassification. T9/T10 below are accept-side controls proving
// this composition is unaffected either way.
//
// ── WHAT EACH ROW IS FOR (all runAot, asserting the RUNTIME VALUE — the
// program compiles and runs on both sides of this fix; only the answer
// differs, so a compile-success assertion alone would be vacuous) ────────
//   T1  the filed issue's own repro, AS MEASURED: a CLASS property
//       (`Animal.Sound`, `get => (case this of ...)`, an inline getter read
//       per the root-cause note above) first-touched from inside a CHOICE
//       property's getter window. Site 1.
//   T2  ACCEPT-SIDE CONTROL: the identical class hierarchy/property read
//       with NO choice involved at all.
//   T3  ACCEPT-SIDE CONTROL: the identical nested read wrapped in a plain
//       CLASS property's getter instead of a choice's — proves the
//       corruption is CHOICE-WINDOW-specific, not "any nested getter call".
//   T4  site 2 (`makePropertyThunk`): the nested class property read AS A
//       FUNCTION VALUE from inside a choice property's window — measures
//       the thunk's OWN independent leak (T4 fails if ONLY the
//       `NestedBodyScope`/site-1 fixes land, since this builder is neither).
//   T5  site 3 (`ensurePropertyAccessorFn`): the nested class property
//       belongs to a type reached through an INTERFACE-typed receiver
//       inside a choice's window, forcing the itable accessor build.
//       MEASURED to pass on BOTH sides of this fix — not a mistake:
//       `ensurePropertyAccessorFn`'s itable accessor is built EAGERLY by
//       `declareVtables()` (LlvmBackend_Setup.cpp) for every ordinary
//       (non-generic) interface implementation, in a pass that runs before
//       ANY method or property body — including a choice's own getter — is
//       ever lowered, so this construction cannot reach the accessor's
//       FIRST build from inside an active choice window regardless of
//       fix/no-fix. Retained as a correctness/anti-regression check for the
//       interface-dispatch path (and as a record of why this axis, unlike
//       T1/T4/T6/T7, could not be made to fail without the fix); the fix's
//       own protection for this builder (`NestedBodyScope`'s clear) is
//       proven independently by T6/T7's isolation measurement instead (see
//       the commit message).
//   T6  `ensurePropertyBlockGetter` (a genuinely STATEMENT-bodied
//       `get => do ... end;` class property) first-touched from inside a
//       choice's window — measures the `NestedBodyScope` clear in
//       isolation (this builder has no set-from-owner code of its own).
//   T7  `ensureIndexerBlockGetter` (an indexer's `get => do ... end;`)
//       first-touched from inside a choice's window — same isolation as T6
//       for the indexer twin.
//   T8  ANTI-REGRESSION: fix #4113's own recovery — a choice property's
//       `case this of` with NO nested leak scenario — still resolves
//       correctly (self-contained here; also covered by
//       choice_property_case_this_crash_4113_test.cpp, unmodified).
//   T9  ACCEPT-SIDE CONTROL: an ENUM property's `case this of` (the #574/
//       CG30 precedent, sharing `computedGetterOf` with T1's class case)
//       first-touched from inside a CHOICE property's window — proves the
//       enum precedent is untouched by this fix.
//   T10 ACCEPT-SIDE CONTROL: the reverse composition — a CLASS property's
//       `case this of` first-touched from inside an ENUM property's window
//       — proves declining `currentEnumMethod_` does not regress this
//       ordinary (non-colliding-name) composition.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// T1 — the filed issue's own repro, measured through its REAL call site
// (an inline getter read, not ensurePropertyBlockGetter as originally
// hypothesized — see the root-cause note above).
BEZEL_TEST(codegen_choice_property_getter_window_leak_4143,
           class_property_case_this_nested_in_choice_property_window_spec_35_2_35_5) {
    const std::string program =
        "module M4143A; "
        "class Animal4143A "
        "  public property Sound4143A: string "
        "    get => (case this of Dog4143A => \"Woof\", Cat4143A => \"Meow\", _ => \"...\"); "
        "  end Sound4143A; "
        "end Animal4143A; "
        "class Dog4143A: Animal4143A end Dog4143A; "
        "class Cat4143A: Animal4143A end Cat4143A; "
        "choice ShapeQ4143A "
        "  CircleQ4143A(radius: double), SquareQ4143A(side: double); "
        "  public property Kind4143A: string => "
        "    (case this of CircleQ4143A(r) => new Dog4143A().Sound4143A, "
        "                  SquareQ4143A(s) => \"square\"); "
        "end ShapeQ4143A; "
        "class C "
        "  public static method Main(): int "
        "    var s := ShapeQ4143A.CircleQ4143A(1.0); "
        "    return (if s.Kind4143A = \"Woof\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runProg(program), 42);
}

// T2 — ACCEPT-SIDE CONTROL: the same class hierarchy, no choice involved.
BEZEL_TEST(codegen_choice_property_getter_window_leak_4143,
           class_property_case_this_standalone_no_choice_accept_control) {
    const std::string program =
        "module M4143B; "
        "class Animal4143B "
        "  public property Sound4143B: string "
        "    get => (case this of Dog4143B => \"Woof\", Cat4143B => \"Meow\", _ => \"...\"); "
        "  end Sound4143B; "
        "end Animal4143B; "
        "class Dog4143B: Animal4143B end Dog4143B; "
        "class Cat4143B: Animal4143B end Cat4143B; "
        "class C "
        "  public static method Main(): int "
        "    return (if new Dog4143B().Sound4143B = \"Woof\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runProg(program), 42);
}

// T3 — ACCEPT-SIDE CONTROL: the same nested read wrapped in a plain CLASS
// property's getter (not a choice's) — proves this is choice-window
// specific, not "any nested getter call corrupts the callee".
BEZEL_TEST(codegen_choice_property_getter_window_leak_4143,
           class_property_case_this_nested_in_class_property_window_accept_control) {
    const std::string program =
        "module M4143C; "
        "class Animal4143C "
        "  public property Sound4143C: string "
        "    get => (case this of Dog4143C => \"Woof\", Cat4143C => \"Meow\", _ => \"...\"); "
        "  end Sound4143C; "
        "end Animal4143C; "
        "class Dog4143C: Animal4143C end Dog4143C; "
        "class Cat4143C: Animal4143C end Cat4143C; "
        "class Wrapper4143C "
        "  public property Kind4143C: string => "
        "    (if true then new Dog4143C().Sound4143C else \"square\"); "
        "end Wrapper4143C; "
        "class C "
        "  public static method Main(): int "
        "    return (if new Wrapper4143C().Kind4143C = \"Woof\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runProg(program), 42);
}

// T4 — site 2 (`makePropertyThunk`): the nested class property is read AS A
// FUNCTION VALUE from inside a choice property's window. This builder is
// NOT one of the six `NestedBodyScope`-guarded emitters (it predates that
// guard and hand-rolls its own save/restore), so it needed its OWN
// set-from-owner fix independently — measured in isolation (with ONLY this
// site's fix reverted) to fail while T1 still passes.
BEZEL_TEST(codegen_choice_property_getter_window_leak_4143,
           class_property_read_as_function_value_nested_in_choice_property_window_spec_27_6) {
    const std::string program =
        "module M4143D; "
        "class Animal4143D "
        "  public property Sound4143D: string "
        "    get => (case this of Dog4143D => \"Woof\", Cat4143D => \"Meow\", _ => \"...\"); "
        "  end Sound4143D; "
        "end Animal4143D; "
        "class Dog4143D: Animal4143D end Dog4143D; "
        "class Cat4143D: Animal4143D end Cat4143D; "
        "class Helper4143D "
        "  public static method Invoke0(f: () -> string): string => f(); "
        "end Helper4143D; "
        "choice ShapeQ4143D "
        "  CircleQ4143D(radius: double), SquareQ4143D(side: double); "
        "  public property Kind4143D: string => "
        "    (case this of CircleQ4143D(r) => "
        "        Helper4143D.Invoke0(new Dog4143D().Sound4143D), "
        "      SquareQ4143D(s) => \"square\"); "
        "end ShapeQ4143D; "
        "class C "
        "  public static method Main(): int "
        "    var s := ShapeQ4143D.CircleQ4143D(1.0); "
        "    return (if s.Kind4143D = \"Woof\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runProg(program), 42);
}

// T5 — site 3 (`ensurePropertyAccessorFn`): the nested class property is
// read through an INTERFACE-typed receiver from inside a choice property's
// window, forcing the standalone itable accessor build.
BEZEL_TEST(codegen_choice_property_getter_window_leak_4143,
           class_property_via_interface_receiver_nested_in_choice_property_window_spec_35_8) {
    const std::string program =
        "module M4143E; "
        "interface ISound4143E readonly property Sound4143E: string; end ISound4143E; "
        "class Animal4143E: ISound4143E "
        "  public property Sound4143E: string "
        "    get => (case this of Dog4143E => \"Woof\", Cat4143E => \"Meow\", _ => \"...\"); "
        "  end Sound4143E; "
        "end Animal4143E; "
        "class Dog4143E: Animal4143E end Dog4143E; "
        "class Cat4143E: Animal4143E end Cat4143E; "
        "class Helper4143E "
        "  public static method ReadSound(): string "
        "    var d: ISound4143E := new Dog4143E(); "
        "    return d.Sound4143E; "
        "  end ReadSound; "
        "end Helper4143E; "
        "choice ShapeQ4143E "
        "  CircleQ4143E(radius: double), SquareQ4143E(side: double); "
        "  public property Kind4143E: string => "
        "    (case this of CircleQ4143E(r) => Helper4143E.ReadSound(), "
        "                  SquareQ4143E(s) => \"square\"); "
        "end ShapeQ4143E; "
        "class C "
        "  public static method Main(): int "
        "    var s := ShapeQ4143E.CircleQ4143E(1.0); "
        "    return (if s.Kind4143E = \"Woof\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runProg(program), 42);
}

// T6 — `ensurePropertyBlockGetter` (a genuinely STATEMENT-bodied
// `get => do ... end;` class property) first-touched from inside a choice
// property's window. This builder has no set-from-owner code of its own —
// it relies entirely on `NestedBodyScope` clearing `currentChoiceMethod_`.
BEZEL_TEST(codegen_choice_property_getter_window_leak_4143,
           class_block_statement_property_case_this_nested_in_choice_property_window_spec_23_3) {
    const std::string program =
        "module M4143F; "
        "class Animal4143F "
        "  public property Sound4143F: string "
        "    get => do "
        "      return (case this of Dog4143F => \"Woof\", Cat4143F => \"Meow\", _ => \"...\"); "
        "    end; "
        "  end Sound4143F; "
        "end Animal4143F; "
        "class Dog4143F: Animal4143F end Dog4143F; "
        "class Cat4143F: Animal4143F end Cat4143F; "
        "choice ShapeQ4143F "
        "  CircleQ4143F(radius: double), SquareQ4143F(side: double); "
        "  public property Kind4143F: string => "
        "    (case this of CircleQ4143F(r) => new Dog4143F().Sound4143F, "
        "                  SquareQ4143F(s) => \"square\"); "
        "end ShapeQ4143F; "
        "class C "
        "  public static method Main(): int "
        "    var s := ShapeQ4143F.CircleQ4143F(1.0); "
        "    return (if s.Kind4143F = \"Woof\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runProg(program), 42);
}

// T7 — `ensureIndexerBlockGetter` (an indexer's `get => do ... end;`)
// first-touched from inside a choice property's window — the indexer twin
// of T6, same isolation rationale.
BEZEL_TEST(codegen_choice_property_getter_window_leak_4143,
           class_block_statement_indexer_case_this_nested_in_choice_property_window_spec_23_4) {
    const std::string program =
        "module M4143G; "
        "class Animal4143G "
        "  public property [i: int]: string "
        "    get => do "
        "      return (case this of Dog4143G => \"Woof\", Cat4143G => \"Meow\", _ => \"...\"); "
        "    end; "
        "  end; "
        "end Animal4143G; "
        "class Dog4143G: Animal4143G end Dog4143G; "
        "class Cat4143G: Animal4143G end Cat4143G; "
        "choice ShapeQ4143G "
        "  CircleQ4143G(radius: double), SquareQ4143G(side: double); "
        "  public property Kind4143G: string => "
        "    (case this of CircleQ4143G(r) => new Dog4143G()[0], "
        "                  SquareQ4143G(s) => \"square\"); "
        "end ShapeQ4143G; "
        "class C "
        "  public static method Main(): int "
        "    var s := ShapeQ4143G.CircleQ4143G(1.0); "
        "    return (if s.Kind4143G = \"Woof\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runProg(program), 42);
}

// T8 — ANTI-REGRESSION: fix #4113's own recovery (a choice property's
// `case this of` with no nested class call at all) still resolves the
// correct arm. Self-contained here in addition to the unmodified
// choice_property_case_this_crash_4113_test.cpp.
BEZEL_TEST(codegen_choice_property_getter_window_leak_4143,
           choice_property_case_this_no_nested_leak_scenario_anti_regression_spec_35_2_35_5) {
    const std::string program =
        "module M4143H; choice ShapeQ4143H "
        "  CircleQ4143H(radius: double), SquareQ4143H(side: double); "
        "  public property IsCircle4143H: bool => "
        "    (case this of CircleQ4143H(_) => true, SquareQ4143H(_) => false); "
        "end ShapeQ4143H; "
        "class C "
        "  public static method Main(): int "
        "    var s := ShapeQ4143H.CircleQ4143H(2.0); "
        "    return (if s.IsCircle4143H then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runProg(program), 42);
}

// T9 — ACCEPT-SIDE CONTROL: an ENUM property's `case this of` (the #574/
// CG30 precedent) first-touched from inside a CHOICE property's window —
// proves the enum precedent is unaffected by this choice-focused fix.
BEZEL_TEST(codegen_choice_property_getter_window_leak_4143,
           enum_property_case_this_nested_in_choice_property_window_accept_control) {
    const std::string program =
        "module M4143I; "
        "enum Color4143I Red4143I, Green4143I, Blue4143I; "
        "  public property Code4143I: int => "
        "    (case this of Red4143I => 100, Green4143I => 200, _ => 300); "
        "end Color4143I; "
        "choice ShapeQ4143I "
        "  CircleQ4143I(radius: double), SquareQ4143I(side: double); "
        "  public property Kind4143I: int => "
        "    (case this of CircleQ4143I(r) => Color4143I.Red4143I.Code4143I, "
        "                  SquareQ4143I(s) => 999); "
        "end ShapeQ4143I; "
        "class C "
        "  public static method Main(): int "
        "    var s := ShapeQ4143I.CircleQ4143I(1.0); "
        "    return (if s.Kind4143I = 100 then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runProg(program), 42);
}

// T10 — ACCEPT-SIDE CONTROL: the reverse composition — a CLASS property's
// `case this of` first-touched from inside an ENUM property's window —
// proves declining `currentEnumMethod_` (see the file header) does not
// regress this ordinary, non-colliding-name composition.
BEZEL_TEST(codegen_choice_property_getter_window_leak_4143,
           class_property_case_this_nested_in_enum_property_window_accept_control) {
    const std::string program =
        "module M4143J; "
        "class Animal4143J "
        "  public property Sound4143J: string "
        "    get => (case this of Dog4143J => \"Woof\", Cat4143J => \"Meow\", _ => \"...\"); "
        "  end Sound4143J; "
        "end Animal4143J; "
        "class Dog4143J: Animal4143J end Dog4143J; "
        "class Cat4143J: Animal4143J end Cat4143J; "
        "enum Color4143J Red4143J, Green4143J; "
        "  public property Trigger4143J: string => "
        "    (case this of Red4143J => new Dog4143J().Sound4143J, Green4143J => \"n/a\"); "
        "end Color4143J; "
        "class C "
        "  public static method Main(): int "
        "    return (if Color4143J.Red4143J.Trigger4143J = \"Woof\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runProg(program), 42);
}
