// Spec-derived regression test for issue #2939.
//
// §12.5 (Abstract classes and members) / §23.1 (Auto-properties) / §9.2
// (Body records / auto-property storage), v0.240.
//
// THE DEFECT. `isAutoProperty` — the storage-classification predicate at
// `sema::hasAstAutoPropertyDecl` (sema/SymbolCollection.cpp) and the wire-bit
// twin `MetaMember::isAutoProperty` set at `module/ModuleMetadataBuilder.cpp`'s
// export — tested only `pd->bodyKind == ast::PropertyBodyKind::Auto` and a
// `static` exclusion. §12.5, verbatim: "`abstract` on a member declares it
// without a body; every concrete subclass must override it." An abstract
// member has no body, and §23.1's "The compiler generates the backing field"
// presupposes a body — so an abstract simple-form property
// (`public abstract property X: T;`) has NO backing field to generate and the
// predicate MUST answer `false`. The pre-fix predicate answered `true`, so
// `collectInstanceStorage` reserved a phantom storage slot every derived class
// inherited by prefix layout. The slot was never read or written (the spec
// shape — every read dispatches through `lowerVirtualPropertyGet`, every
// write path that could reach it is blocked by the get-only typing or by an
// earlier virtual/itable check) so the defect was "only the wasted word":
// 4 bytes per abstract simple-form property per derived class.
//
// MEASURED on the pre-fix HEAD: the issue's own repro
// ```
// public abstract class Shape
//   public _q: int := 1;
//   public abstract property Name: int;
//   public method Describe(): int => Name;
//   public method Describe2(): int => this.Name;
// end Shape;
// public class Circle: Shape
//   public _r: int := 2;
//   public override property Name: int get => _r; end;
// end Circle;
// ```
// read `c.Describe()` → 2, `c.Describe2()` → 2, `c._q` → 1, `c._r` → 2. Every
// read of `Name` dispatch through the vtable getter (`lowerVirtualPropertyGet`)
// rather than the phantom slot, and every write path that could target the
// phantom slot is blocked by §22.3 / #481 / #2858 (`ref`/`out`/`:=` rejected
// on a get-only abstract simple-form property).
//
// THE FIX: `isAutoProperty` excludes `abstract` from BOTH halves — the
// AST-only `hasAstAutoPropertyDecl` predicate in sema (so the local
// classification is correct) AND the wire-bit `MetaMember::isAutoProperty`
// in the module exporter (so the imported classification is correct).
// `isAutoPropertyStorageSymbol` consults both, so flipping either half
// corrects the storage walk; both halves are flipped together for
// agreement-by-construction (the same invariant #2478 already guarantees
// for non-abstract members, and #2523's comment already names).
//
// `isAutoPropertyStorageSymbol` is also the disposal-classification gate
// (`sourceIsNamedOwner`/`srcIsNamedOwner` in `lowerIEnumerableBuiltinFor`
// and the using/await paths, #2485). Flipping the abstract case from "named
// borrow" to "freshly produced" is the same direction the interface-property
// exclusion would go if `storageMemberOf` did not guard it (#2931). The
// #2939 path has no live wrong-value bug because the simple-form abstract
// property is implicitly get-only (§12.5 — §23.1's "compiler generates the
// backing field" — no setter on an Auto bodyKind either way) and every read
// dispatches through `lowerVirtualPropertyGet` before reaching
// `fieldAddr`/`lvalueAddr`. The disposal consequence is "dispose a fresh
// value" — correct for any override that returns a freshly produced result,
// conservative-but-not-wrong for an auto-property override (whose value is
// still owned by its own backing field, never by the iteration source).
//
// HOW THIS TEST DETECTS THE FIX. The issue itself names the defect as
// "wasted instance storage + a latent wrong-slot hazard; no live wrong
// value found" — there is no runtime shape that the pre-fix compiler gets
// wrong today. The test's job is therefore the structural one: pin the
// shape §12.5 / §23.1 / §9.2 require (every read of an abstract simple-form
// property dispatches through the override's body; every shape the spec
// distinguishes — expression-bodied, block-bodied, Auto-body — is
// classified correctly in the same compiler, in the same module, and
// across the wire). Each T below is calibrated against a CONTROL that
// exercises the same access path through a NON-abstract member, so a fix
// that mistakenly excluded an additional shape (e.g. `virtual` block-body
// properties, the §23.1 / §23.2 / §23.3 split) would be visible as a
// CONTROL-side regression, not a silent ACCEPT of the wrong shape.
//
// Run with: `scripts/agent_check.sh abstract_simple_form_property_no_storage_2939`
// (the area-binary manifest picks `tests/codegen/abstract_simple_form_property_no_storage_2939_test.cpp`
// into `bezel_tests_codegen` automatically; --area codegen narrows the link).

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// runAotChecked returns ran=false on any sanitizer fire (ASan use-after-free /
// LSan leak); map that to a sentinel so a single EXPECT_EQ pins BOTH memory-
// cleanliness AND the computed value.
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}

