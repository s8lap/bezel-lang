// #4952 (§4.6 "string" / §28.3 "Unbound owned temporaries" / §28.9
// "Temporaries", v0.307): reading a `string`-returning property through a
// DYNAMICALLY DISPATCHED receiver (an interface-typed variable, or a
// base-class-typed variable over a virtual property) leaked a fresh
// (+1-owned) result, while the SAME property read directly on the concrete
// class was clean.
//
// ROOT CAUSE. `isPlusOneStringProducer` (LlvmBackend_Setup.cpp) — the
// predicate every string-owning consumer (a `var` bind, a ctor arg, a
// return, a field store, ...) uses to decide whether it must release a
// property-read result — classified a MemberAccessExpr's producer-ness by
// inspecting `ps->decl`'s OWN getter body, where `ps` is the STATIC
// (declaring interface/base) PropertySymbol resolved at the call site. For
// a dynamically-dispatched read this is the wrong body to ask about: an
// interface property has no body at all (always classified `false` — a
// borrow, never released), and even a virtual CLASS property's introducing
// declaration can differ from the override that actually runs. Meanwhile
// the compiled getter FUNCTION installed into the vtable/itable slot
// (`ensurePropertyAccessorFn`) returns its body's raw value via a bare
// `CreateRet` with NO ownership normalization (by design, for the
// direct/inlined read — see #2708/#2703's "Axiom A4") — so a producer-bodied
// override (`property S: string => $"{_v}";`) handed the caller an actual
// +1 that the call site, believing it a borrow, never released. LEAK.
//
// THE FIX. Two parts, mirroring how a virtual/interface METHOD call is
// already sound (`isUserMethodStringReturnCall` trusts the compiled body's
// own mint unconditionally, per the METHOD ReturnStmt path):
//
//  1. `isPlusOneStringProducer`'s MemberAccessExpr arm now classifies a
//     non-nullable `string` property read as a +1 producer UNCONDITIONALLY
//     when `ps` has a vtable slot (`propGetSlot_`) or an itable slot
//     (`ifacePropertyGetIndex_`) — except `base.P` (§12.3, which bypasses
//     dispatch and inlines `ps`'s own body directly) and the §14.2
//     record-devirtualization shape (which also inlines a DIFFERENT body —
//     the record's own — bypassing the slot; see the generic-record note
//     below), both of which keep the existing body-inspection classification.
//  2. `ensureVirtualStringGetterOwnershipThunk` (LlvmBackend_Setup.cpp),
//     installed at every vtable/itable/generic-instantiation slot-fill site
//     that can hold a non-nullable `string` getter, wraps the compiled
//     accessor with a `bz_string_retain` before returning UNLESS the body is
//     already statically provable as a +1 producer (avoiding a double
//     mint). This guarantees every possible override reachable through the
//     slot hands the caller a normalized +1, so (1)'s unconditional
//     classification is sound.
//
// Sibling shapes checked here: a virtual (non-interface) class property
// (base-typed reference), an interface METHOD call (already sound —
// regression lock-in), and a GENERIC class implementing an interface
// (exercises the generic-instantiation itable/vtable re-specialization
// sites, LlvmBackend_Layout.cpp + LlvmBackend_Setup.cpp).
//
// Every loop below runs 220 iterations (>= the 200 CLAUDE.md floor for a
// loop-amplified leak repro) and accumulates an observable via `.Length`
// (a BORROW read — does not itself affect the string's refcount) into a
// numeric sink, printed once at the end so the expected stdout is a single
// deterministic line, independent of the leak.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string trimTrailingNewline(std::string s) {
    if (!s.empty() && s.back() == '\n') {
        s.pop_back();
        if (!s.empty() && s.back() == '\r') s.pop_back();
    }
    return s;
}

// §4.6 / §22 / §28.9 — the issue's cardinal repro: an interface `J` declares
// `readonly property S: string;`; class `C: J` implements it with a
// PRODUCER body (`$"{_v}"` — a fresh interpolation result each read). Read
// 220 times through a `J`-typed variable (dynamic itable dispatch).
// _v = 42 -> "42".Length == 2 every iteration -> sink == 440.
const char* kInterfacePropertyLoop =
    "module M; "
    "public interface J4952 "
    "  readonly property S: string; "
    "end J4952; "
    "public class C4952: J4952 "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public property S: string => $\"{_v}\"; "
    "end C4952; "
    "public class Program4952A "
    "  public static method Main(): int "
    "    using var c := new C4952(42); "
    "    var j: J4952 := c; "
    "    var i: int := 0; "
    "    var sink: int := 0; "
    "    while i < 220 do "
    "      var s: string := j.S; "
    "      sink := sink + s.Length; "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(sink); "
    "    return 0; "
    "  end Main; "
    "end Program4952A;";

