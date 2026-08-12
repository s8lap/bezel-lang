// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// The ANTI-OVER-REJECT control for #2758's §23.5 rejection (v0.241, PR #3043).
//
// #2758 files its own warning about this: "this is a rejection-or-resolution
// change, so the false-reject risk is the hazard: #2709, #2713 and #2722 each
// took multiple rounds on exactly that this run. Enumerate every spelling
// reaching the new predicate and ship an accept-control for each before
// trusting it."
//
// The predicate is `!CallArgument::name.empty()`, which `Parser::parseArgList`
// sets ONLY for a literal `Identifier ":="` prefix. Every POSITIONAL spelling
// below leaves it empty and must compile with no diagnostic at all:
//
//   * a user indexer, single- and multi-parameter, get and set;
//   * an OVERLOADED indexer (§23.4 "Indexers may be overloaded by parameter
//     type list") and a GENERIC one;
//   * a record indexer (§23.4 "Records may declare read-only indexers");
//   * `this[i]` (the spelling §23.4 requires) and `base[i]` (§12.3);
//   * the built-in receiver families — `Array<T>`, `Dictionary<K,V>`,
//     `string` (§4.6), an inline array (§15.1), a span;
//   * a trailing comma in the index list (§1.6 / proposal 0059, v0.214);
//   * an index whose VALUE is a local literally named `key` — the one shape
//     that could be confused for the named form and is not, because the parser
//     only takes the prefix when `:=` follows the identifier;
//   * and — the other direction — the named form where it IS admitted: a
//     method call (§22.5) and a constructor call (§22.5/§22.7). Those are
//     different node kinds and must stay untouched.
//
// The VALUES these produce are asserted in
// tests/sema/index_named_arg_positional_only_2758_test.cpp; this file's oracle
// is only that nothing is rejected and nothing degrades.
//
// NODIAG-NOT: §23.5
// NODIAG-NOT: index arguments are positional-only
// NODIAG-NOT: internal:

module ACC2758;

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

class Ov
  public property [i: int]: int => i;
  public property [s: string]: int => s.Length;
end Ov;

class Box<T>
  private _v: T;
  public constructor(v: T)
    _v := v;
  end;
  public property [i: int]: T => _v;
end Box;

record Rec(BaseValue: int)
  public property [i: int]: int => BaseValue + i;
end Rec;

class Base
  public virtual property [i: int]: int => i + 1;
end Base;

class Der: Base
  public override property [i: int]: int => i + 2;
  public method ViaThis(): int
    return this[4];
  end ViaThis;
  public method ViaBase(): int
    return base[4];
  end ViaBase;
end Der;

class Cfg
  public readonly property N: int;
  public constructor(verbose: bool, n: int)
    N := n;
  end;
end Cfg;

class ACC
  public static method Add(a: int, b: int): int
    return a + b;
  end Add;

  public static method Main(): int
    var d := new Dict();
    var total := d["abcd", 1];          // multi-parameter, positional
    total := total + d["abcd", 1,];     // trailing comma (§1.6, v0.214)

    // An index whose VALUE is a local named `key` — NOT the named form.
    var key := "abcd";
    total := total + d[key, 1];

    var o := new One();
    total := total + o[3];              // single-parameter getter
    o[3] := 9;                          // the SETTER side

    var v := new Ov();
    total := total + v[1] + v["ab"];    // §23.4 overload by parameter type list

    using var b := new Box<int>(5);
    total := total + b[0];              // generic indexer

    var r := new Rec(3);
    total := total + r[2];              // record indexer

    using var der := new Der();
    total := total + der.ViaThis() + der.ViaBase();

    using var a := new Array<int>();
    a.Add(7);
    total := total + a[0];              // Array<T>
    var sp := a.AsSpan();
    total := total + sp[0];             // a span

    using var m := new Dictionary<string, int>();
    m["k"] := 5;
    total := total + m["k"];            // Dictionary<K,V>

    var s := "abc";
    total := total + int(s[0]);         // string (§4.6)

    var ia: [3]int := [1, 2, 3];
    total := total + ia[1];             // inline array (§15.1)

    // The named form where §22.5 DOES admit it — a call and a `new`.
    total := total + Add(a := 1, b := 2);
    using var cfg := new Cfg(verbose := true, n := 3);
    total := total + cfg.N;

    Console.WriteLine(total);
    return 0;
  end Main;
end ACC;