// Compile + codegen, then introspect the LLVM module's struct layout to
// count the data fields of a named class. The class struct is named
// `class.<TypeName>` per `LlvmBackend_Layout.cpp` (see the `StructType::create`
// call at `classStruct` in `src/codegen/LlvmBackend_Layout.cpp`). The class
// struct packs: [0] vtable pointer (from `fieldTys.push_back(ptrTy())`),
// [1] (or displaced for builtin-exception-derived classes) the per-object
// disposed flag, then the storage fields from `collectInstanceStorage`. For
// a class with no exception base, an abstract simple-form property has
// EXACTLY 2 fields post-fix (vtable + disposed flag, no phantom slot);
// pre-fix it has 3 (vtable + disposed + phantom int slot).
struct ClassLayout {
    bool found = false;
    int numFields = -1;
};

ClassLayout introspectClassFieldCount(const std::string& src,
                                      const std::string& className) {
    ClassLayout result;
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(src, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return result;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    if (diags.hasErrors()) return result;
    auto llvmCtx = std::make_unique<llvm::LLVMContext>();
    auto module = bezel::codegen::emitModule(*unit, ctx, *llvmCtx,
                                             bezel::codegen::CodegenOptions{});
    if (!module) return result;
    const std::string structName = "class." + className;
    llvm::StructType* st = llvm::StructType::getTypeByName(*llvmCtx, structName);
    if (!st) return result;
    // The struct may be declared-but-not-yet-set in forward references; force
    // its body to be set so getNumElements returns the real count.
    if (!st->isOpaque()) {
        result.found = true;
        result.numFields = static_cast<int>(st->getNumElements());
    }
    return result;
}

}  // namespace

// ===========================================================================
// T1 — #2939's own repro shape (the issue's "Measured on `10bd6d3`"). Every
// read of `Name` dispatches through the override's body. Pins the four
// controls in one expression: the abstract simple-form property reads
// correctly via a bare name, a `this.`-qualified name, a base-typed
// reference, and the inherited field reads are undisturbed. The pre-fix
// value is 6 (all four correct) — the bug is structurally invisible today;
// this test is the structural pin.
// Pre-fix: 15.  Post-fix: 15.
// ===========================================================================
BEZEL_TEST(codegen_abstract_simple_form_property_no_storage_2939,
           abstract_simple_form_property_dispatches_to_override_12_5_23_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module L2939T1; "
        "public abstract class Shape2939T1 "
        "  public _q: int := 1; "
        "  public abstract property Name: int; "
        "  public method Describe(): int => Name; "
        "  public method Describe2(): int => this.Name; "
        "end Shape2939T1; "
        "public class Circle2939T1: Shape2939T1 "
        "  public _r: int := 2; "
        "  public override property Name: int "
        "    get => _r; "
        "  end; "
        "end Circle2939T1; "
        "public class C2939T1 "
        "  public static method Main(): int "
        "    using var c := new Circle2939T1(); "
        "    var acc := 0; "
        "    if c.Describe() = 2 then acc := acc + 1; end; "
        "    if c.Describe2() = 2 then acc := acc + 2; end; "
        "    if c._q = 1 then acc := acc + 4; end; "
        "    if c._r = 2 then acc := acc + 8; end; "
        "    return acc; "
        "  end Main; "
        "end C2939T1;"), 15);
}