// Control: the SAME property read DIRECTLY on the concrete class (no
// interface indirection, no dynamic dispatch) — already clean pre-fix; a
// regression guard that the fix does not disturb the static/inlined path.
const char* kDirectPropertyLoop =
    "module M; "
    "public interface J4952B "
    "  readonly property S: string; "
    "end J4952B; "
    "public class C4952B: J4952B "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public property S: string => $\"{_v}\"; "
    "end C4952B; "
    "public class Program4952B "
    "  public static method Main(): int "
    "    using var c := new C4952B(42); "
    "    var i: int := 0; "
    "    var sink: int := 0; "
    "    while i < 220 do "
    "      var s: string := c.S; "
    "      sink := sink + s.Length; "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(sink); "
    "    return 0; "
    "  end Main; "
    "end Program4952B;";

// §12.1 / §12.5 sibling — a VIRTUAL (non-interface) class property. The
// abstract base declares the property with no body; the leaf's override is
// a producer. Read through a BASE-typed reference (vtable dispatch, not an
// itable). _v = 7 -> "7".Length == 1 -> sink == 220.
const char* kVirtualClassPropertyLoop =
    "module M; "
    "public abstract class Base4952 "
    "  public abstract property Tag: string; "
    "end Base4952; "
    "public class Leaf4952: Base4952 "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public override property Tag: string => $\"{_v}\"; "
    "end Leaf4952; "
    "public class Program4952C "
    "  public static method Main(): int "
    "    using var leaf := new Leaf4952(7); "
    "    var b: Base4952 := leaf; "
    "    var i: int := 0; "
    "    var sink: int := 0; "
    "    while i < 220 do "
    "      var s: string := b.Tag; "
    "      sink := sink + s.Length; "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(sink); "
    "    return 0; "
    "  end Main; "
    "end Program4952C;";

// Sibling shape (method vs property): an interface METHOD call returning a
// fresh string was ALREADY sound (isUserMethodStringReturnCall trusts the
// compiled body unconditionally). Regression lock-in — must stay clean.
const char* kInterfaceMethodLoop =
    "module M; "
    "public interface K4952 "
    "  method Show(): string; "
    "end K4952; "
    "public class D4952: K4952 "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public method Show(): string => $\"{_v}\"; "
    "end D4952; "
    "public class Program4952D "
    "  public static method Main(): int "
    "    using var d := new D4952(99); "
    "    var k: K4952 := d; "
    "    var i: int := 0; "
    "    var sink: int := 0; "
    "    while i < 220 do "
    "      var s: string := k.Show(); "
    "      sink := sink + s.Length; "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(sink); "
    "    return 0; "
    "  end Main; "
    "end Program4952D;";

// §14.1 sibling (generic-constrained receiver): a GENERIC class `Box<T>`
// implementing `IBox<T>`, instantiated at `T = Color4952` (an enum — its
// erased-template vs. concrete layout differ, forcing the itable
// re-specialization path in LlvmBackend_Layout.cpp / LlvmBackend_Setup.cpp
// to re-lower `S`'s accessor for THIS instantiation). Read through an
// `IBox<Color4952>`-typed variable. _v = 5 -> "5".Length == 1 -> sink == 220.
const char* kGenericInterfacePropertyLoop =
    "module M; "
    "public enum Color4952 Red, Green, Blue; end Color4952; "
    "public interface IBox4952<T> "
    "  method Get(): T; "
    "  readonly property S: string; "
    "end IBox4952; "
    "public class Box4952<T>: IBox4952<T> "
    "  private _t: T; "
    "  private _v: int; "
    "  public constructor(t: T, v: int) _t := t; _v := v; end constructor; "
    "  public method Get(): T => _t; "
    "  public property S: string => $\"{_v}\"; "
    "end Box4952; "
    "public class Program4952E "
    "  public static method Main(): int "
    "    using var box := new Box4952<Color4952>(Color4952.Blue, 5); "
    "    var ib: IBox4952<Color4952> := box; "
    "    var i: int := 0; "
    "    var sink: int := 0; "
    "    while i < 220 do "
    "      var s: string := ib.S; "
    "      sink := sink + s.Length; "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(sink); "
    "    return 0; "
    "  end Main; "
    "end Program4952E;";

