// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail — see
// tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// Issue #1847 — a TOP-LEVEL bare-identifier `case` pattern must be a type test,
// a named constant, or (over an enum scrutinee) an enum constant.
//
// Spec §19.3 (v0.196): a single-identifier pattern is ambiguous in the grammar
// (§A.7) and disambiguated by resolving the name — "if `Dog` resolves to a
// type, the pattern is a type test; otherwise it is a constant pattern". A bare
// identifier that resolves to NEITHER a type NOR a constant — a runtime value
// (a `var` local or a parameter), or a name that is not in scope at all — is
// well-formed in neither reading and is a compile error.
//
// Before the fix sema performed no such validation and codegen's NamedPattern
// handler returned nullptr for the unresolved name: in STATEMENT position the
// whole arm chain was silently truncated (the mandatory `_` catch-all skipped,
// §19.6 — a silent miscompile that printed neither arm); in EXPRESSION position
// `case.end` was left without a terminator (malformed IR — an ICE that aborts
// the backend). This test fails without the sema reject (the ill-formed program
// compiles) and passes with it (the three §19.3 diagnostics below).
//
// FileCheck matches in source order (statement value-name, statement
// unknown-name, expression value-name).

module M;
class Program
  public static method Main()
    var n := 50;
    var target := 999;

    // (A) statement position, a bare identifier resolving to a runtime VALUE
    //     (the local `target`) — the reported silent miscompile.
    case n of
      target => Console.WriteLine("matched-target"),
      _      => Console.WriteLine("matched-discard")
    end;
    // CHECK: :[[@LINE-3]]:{{[0-9]+}}: `target` is a value, not a type or constant, so it cannot be a top-level `case` pattern (§19.3)

    // (B) statement position, a bare identifier that is NOT IN SCOPE — no
    //     "unresolved identifier" was reported before the fix either.
    case n of
      undefinedName => Console.WriteLine("matched-unknown"),
      _             => Console.WriteLine("matched-discard")
    end;
    // CHECK: :[[@LINE-3]]:{{[0-9]+}}: `undefinedName` does not name a type or a constant, so it cannot be a top-level `case` pattern (§19.3)

    // (C) expression position, a bare identifier resolving to a runtime VALUE
    //     — the reported malformed-IR / ICE.
    var s := case n of
      target => "x",
      _      => "y"
    end;
    Console.WriteLine(s);
    // CHECK: :[[@LINE-4]]:{{[0-9]+}}: `target` is a value, not a type or constant, so it cannot be a top-level `case` pattern (§19.3)
  end Main;
end Program;