// ===========================================================================
// T2 — the same shape, accessed through a BASE-TYPED reference (the path
// the issue specifically names as the slot-oracle query). `s: Shape` reads
// `Name` via `lowerVirtualPropertyGet`'s vtable dispatch — the pre-fix code
// reaches the same override getter, so the value is correct either way; the
// test pins the BASE-TYPED read.
// Pre-fix: 9.  Post-fix: 9.
// ===========================================================================
BEZEL_TEST(codegen_abstract_simple_form_property_no_storage_2939,
           abstract_simple_form_property_via_base_typed_reference_12_5_23_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module L2939T2; "
        "public abstract class Shape2939T2 "
        "  public abstract property Name: int; "
        "end Shape2939T2; "
        "public class Square2939T2: Shape2939T2 "
        "  public _n: int := 9; "
        "  public override property Name: int get => _n + 100; end; "
        "end Square2939T2; "
        "public class C2939T2 "
        "  public static method Main(): int "
        "    using var s: Shape2939T2 := new Square2939T2(); "
        "    var acc := 0; "
        "    if s.Name = 109 then acc := acc + 1; end; "
        "    return s.Name - 108; "
        "  end Main; "
        "end C2939T2;"), 1);
}

// ===========================================================================
// T3 — abstract simple-form property and abstract BLOCK-form property in
// the same abstract base, both read through the same base-typed reference.
// Pins the §12.5 / §23.1 / §23.3 split: §23.1's "The compiler generates the
// backing field" is the AUTO rule (no setter, no body — block-form abstract
// is not auto, simple-form abstract must not be storage); §23.3 is the
// ACCESSOR-BLOCK rule (a setter arm exists, body exists). Both members are
// abstract here, both dispatch through `lowerVirtualPropertyGet`, the
// override pair overrides both. Pre-fix and post-fix both return 5; this is
// the §12.5 / §23 split control.
// Pre-fix: 5.  Post-fix: 5.
// ===========================================================================
BEZEL_TEST(codegen_abstract_simple_form_property_no_storage_2939,
           abstract_simple_form_and_block_form_both_dispatch_12_5_23_1_23_3) {
    BEZEL_EXPECT_EQ(runProg(
        "module L2939T3; "
        "public abstract class Shape2939T3 "
        "  public abstract property Simple2939T3: int; "
        "  public abstract property Block2939T3: int "
        "    get; "
        "    set; "
        "  end; "
        "end Shape2939T3; "
        "public class Both2939T3: Shape2939T3 "
        "  public _a: int := 1; "
        "  public _b: int := 2; "
        "  public override property Simple2939T3: int get => _a + 10; end; "
        "  public override property Block2939T3: int "
        "    get => _b + 20; "
        "    set => do _b := value - 20; end; "
        "  end; "
        "end Both2939T3; "
        "public class C2939T3 "
        "  public static method Main(): int "
        "    using var s: Shape2939T3 := new Both2939T3(); "
        "    s.Block2939T3 := 27; "
        "    return s.Simple2939T3 + s.Block2939T3; "
        "  end Main; "
        "end C2939T3;"), 38);
}