// §14.2 sibling (record-conforms-to-interface devirtualization / §9.4): a
// generic method `GetS<T>(x: T) where T: J4952F` BINDS `x.S` to a local
// `var` inside its body (deliberately NOT a bare `return x.S;` — a
// method's own return normalizes any returned string uniformly
// (emitFunctionBody/emitReturnStmt), which is exactly why the interface-
// METHOD-call control above was already sound and would mask this
// specific seam; a local `var` binding is what actually consults
// isPlusOneStringProducer for this MemberAccessExpr). Sema resolves `x.S`
// to the INTERFACE property, but `x`'s static type is the bare type
// parameter `T`, and the active substitution (T = RecF4952, a RECORD)
// makes lowerVirtualPropertyGet's record-devirtualization branch
// (LlvmBackend_Exprs.cpp, §14.2/§9.4) redirect to the record's OWN
// accessor and fully INLINE its body — bypassing the itable slot (and
// this fix's ownership thunk) entirely, the same way `base.P` bypasses
// it. This is the exact shape isPlusOneStringProducer's record-
// devirtualization redirect (LlvmBackend_Setup.cpp) exists for: asking
// about the INTERFACE property's (bodyless) declaration would wrongly
// classify this as a borrow (pre-#4952-fix conservative default, and the
// LOCAL `s` would then never be released — a leak) while the
// unconditional-dynamic-slot fast path would wrongly classify it as an
// always-+1 producer (neither is what actually runs) — the redirect
// instead asks about the RECORD's own body, agreeing with the inlined
// codegen. V = 8 -> "8".Length == 1 -> sink == 220.
const char* kRecordDevirtualizedGenericPropertyLoop =
    "module M; "
    "public interface J4952F "
    "  readonly property S: string; "
    "end J4952F; "
    "public record RecF4952(V: int) : J4952F "
    "  public property S: string => $\"{V}\"; "
    "end RecF4952; "
    "static class Util4952F "
    "  public static method GetLen<T>(x: T): int where T: J4952F "
    "    var s: string := x.S; "
    "    return s.Length; "
    "  end GetLen; "
    "end Util4952F; "
    "public class Program4952F "
    "  public static method Main(): int "
    "    var r := new RecF4952(8); "
    "    var i: int := 0; "
    "    var sink: int := 0; "
    "    while i < 220 do "
    "      sink := sink + Util4952F.GetLen(r); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(sink); "
    "    return 0; "
    "  end Main; "
    "end Program4952F;";

}  // namespace

// =====================================================================
// #4952 — the issue's cardinal repro: an interface property returning a
// fresh string, read 220x through an interface-typed variable. WITHOUT the
// fix, every read leaks the interpolation result (LSan flips r.ran false).
// WITH the fix, the run is clean AND prints the correct accumulated sink.
// =====================================================================

BEZEL_TEST(codegen_interface_property_string_getter_ownership_4952,
           interface_dispatched_producer_property_does_not_leak_spec_4_6_28_3_28_9) {
    JitResult r = runAotChecked(kInterfacePropertyLoop);

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(trimTrailingNewline(r.stdoutText), std::string("440"));
}

// =====================================================================
// Control: the same property read directly on the concrete class (no
// interface indirection) is already clean pre-fix — must remain clean.
// =====================================================================

BEZEL_TEST(codegen_interface_property_string_getter_ownership_4952,
           direct_class_read_control_still_clean_spec_4_6_28_9) {
    JitResult r = runAotChecked(kDirectPropertyLoop);

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(trimTrailingNewline(r.stdoutText), std::string("440"));
}

// =====================================================================
// Sibling: a VIRTUAL (non-interface) class property read through a
// base-typed reference (vtable, not itable, dispatch) — the same root
// cause, a different slot kind.
// =====================================================================

BEZEL_TEST(codegen_interface_property_string_getter_ownership_4952,
           virtual_class_property_does_not_leak_spec_12_1_12_5_4_6_28_9) {
    JitResult r = runAotChecked(kVirtualClassPropertyLoop);

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(trimTrailingNewline(r.stdoutText), std::string("220"));
}

// =====================================================================
// Sibling (method vs property): an interface METHOD call was already sound
// — regression lock-in, must stay clean.
// =====================================================================

BEZEL_TEST(codegen_interface_property_string_getter_ownership_4952,
           interface_method_call_control_already_sound_spec_4_6_28_9) {
    JitResult r = runAotChecked(kInterfaceMethodLoop);

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(trimTrailingNewline(r.stdoutText), std::string("440"));
}

// =====================================================================
// Sibling (generic-constrained receiver): a generic class implementing an
// interface, instantiated with an enum type argument that forces the
// itable/vtable generic-instantiation re-specialization path to re-lower
// the (unrelated-to-T) string property's accessor for this instantiation.
// =====================================================================

BEZEL_TEST(codegen_interface_property_string_getter_ownership_4952,
           generic_class_interface_property_does_not_leak_spec_14_1_4_6_28_9) {
    JitResult r = runAotChecked(kGenericInterfacePropertyLoop);

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(trimTrailingNewline(r.stdoutText), std::string("220"));
}

// =====================================================================
// Sibling (record-conforms-to-interface devirtualization): a generic
// method with a `where T: J` constraint, instantiated with a RECORD,
// reads the interface property through the bare type-parameter receiver.
// Codegen fully inlines the record's own accessor body, bypassing the
// itable slot (and this fix's thunk) entirely — must be classified
// correctly by isPlusOneStringProducer's redirect, not the dynamic-slot
// fast path.
// =====================================================================

BEZEL_TEST(codegen_interface_property_string_getter_ownership_4952,
           record_devirtualized_generic_property_does_not_leak_spec_14_2_9_4_4_6_28_9) {
    JitResult r = runAotChecked(kRecordDevirtualizedGenericPropertyLoop);

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(trimTrailingNewline(r.stdoutText), std::string("220"));
}
