// fix #2174 / §28.10, §28.3, §28.6, #2137 (spec v0.220): the residual typed
// `using var` adoption shapes #2128 / #2137 left open — the interface- or
// virtual-DISPATCHED property read and the receiver-borrow call through an
// interface receiver. §28.6 guarantees a USER getter is a borrow-producing
// position signature-level: "a property or indexer getter may not return a
// freshly-produced owned value — a getter is a borrow-producing position".
// That guarantee is signature-level for ANY user-declared getter — including
// an interface-dispatched read (the PropertySymbol resolves to the
// interface's declaration, which has no body to body-scan) or an inherited
// virtual member on a base class.
//
// Pre-fix, `using var y: Widget := i.W;` over an interface-typed receiver
// kept adoption: the typed using-var reached codegen's classOfLocal dispose
// path and double-freed the borrow (heap-use-after-free under ASan, the
// #2137 family). The fix extends usingVarInitIsRecognizedBorrow's
// PropertySymbol branch to also recognize a non-builtin PropertySymbol
// whose `ps->decl` is null AND whose receiver's static type is an interface
// head (or whose parent is a class with a virtual/override modifier — the
// inherited-virtual case). The fix is narrow:
//   1. The read is a `MemberAccessExpr` (the dispatch form);
//   2. The receiver's static type is an interface head (declaration
//      kind == Interface), OR the parent is a class with a virtual/override
//      member (inherited-virtual);
//   3. Builtin properties (no PropertyDecl, no parent — registered
//      through a different pathway) keep adoption.
//
// Each pin test drives runAotChecked: without the fix the double-free makes
// r.ran false; with it the run is clean and the live-counter is 0 (disposed
// exactly once, by the underlying holder). Controls keep the unrelated
// shapes admitting. // per §28.10, §28.3, §28.6, #2137, #2174

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// ctor +1 / dtor -1 net-zero dispose-once oracle (double-free → ASan).
const char* kWidget =
    "module M; "
    "class Widget "
    "  public static live: int := 0; "
    "  public constructor() Widget.live := Widget.live + 1; end constructor; "
    "  public method Ping(): int return 1; end Ping; "
    "  destructor Widget.live := Widget.live - 1; end destructor; "
    "end Widget; ";

}  // namespace

// ===========================================================================
// (1) §28.10 / §28.3 / §28.6 — INTERFACE-DISPATCHED getter read on a typed
// `using var`. The PropertySymbol is the interface's `W` (the body is the
// interface's own auto-property declaration, no body to scan). Pre-fix the
// typed using var adopted and double-freed `o`'s widget. // per §28.10, #2174
// ===========================================================================
BEZEL_TEST(dispatched_getter_borrow_pin_2174,
           interface_dispatched_getter_read_pins_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "interface IHas "
        // `readonly` per §11.2 / §23.3 (#2413): `Owner` below satisfies this
        // obligation with a getter-only expression-bodied property
        // (`=> _w`), and a getter-only implementation of a READ-WRITE
        // interface property is a compile error as of #2413 (it left an
        // unfillable null itable setter slot — SIGSEGV on write). These
        // tests only ever READ `W`, so declaring the obligation `readonly`
        // preserves exactly what they pin (§28.10 borrow pinning through a
        // dispatched getter) while making the program well-formed.
        "  readonly property W: Widget; "
        "end IHas; "
        "class Owner: IHas "
        "  private owned _w: Widget; "
        "  public constructor() _w := new Widget(); end constructor; "
        "  public property W: Widget => _w; "
        "end Owner; "
        "class P "
        "  public static method Run(): int "
        "    using var o := new Owner(); "
        "    var i: IHas := o; "
        "    using var y: Widget := i.W; "    // interface-dispatched read
        "    return y.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return Widget.live; "           // 0 — disposed once, by `o`
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: ASan UAF on double-free
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (2) §28.10 / §28.3 / §28.6 — UNTYPED `using var` over the same interface-
// dispatched read: the untyped path was already recognized by the bare-id
// tracked-binding branch (#1285/#2071). The fix does NOT change that
// behavior. Control: confirms the untyped path was never broken. // per
// §28.10, #2128
// ===========================================================================
BEZEL_TEST(dispatched_getter_borrow_pin_2174,
           interface_dispatched_getter_read_untyped_pin_control_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "interface IHas "
        // `readonly` per §11.2 / §23.3 (#2413): `Owner` below satisfies this
        // obligation with a getter-only expression-bodied property
        // (`=> _w`), and a getter-only implementation of a READ-WRITE
        // interface property is a compile error as of #2413 (it left an
        // unfillable null itable setter slot — SIGSEGV on write). These
        // tests only ever READ `W`, so declaring the obligation `readonly`
        // preserves exactly what they pin (§28.10 borrow pinning through a
        // dispatched getter) while making the program well-formed.
        "  readonly property W: Widget; "
        "end IHas; "
        "class Owner: IHas "
        "  private owned _w: Widget; "
        "  public constructor() _w := new Widget(); end constructor; "
        "  public property W: Widget => _w; "
        "end Owner; "
        "class P "
        "  public static method Run(): int "
        "    using var o := new Owner(); "
        "    var i: IHas := o; "
        "    using var y := i.W; "            // untyped pin (always admitted)
        "    return y.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return Widget.live; "
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (3) §28.10 / §28.3 / §28.6 — DIRECT-DISPATCH getter read on a typed
// `using var` (the #2137 control). The PropertySymbol resolves to
// `Owner.W`'s concrete PropertyDecl (with body), so propertyGetterIsRecognized-
// Borrow's body scan applies. This was the #2137 shape, ALREADY admitted;
// the dispatch fix doesn't regress it. // per §28.10, #2137
// ===========================================================================
BEZEL_TEST(dispatched_getter_borrow_pin_2174,
           direct_dispatched_getter_read_still_pins_control_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Owner "
        "  private owned _w: Widget; "
        "  public constructor() _w := new Widget(); end constructor; "
        "  public property W: Widget => _w; "
        "end Owner; "
        "class P "
        "  public static method Run(): int "
        "    using var o := new Owner(); "
        "    using var y: Widget := o.W; "    // direct dispatch
        "    return y.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return Widget.live; "
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (4) §28.6 / §28.10 — CONTROL: a getter that returns a FRESH owned value
// (the #926 leak) is rejected even when read through an interface. The
// `=> new Cell()` body is a fresh producer; §28.6 forbids it at the
// definition site, and the dispatch fix doesn't reclassify it. // per
// §28.6, #926
// ===========================================================================
// Compile-time-only: the OWN-class getter is already rejected; the
// interface case is the same body, just read through the interface —
// rejection happens at the definition site, not the use site.

