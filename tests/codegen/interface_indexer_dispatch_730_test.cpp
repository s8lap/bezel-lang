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
}  // namespace

// §23.4 / §13: the canonical #730 B2 repro — an expression-bodied class indexer
// dispatched through an interface-typed parameter. 40 + 2 = 42 (was 0).
BEZEL_TEST(interface_indexer_dispatch_730, expression_getter_via_interface) {
    BEZEL_EXPECT_EQ(run(
        std::string(iface) +
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
        "interface IBox "
        "  method Tag(): int; "
        "  property Size: int; "
        "  property [i: int]: int; "
        "  property Name: int; "
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
        std::string(iface) +
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
