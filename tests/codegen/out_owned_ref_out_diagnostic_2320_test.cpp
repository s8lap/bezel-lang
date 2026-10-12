// Spec-derived regression tests for issue #2320.
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool messageContains(const bezel::test::JitResult& r,
                     const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

BEZEL_TEST(out_owned_ref_out_diagnostic_2320,
           out_owned_indexer_slot_cites_22_3) {
    auto r = runAot(std::string(
        "module M; class Cell public N: int; "
        "    public constructor(n: int) this.N := n; end constructor; "
        "  end Cell; "
        "class C "
        "  public static method Make(out owned c: Cell) "
        "    c := new Cell(5); "
        "  end Make; "
        "  public static method Main(): int "
        "    var arr := new Array<Cell>(); "
        "    using var seed := new Cell(1); arr.Add(seed); "
        "    C.Make(out arr[0]); "
        "    return arr[0].N; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(messageContains(r, "§22.3"));
    BEZEL_EXPECT_TRUE(messageContains(r, "§22.4"));
    BEZEL_EXPECT_TRUE(messageContains(r, "out using var"));
    // fix #3842 (§A.4 / §22.4, v0.289 L2): the diagnostic now names the two
    // ADMITTED inline forms. `out owned var x` was the third name it used to
    // advertise and is retired — the parser rejects it now — so asserting it
    // here would pin text the user can no longer act on. The §22.3 / §22.4
    // citations and the "out using var" remedy above are unchanged.
    BEZEL_EXPECT_TRUE(messageContains(r, "out var x"));
    BEZEL_EXPECT_FALSE(messageContains(r, "#1386"));
}

BEZEL_TEST(out_owned_ref_out_diagnostic_2320,
           out_owned_property_cites_22_3) {
    auto r = runAot(std::string(
        "module M; class Box "
        "  public _v: int; "
        "  public constructor(v: int) this._v := v; end constructor; "
        "  public property V: int get => this._v; set => this._v := value; "
        "  end V; "
        "end Box; "
        "class C "
        "  public static method Make(out owned b: Box) "
        "    b := new Box(99); "
        "  end Make; "
        "  public static method Main(): int "
        "    var box := new Box(5); "
        "    C.Make(out box.V); "
        "    return box.V; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(messageContains(r, "§22.3"));
    BEZEL_EXPECT_TRUE(messageContains(r, "§22.4"));
    BEZEL_EXPECT_FALSE(messageContains(r, "#1386"));
}

BEZEL_TEST(out_owned_ref_out_diagnostic_2320,
           out_owned_pre_existing_local_cites_22_3) {
    auto r = runAot(std::string(
        "module M; class Cell public N: int; "
        "    public constructor(n: int) this.N := n; end constructor; "
        "  end Cell; "
        "class C "
        "  public static method Make(out owned c: Cell) "
        "    c := new Cell(5); "
        "  end Make; "
        "  public static method Main(): int "
        "    var existing := new Cell(10); "
        "    C.Make(out existing); "
        "    return existing.N; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(messageContains(r, "§22.3"));
    BEZEL_EXPECT_TRUE(messageContains(r, "§22.4"));
    BEZEL_EXPECT_FALSE(messageContains(r, "#1386"));
}

BEZEL_TEST(out_owned_ref_out_diagnostic_2320,
           out_using_var_for_out_owned_accepted) {
    auto r = runAot(std::string(
        "module M; class Cell public N: int; "
        "    public constructor(n: int) this.N := n; end constructor; "
        "  end Cell; "
        "class C "
        "  public static method Make(out owned c: Cell) "
        "    c := new Cell(5); "
        "  end Make; "
        "  public static method Main(): int "
        "    C.Make(out using var cell); "
        "    return cell.N; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

BEZEL_TEST(out_owned_ref_out_diagnostic_2320,
           out_owned_var_for_out_owned_accepted) {
    auto r = runAot(std::string(
        "module M; class Cell public N: int; "
        "    public constructor(n: int) this.N := n; end constructor; "
        "  end Cell; "
        "class C "
        "  public static method Make(out owned c: Cell) "
        "    c := new Cell(5); "
        "  end Make; "
        "  public static method Main(): int "
        "    C.Make(out var cell); "
        "    return cell.N; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// fix #2320 (post-review): the constructor call path's initial fix picked
// the SOLE or FIRST-arity-matching ConstructorDecl to thread its modifier
// into checkRefOutOperandCategory. Same-arity constructor overloads are
// supported by §22.6/§22.7, so that guess is wrong: an `out owned Cell`
// ctor declared before an `out int` ctor would otherwise tag an `out n`
// argument against the latter as §22.3-out-owned and false-reject. This
// pins the corrected behavior — an unambiguous single-binding overload
// match threads the resolved ctor's modifier; a `new C(out n)` intended
// for `C(out x: int)` compiles and returns 99.
BEZEL_TEST(out_owned_ref_out_diagnostic_2320,
           same_arity_ctor_overload_out_int_accepted) {
    auto r = runAot(std::string(
        "module M; class Cell "
        "  public N: int; "
        "  public constructor(n: int) this.N := n; end constructor; "
        "end Cell; "
        "class C "
        "  public constructor(out owned x: Cell) x := new Cell(5); "
        "  end constructor; "
        "  public constructor(out x: int) x := 99; end constructor; "
        "end C; "
        "class Main "
        "  public static method Main(): int "
        "    var n: int; "
        "    new C(out n); "
        "    return n; "
        "  end Main; "
        "end Main;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);
}

// fix #2320 (post-review): the symmetric case — the same overload set but
// `new C(out arr[0])` intended for the `out owned Cell` overload (where
// `arr: Array<Cell>` is heap-borrowing) MUST still be rejected with the
// §22.3/§22.4 diagnostic. Pins that the re-resolution still threads the
// correct modifier for the unambiguous single-binding match.
BEZEL_TEST(out_owned_ref_out_diagnostic_2320,
           same_arity_ctor_overload_out_owned_still_rejected) {
    auto r = runAot(std::string(
        "module M; class Cell "
        "  public N: int; "
        "  public constructor(n: int) this.N := n; end constructor; "
        "end Cell; "
        "class C "
        "  public constructor(out owned x: Cell) x := new Cell(5); "
        "  end constructor; "
        "  public constructor(out x: int) x := 99; end constructor; "
        "end C; "
        "class Main "
        "  public static method Main(): int "
        "    var arr := new Array<Cell>(); "
        "    using var seed := new Cell(1); arr.Add(seed); "
        "    new C(out arr[0]); "
        "    return arr[0].N; "
        "  end Main; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(messageContains(r, "§22.3"));
    BEZEL_EXPECT_TRUE(messageContains(r, "§22.4"));
    BEZEL_EXPECT_FALSE(messageContains(r, "#1386"));
}