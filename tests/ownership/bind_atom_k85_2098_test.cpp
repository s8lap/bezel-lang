// Disposal-correctness — the §17.15 binding atom's K8.5 boundary (issue
// #2098; spec §17.15, §28.3 "Binding-atom bindings are named temporaries",
// kernel K8.5/K8.4/K3.15, Appendix C.3 — all v0.216).
//
// §28.3 (v0.216): "A binding atom's clause whose initializer is an owned
// production produces a NAMED unbound owned temporary: within the atom the
// binding is an ordinary named owned binding — K3's consuming positions
// discharge it, and a consumed binding is not disposed by the boundary pass —
// but its boundary is the ATOM ITSELF (K8.5) ... undischarged at the atom's
// completion, it is disposed there, after the body's evaluation, in reverse
// clause order, before the enclosing expression continues; the exception edge
// discharges it during unwinding on the same establishment-ordered cleanup
// stack. No must-use warning attaches."
//
// Each case pins one row of that paragraph natively under the checked oracle
// (runAotChecked — ASan/LSan): a missed atom-boundary disposal is a DIRECT
// leak (r.ran flips false); a boundary disposal that wrongly doubles a K3
// consume is a double-free/UAF (r.ran flips false). Destructors print
// per-instance markers so the ORDER (reverse clause order; boundary before
// the enclosing statement continues) is asserted from stdout, not inferred.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"  // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

const char* kPrelude =
    "module M; "
    "class Res "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
    "end Res; "
    "class W "
    "  public static method Sink(owned r: Res): int "
    "    var v := r.Id; "
    "    r.Dispose(); "
    "    return v; "
    "  end Sink; "
    "  public static method Make(id: int): owned Res "
    "    return new Res(id); "
    "  end Make; "
    "  public static method Boom(): int "
    "    throw new InvalidOperationException(\"boom\"); "
    "  end Boom; "
    "end W; ";

} // namespace

