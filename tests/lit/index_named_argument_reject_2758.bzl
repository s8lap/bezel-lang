// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §23.5 "Restrictions" (v0.241, owner ruling on #2758, PR #3043),
// verbatim:
//
//   "Index arguments are **positional-only**. The named-argument form
//    (`Identifier :=`, §22.5) is not admitted at an index position — a compile
//    error naming this rule. Indexer parameters may not declare default values,
//    so §22.5's optional-parameter and named-binding machinery is never reached
//    from an index position; an indexer's parameter names are
//    declaration-internal, not caller-bindable API surface (consistent with
//    §23.4's overloading by parameter *type* list)."
//
// Appendix A.4's `Argument` comment records the over-admission this file
// closes: "Named arguments are additionally rejected at the index position
// (§23.5) — admitted by the shared grammar, rejected semantically." A.7's
// `PrimarySuffix ::= "[" ArgList "]"` really is the CALL argument list —
// `Parser::parseArgList` is literally the same function — so every shape below
// PARSES, which is why the rejection has to be semantic.
//
// MEASURED ON 309356e (pre-fix): each of these compiled with NO diagnostic and
// bound by POSITION, silently dropping the name. `Reordered` below did worse
// than that and killed `bezelc` in the LLVM verifier — "GEP base pointer is not
// a vector or a vector of pointers", "Load operand must be a pointer", "Call
// parameter type does not match function signature" — because the `int 1` bound
// to the `string key` slot. So "the name is silently ignored" understated it.
//
// CHECK-DAG throughout: the declaration-side diagnostic (12) is emitted by the
// TYPE-RESOLUTION pass and the argument-side ones by the EXPRESSION pass, so
// output order is not source order and pinning it would pin an accident.
//
// The ACCEPT control for this file is tests/lit/index_positional_accept_2758.bzl
// (clean compile) and the VALUE oracles are in
// tests/sema/index_named_arg_positional_only_2758_test.cpp.
module REJ2758;

class Dict
  public property [key: string, fallback: int]: int
    get => key.Length + fallback;
  end;
end Dict;

class One
  public property [i: int]: int
    get => i * 2;
    set => Console.WriteLine(value);
  end;
end One;

class Base
  public virtual property [i: int]: int => i + 1;
end Base;

class Der: Base
  public override property [i: int]: int => i + 2;

  // (6) `this[…]` — the spelling §23.4 REQUIRES inside a member of a class
  // with its own indexer ("the brackets need a left-hand side").
  public method ViaThis(): int
    return this[i := 4];
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: the named-argument form `i := {{.*}}` is not admitted at an index position{{.*}}(§23.5)
  end ViaThis;

  // (7) `base[…]` — §12.3's base-qualified indexer access.
  public method ViaBase(): int
    return base[i := 4];
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: the named-argument form `i := {{.*}}` is not admitted at an index position{{.*}}(§23.5)
  end ViaBase;
end Der;

class REJ
  // (1) EVERY index argument named, on a multi-parameter user indexer. One
  // diagnostic per named argument — the rule is per-argument, not per-index.
  public static method AllNamed(d: Dict): int
    return d[key := "abcd", fallback := 1];
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: the named-argument form `key := {{.*}}` is not admitted at an index position — index arguments are positional-only
    // CHECK-DAG: :[[@LINE-2]]:{{[0-9]+}}: the named-argument form `fallback := {{.*}}` is not admitted at an index position
  end AllNamed;

  // (2) MIXED — §22.5 admits "named arguments … in any order after positional
  // arguments" at a CALL; §23.5 admits none of it at an index.
  public static method Mixed(d: Dict): int
    return d["abcd", fallback := 1];
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: the named-argument form `fallback := {{.*}}` is not admitted at an index position
  end Mixed;

  // (3) REORDERED — the shape that produced MALFORMED IR pre-fix.
  public static method Reordered(d: Dict): int
    return d[fallback := 1, key := "abcd"];
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: the named-argument form `fallback := {{.*}}` is not admitted at an index position
    // CHECK-DAG: :[[@LINE-2]]:{{[0-9]+}}: the named-argument form `key := {{.*}}` is not admitted at an index position
  end Reordered;

  // (4) A SINGLE-parameter indexer — no reordering is even expressible, and the
  // form is still rejected: the rule is that the parameter names are not
  // caller-bindable API surface at all, not that a reorder is ambiguous.
  public static method SingleParam(o: One): int
    return o[i := 3];
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: the named-argument form `i := {{.*}}` is not admitted at an index position
  end SingleParam;

  // (5) The SETTER side — an IndexExpr on the left of `:=`, and in a compound
  // assignment. §23.4's indexer is one member with two accessors, and §23.5's
  // rule governs the index list, so it must not depend on which accessor runs.
  public static method SetterForms(o: One)
    o[i := 3] := 9;
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: the named-argument form `i := {{.*}}` is not admitted at an index position
    o[i := 4] += 1;
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: the named-argument form `i := {{.*}}` is not admitted at an index position
  end SetterForms;

  // (8)-(11) The BUILT-IN receiver families. None declares a user indexer for a
  // name to bind against, and each maps `indices[0]` to its one index slot
  // positionally — so each silently dropped the name too.
  public static method BuiltinArray(a: Array<int>): int
    return a[index := 0];
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: the named-argument form `index := {{.*}}` is not admitted at an index position
  end BuiltinArray;

  public static method BuiltinDictionary(m: Dictionary<string, int>): int
    return m[key := "k"];
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: the named-argument form `key := {{.*}}` is not admitted at an index position
  end BuiltinDictionary;

  public static method BuiltinString(s: string): char
    return s[i := 0];
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: the named-argument form `i := {{.*}}` is not admitted at an index position
  end BuiltinString;

  public static method InlineArray(): int
    var ia: [3]int := [1, 2, 3];
    return ia[i := 1];
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: the named-argument form `i := {{.*}}` is not admitted at an index position
  end InlineArray;

  // (12) The DECLARATION-SIDE half of the same §23.5 bullet: "Indexer
  // parameters may not declare default values". A.4's IndexerDecl comment
  // records the shared-ParamList over-admission alongside the `static` one.
  // Pre-fix this compiled clean and the default was simply unreachable — there
  // is no argument-less `[]` form, so no call site can omit an index.
  public property [j: int := 0]: int => j + 10;
  // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: indexer parameter `j` may not declare a default value{{.*}}(§23.5)

  public static method Main(): int
    return 0;
  end Main;
end REJ;
