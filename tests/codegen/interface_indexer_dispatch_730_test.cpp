// fix #730 / §13 / §23.4: an indexer accessed through an INTERFACE-typed
// reference dispatches dynamically through the itable to the implementing
// class's (or record's) concrete indexer accessor. Before the fix, interface
// indexers were never assigned itable slots (declareVtables skipped them), so
// `(arr: IIndexable)[i]` reached a null/absent slot and silently returned 0.
//
// These are codegen (dispatch) tests — no ownership involved — so runJit is the
// right harness.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
const char* iface =
    "module M; "
    "interface IIndexable property [i: int]: int; end IIndexable; ";
// fix #2417 (§11.2 / §23.4, v0.235): a SEPARATE, read-only interface for the
// two tests below that dispatch a getter-only indexer through the interface
// and never write through it. `iface` above stays read-write (no `readonly`)
// because `getter_and_setter_via_interface` genuinely needs a settable
// obligation to exercise the setter path; reusing it for a getter-only
// implementer is exactly the latent shape #2417's own accessor-set
// conformance check now rejects (a getter-only implementation of a
// read-write interface indexer installs a null function pointer in the
// itable setter slot — SIGSEGV on write, never exercised by these two
// read-only tests, but now a compile error regardless of use).
const char* ifaceReadOnly =
    "module M; "
    "interface IIndexable readonly property [i: int]: int; end IIndexable; ";
}  // namespace

// §23.4 / §13: the canonical #730 B2 repro — an expression-bodied class indexer
// dispatched through an interface-typed parameter. 40 + 2 = 42 (was 0).
BEZEL_TEST(interface_indexer_dispatch_730, expression_getter_via_interface) {
    BEZEL_EXPECT_EQ(run(
        std::string(ifaceReadOnly) +
        "class SingleInt: IIndexable "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public property [i: int]: int => _v + i; "
        "end SingleInt; "
        "class C "
        "  public static method GetAt(arr: IIndexable, i: int): int => arr[i]; "
        "  public static method Main(): int "
        "    using var s := new SingleInt(40); "
        "    return GetAt(s, 2); "          // 42
        "  end Main; "
        "end C;"), 42);
}

// §23.4 / §13: a get/set indexer dispatched through an interface — both
// accessors reach the implementing class. get(5)=105; set [3]:=50 stores
// value-i = 47 into _v; then get(0)=47.
BEZEL_TEST(interface_indexer_dispatch_730, getter_and_setter_via_interface) {
    BEZEL_EXPECT_EQ(run(
        std::string(iface) +
        "class NumberStore: IIndexable "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end NumberStore; "
        "class C "
        "  public static method Main(): int "
        "    using var ns := new NumberStore(100); "
        "    var iface: IIndexable := ns; "
        "    if iface[5] <> 105 then return 1; end; "
        "    iface[3] := 50; "             // _v := 50 - 3 = 47
        "    if iface[0] <> 47 then return 2; end; "
        "    return 42; "
        "  end Main; "
        "end C;"), 42);
}

// §23.4 / §13: an interface that interleaves a method, a named property, an
// indexer, and another named property. Pins that the indexer's itable slot does
// not perturb the method/named-property slot offsets. 7 + 10 + 15 + 20 = 52.
BEZEL_TEST(interface_indexer_dispatch_730, indexer_interleaved_with_members) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        // §11.2/§23.3 (#2413) / §11.2/§23.4 (#2417): Box's `Size`, `Name`,
        // and indexer are all getter-only and this test never writes through
        // the interface for any of them, so all three obligations are
        // declared `readonly` — a getter-only implementation of a
        // read-write interface property/indexer is a compile error (#2413
        // for the property case, #2417 for the indexer sibling here).
        "interface IBox "
        "  method Tag(): int; "
        "  readonly property Size: int; "
        "  readonly property [i: int]: int; "
        "  readonly property Name: int; "
        "end IBox; "
        "class Box: IBox "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Tag(): int => 7; "
        "  public property Size: int => _v; "
        "  public property [i: int]: int => _v + i; "
        "  public property Name: int => _v * 2; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(10); "
        "    var ib: IBox := b; "
        "    return ib.Tag() + ib.Size + ib[5] + ib.Name; "  // 7+10+15+20 = 52
        "  end Main; "
        "end C;"), 52);
}