// §17.15 / K8.5 v0.216: two owned clauses, neither consumed — disposed at the
// atom's completion, AFTER the body's evaluation, in REVERSE clause order
// (D2 then D1), BEFORE the enclosing statement continues (both precede the
// `S3` the statement after the declaration prints). Without the boundary
// disposal both are direct LSan leaks.
BEZEL_TEST(bind_atom_k85_2098, unconsumed_clauses_disposed_reverse_spec_28_3) {
    auto r = runAotChecked(std::string(kPrelude) +
        "class C "
        "  public static method Main(): int "
        "    var sum := (var a := new Res(1); var b := new Res(2); "
        "                a.Id + b.Id); "
        "    Console.WriteLine(\"S\" + sum.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    const std::string& out = r.stdoutText;
    BEZEL_EXPECT_EQ(countOccurrences(out, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(out, "D2"), 1);
    std::size_t d2 = out.find("D2"), d1 = out.find("D1"), s = out.find("S3");
    BEZEL_EXPECT_TRUE(d2 != std::string::npos && d1 != std::string::npos &&
                      s != std::string::npos);
    BEZEL_EXPECT_TRUE(d2 < d1);  // reverse clause order
    BEZEL_EXPECT_TRUE(d1 < s);   // before the enclosing statement continues
}

// §17.15 / K8.5 / K3.3 v0.216: a clause consumed INSIDE the atom by a K3
// position (a call-site `owned` argument) — "K3's consuming positions
// discharge it, and a consumed binding is not disposed by the boundary
// pass." The sink disposes exactly once; a boundary disposal on top would be
// a double-free (ASan). Exactly one D7.
BEZEL_TEST(bind_atom_k85_2098, clause_consumed_by_owned_arg_spec_28_3) {
    auto r = runAotChecked(std::string(kPrelude) +
        "class C "
        "  public static method Main(): int "
        "    var v := (var c := new Res(7); W.Sink(move c)); "
        "    Console.WriteLine(\"V\" + v.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D7"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "V7"), 1);
}

// §17.15 / K8.5 / K3.15 v0.216: EXCEPTION-EDGE PARITY — "the exception edge
// discharges it during unwinding on the same establishment-ordered cleanup
// stack." The body throws after both clauses are established: both are
// disposed during unwinding, in reverse establishment order (D2 before D1),
// before the catch arm runs. Without the parity both leak (LSan).
BEZEL_TEST(bind_atom_k85_2098, exception_edge_parity_spec_k3_15) {
    auto r = runAotChecked(std::string(kPrelude) +
        "class C "
        "  public static method Main(): int "
        "    try "
        "      _ := (var a := new Res(1); var b := new Res(2); W.Boom()); "
        "    catch var e: InvalidOperationException => "
        "      Console.WriteLine(\"C\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    const std::string& out = r.stdoutText;
    BEZEL_EXPECT_EQ(countOccurrences(out, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(out, "D2"), 1);
    std::size_t d2 = out.find("D2"), d1 = out.find("D1"), c = out.find("C");
    BEZEL_EXPECT_TRUE(d2 != std::string::npos && d1 != std::string::npos &&
                      c != std::string::npos);
    BEZEL_EXPECT_TRUE(d2 < d1);  // reverse establishment order on unwind
    BEZEL_EXPECT_TRUE(d1 < c);   // during unwinding, before the catch arm
}

// §17.15 / K3.4 v0.216: "a marked transfer `var y := owned x;` is K3.4's
// binding transfer, consuming the outer binding on the atom's straight-line
// evaluation" — the clause ADOPTS the obligation, so the atom boundary (not
// the outer scope) disposes: exactly one D3, no double-free at Main's end.
BEZEL_TEST(bind_atom_k85_2098, marked_transfer_clause_adopts_spec_k3_4) {
    auto r = runAotChecked(std::string(kPrelude) +
        "class C "
        "  public static method Main(): int "
        "    var s := new Res(3); "
        "    var v := (var t := owned s; t.Id); "
        "    Console.WriteLine(\"V\" + v.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D3"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "V3"), 1);
}

// §17.15 / §28.3 producers row v0.216: the atom's OWN classification is the
// BODY's — a `using var` bound to an atom whose body is an owned production
// owns the body's value (disposed at the STATEMENT scope's end), while the
// atom's internal owned clause is disposed at the ATOM's end. K8.5 is
// "deliberately tighter than the enclosing full expression": D1 (the clause,
// at the atom's completion) precedes X2 (the statement after it), which
// precedes D2 (the using var, at scope end).
BEZEL_TEST(bind_atom_k85_2098, atom_classifies_as_body_using_var_spec_28_3) {
    auto r = runAotChecked(std::string(kPrelude) +
        "class C "
        "  public static method Main(): int "
        "    using var x := (var w := new Res(1); W.Make(2)); "
        "    Console.WriteLine(\"X\" + x.Id.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    const std::string& out = r.stdoutText;
    BEZEL_EXPECT_EQ(countOccurrences(out, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(out, "D2"), 1);
    std::size_t d1 = out.find("D1"), x = out.find("X2"), d2 = out.find("D2");
    BEZEL_EXPECT_TRUE(d1 != std::string::npos && x != std::string::npos &&
                      d2 != std::string::npos);
    BEZEL_EXPECT_TRUE(d1 < x);  // atom boundary is tighter than the statement
    BEZEL_EXPECT_TRUE(x < d2);  // using var disposes at scope end
}

// §17.15 / §28.3 v0.216: "a borrow initializer makes its binding a borrow
// carrying the initializer's source set (K2)" — a borrow clause takes NO
// disposal obligation: the outer `using var` remains the sole owner (exactly
// one D5; a boundary disposal of the borrow clause would double-free).
BEZEL_TEST(bind_atom_k85_2098, borrow_clause_no_boundary_disposal_spec_k2) {
    auto r = runAotChecked(std::string(kPrelude) +
        "class C "
        "  public static method Main(): int "
        "    using var r := new Res(5); "
        "    var v := (var q := r; q.Id + 1); "
        "    Console.WriteLine(\"V\" + v.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D5"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "V6"), 1);
}