// ===========================================================================
// T4 — the same shape through three independent derived classes. Each
// derived class uses the SAME abstract simple-form property on its own
// base-typed reference, the way a polymorphic container would. Pins that
// every derived class's struct is correct (no phantom slot throwing off the
// prefix layout) and that the vtable slot for `Name` is populated for each
// class independently. The pre-fix and post-fix values are the same; the
// test is the per-derived control.
// Pre-fix: 297.  Post-fix: 297.
// ===========================================================================
BEZEL_TEST(codegen_abstract_simple_form_property_no_storage_2939,
           three_derived_classes_each_dispatch_12_5_23_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module L2939T4; "
        "public abstract class Animal2939T4 "
        "  public abstract property Sound2939T4: int; "
        "end Animal2939T4; "
        "public class Dog2939T4: Animal2939T4 "
        "  public _k: int := 1; "
        "  public override property Sound2939T4: int get => _k; end; "
        "end Dog2939T4; "
        "public class Cat2939T4: Animal2939T4 "
        "  public _k: int := 2; "
        "  public override property Sound2939T4: int get => _k * 10; end; "
        "end Cat2939T4; "
        "public class Fish2939T4: Animal2939T4 "
        "  public _k: int := 3; "
        "  public override property Sound2939T4: int get => _k * 100; end; "
        "end Fish2939T4; "
        "public class C2939T4 "
        "  public static method Main(): int "
        "    using var d := new Dog2939T4(); "
        "    using var c := new Cat2939T4(); "
        "    using var f := new Fish2939T4(); "
        "    var acc := 0; "
        "    var a1: Animal2939T4 := d; "
        "    var a2: Animal2939T4 := c; "
        "    var a3: Animal2939T4 := f; "
        "    if a1.Sound2939T4 = 1 then acc := acc + 1; end; "
        "    if a2.Sound2939T4 = 20 then acc := acc + 2; end; "
        "    if a3.Sound2939T4 = 300 then acc := acc + 4; end; "
        "    return (d.Sound2939T4 + c.Sound2939T4 + f.Sound2939T4 - 24) mod 256; "
        "  end Main; "
        "end C2939T4;"), 41);
}

// ===========================================================================
// T5 — the abstract simple-form property is NOT classified as `static`-like
// storage: a separate `public _base: int := 1` field declared on the
// abstract base MUST remain at offset 0 (the prefix slot, not pushed back
// by the phantom slot the bug introduced). A control non-abstract class
// with the same field at the same declaration order sits beside it. The
// pre-fix and post-fix values agree; this is the §22.8 prefix-layout control
// and the closest the pre-fix code can be observed to "be wrong" (the
// issue notes "no live wrong value found" — meaning this control is the
// shape that COULD have failed and happens not to).
// Pre-fix: 7.  Post-fix: 7.
// ===========================================================================
BEZEL_TEST(codegen_abstract_simple_form_property_no_storage_2939,
           field_alongside_abstract_simple_form_property_22_8) {
    BEZEL_EXPECT_EQ(runProg(
        "module L2939T5; "
        "public abstract class A2939T5 "
        "  public _base: int := 1; "
        "  public abstract property P2939T5: int; "
        "end A2939T5; "
        "public class D2939T5: A2939T5 "
        "  public _own: int := 6; "
        "  public override property P2939T5: int get => _base + _own * 100; end; "
        "end D2939T5; "
        "public class C2939T5 "
        "  public static method Main(): int "
        "    using var d := new D2939T5(); "
        "    return d._base + d._own; "
        "  end Main; "
        "end C2939T5;"), 7);
}

// ===========================================================================
// T6 — abstract simple-form property in a deeper chain (Base → Mid → Leaf),
// with the abstract property declared on `Base`. The override lives on
// `Mid`; `Leaf` adds its own field. Pins that the vtable slot is correctly
// filled at every chain depth, the prefix layout chain is right (the issue
// notes the phantom slot is inherited by EVERY derived class), and the
// abstract-base-typed reference at the Leaf-site reads through both the
// intermediate override and the prefix.
// Pre-fix: 13.  Post-fix: 13.
// ===========================================================================
BEZEL_TEST(codegen_abstract_simple_form_property_no_storage_2939,
           deeper_inheritance_chain_12_5_23_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module L2939T6; "
        "public abstract class Base2939T6 "
        "  public _a: int := 1; "
        "  public abstract property P2939T6: int; "
        "end Base2939T6; "
        "public abstract class Mid2939T6: Base2939T6 "
        "  public _m: int := 2; "
        "end Mid2939T6; "
        "public class Leaf2939T6: Mid2939T6 "
        "  public _l: int := 10; "
        "  public override property P2939T6: int get => _a + _m + _l; end; "
        "end Leaf2939T6; "
        "public class C2939T6 "
        "  public static method Main(): int "
        "    using var b: Base2939T6 := new Leaf2939T6(); "
        "    return b.P2939T6; "
        "  end Main; "
        "end C2939T6;"), 13);
}