// §23.4 / §9.6 / §13: a RECORD (value type) implementing an interface indexer
// dispatches through its boxed itable when accessed via the interface — the box
// thunk GEPs past the box header to the record bytes. 40 + 2 = 42.
BEZEL_TEST(interface_indexer_dispatch_730, record_indexer_via_interface) {
    BEZEL_EXPECT_EQ(run(
        std::string(ifaceReadOnly) +
        "record Pt(x: int): IIndexable "
        "  public property [i: int]: int => x + i; "
        "end Pt; "
        "class C "
        "  public static method Main(): int "
        "    var p := new Pt(40); "
        "    var ip: IIndexable := p; "
        "    return ip[2]; "               // 42
        "  end Main; "
        "end C;"), 42);
}

// §23.4 / §11.2 / §14.1: a generic interface indexer must preserve the
// concrete element type through itable dispatch. `IFlag<bool>[5]` returns a
// bool, so the discriminator branch below must receive i1 rather than the i32
// generic-erasure placeholder. Pre-fix this test fails during LLVM verification.
BEZEL_TEST(interface_indexer_dispatch_730, generic_interface_bool_indexer_2528) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IFlag<T> property [i: int]: T; end IFlag; "
        "class Flags: IFlag<bool> "
        "  private _b: bool := false; "
        "  public property [i: int]: bool "
        "    get => i > 2; "
        "    set => _b := value; "
        "  end; "
        "end Flags; "
        "class C "
        "  public static method Main(): int "
        "    using var obj := new Flags(); "
        "    var f: IFlag<bool> := obj; "
        "    const g: bool := f[5]; "
        "    if g then return 1; else return 0; end if; "
        "  end Main; "
        "end C;"), 1);
}

// §23.4 / §11.2 / §14.1: a generic interface indexer with `T = string` lowers
// to a bz_string* (i.e. ptr) — guards that the substitution path doesn't
// crash for a non-primitive element type, and that the call returns the
// expected bz_string* through the itable.
BEZEL_TEST(interface_indexer_dispatch_730,
           generic_interface_string_indexer_2528) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IRead<T> readonly property [i: int]: T; end IRead; "
        "class Holder: IRead<string> "
        "  private _v: string := \"ok\"; "
        "  public property [i: int]: string => _v; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    using var obj := new Holder(); "
        "    var f: IRead<string> := obj; "
        "    const g: string := f[0]; "
        "    if g = \"ok\" then return 1; else return 0; end if; "
        "  end Main; "
        "end C;"), 1);
}

// §23.4 / §11.2 / §14.1: a generic interface indexer with `T = int` keeps
// the i32 ABI (erasure coincidentally correct), but the get/set round-trip
// still has to survive the substitution — guard that the fix doesn't break
// the coincidentally-correct primitive case.
BEZEL_TEST(interface_indexer_dispatch_730,
           generic_interface_int_indexer_2528) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IFlag<T> property [i: int]: T; end IFlag; "
        "class Flags: IFlag<int> "
        "  private _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value; "
        "  end; "
        "end Flags; "
        "class C "
        "  public static method Main(): int "
        "    using var obj := new Flags(); "
        "    var f: IFlag<int> := obj; "
        "    f[5] := 9; "
        "    const g: int := f[2]; "
        "    if g = 11 then return 1; else return 0; end if; "
        "  end Main; "
        "end C;"), 1);
}

// §23.4 / §11.2 / §14.1: a multi-dimensional generic interface indexer
// preserves the element type. Mirrors T7 of xmod_indexer_import_2414's own
// int-indexer (the pre-fix #2422 fix) but for a non-trivial element type.
BEZEL_TEST(interface_indexer_dispatch_730,
           generic_interface_bool_grid_2528) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IGrid<T> readonly property [i: int, j: int]: T; "
        "end IGrid; "
        "class Grid: IGrid<bool> "
        "  public property [i: int, j: int]: bool => i > j; "
        "end Grid; "
        "class C "
        "  public static method Main(): int "
        "    using var obj := new Grid(); "
        "    var f: IGrid<bool> := obj; "
        "    const g: bool := f[2, 5]; "
        "    if g then return 1; else return 0; end if; "
        "  end Main; "
        "end C;"), 0);
}