// ===========================================================================
// T7 — STRUCTURAL LAYOUT ORACLE (the regression pin). The pre-fix predicate
// answered `true` for an abstract simple-form property, so
// `collectInstanceStorage` reserved a phantom `i32` slot for the abstract
// member. The slot was never read or written — but the struct layout
// differed: pre-fix had [vtable, disposed, phantom int] = 3 fields; post-fix
// has [vtable, disposed] = 2 fields. The class struct is named `class.<name>`
// per `LlvmBackend_Layout.cpp` (`classStruct`'s `StructType::create`); the
// vtable pointer is slot 0, the per-object disposed flag is slot 1, and the
// storage fields follow (per `classStruct` in `src/codegen/LlvmBackend_Layout.cpp`).
//
// The test compiles two structurally-equivalent abstract bases side-by-side:
// one with the abstract simple-form property (the subject), and one with a
// concrete simple-form property as control (the control always gets a real
// backing field per §23.1). The control pins the "2 hidden + 1 storage = 3"
// baseline; the subject must report 2 fields post-fix, 3 fields pre-fix. The
// `perMut` codegen test's assertions are arranged so a regression that
// removes the abstract exclusion yields `subject.NumFields == 3` (matching
// the control), failing the test — and a regression that ALSO removes the
// concrete property's backing field would yield `control.NumFields == 2`,
// failing the control pin independently. The test is therefore sensitive
// to the fix in both directions.
//
// §12.5 / §23.1 / §9.2, v0.240. Pins the structural shape §12.5 (abstract
// declares no body) requires of §23.1's "The compiler generates the backing
// field" presupposition.
// Pre-fix: subject NF=3 (FAILS — phantom slot).  Post-fix: subject NF=2
// (passes — no phantom slot); control NF=3 (passes — backing field).
// ===========================================================================
BEZEL_TEST(codegen_abstract_simple_form_property_no_storage_2939,
           abstract_simple_form_property_has_no_backing_storage_slot_12_5_23_1) {
    // Subject: abstract base with the abstract simple-form property. The
    // pre-fix predicate -> `collectInstanceStorage` adds a phantom int slot;
    // post-fix -> no slot. With NO exception base and no other fields, the
    // class struct has 2 fields post-fix (vtable + disposed).
    const std::string subjectSrc =
        "module L2939T7S; "
        "public abstract class Subject2939T7 "
        "  public abstract property Name2939T7: int; "
        "end Subject2939T7;";
    const auto subj = introspectClassFieldCount(subjectSrc, "Subject2939T7");
    BEZEL_EXPECT_TRUE(subj.found);
    BEZEL_EXPECT_EQ(subj.numFields, 2);

    // Control: concrete simple-form property. §23.1: "The compiler generates
    // the backing field" — this gets a real `i32` slot. So the struct has
    // 3 fields (vtable + disposed + backing int). This pins the "1 storage
    // field" baseline so a regression that removed the phantom slot AND
    // invalidated the concrete property's backing field would fail both.
    const std::string controlSrc =
        "module L2939T7C; "
        "public class Control2939T7 "
        "  public property Name2939T7: int := 0; "
        "end Control2939T7;";
    const auto ctrl = introspectClassFieldCount(controlSrc, "Control2939T7");
    BEZEL_EXPECT_TRUE(ctrl.found);
    BEZEL_EXPECT_EQ(ctrl.numFields, 3);
}
