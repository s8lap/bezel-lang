// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §35.5 *The owning match*, §28.3 K8.4 + K7.2/K2.2 (v0.307) — issue #4818.
//
// An owning match's payload binding `w` is a BORROW of a scrutinee consumed
// and disposed at construct completion (K8.3). K7.2 gives an `if` expression
// the union of its selected values' source sets (K2.2), so the arm result
// `(if w.Ping() = 7 then w else other)` carries `w` and escapes the `case` —
// rejected by K8.4. `w` read inside the CONDITION is not a source.
//
// FAIL-WITHOUT: before #4818 the escape check rooted the value with
// `identifierRoot`, which stops at a ConditionalExpr, so this compiled clean
// (and, under #4634's arm-end drop, read freed memory).

// CHECK: this arm's value borrows owning-match payload binding `w`, whose payload does not outlive the `case` construct
module M;
public class Inner
  private _n: string;
  public constructor(n: string) _n := n; end constructor;
  destructor Console.WriteLine($"dispose inner {_n}"); end destructor;
  public method Ping(): int => 11;
  public method Name(): string => _n;
end Inner;
public class Widget
  private _n: string;
  private owned _in: Inner;
  public constructor(n: string) _n := n; _in := new Inner(n); end constructor;
  destructor Console.WriteLine($"dispose {_n}"); end destructor;
  public method Ping(): int => 7;
  public method Name(): string => _n;
  public method In(): Inner => _in;
end Widget;
public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer;
public class Holder
  private _w: Widget?;
  private _i: Inner?;
  public constructor() _w := null; _i := null; end constructor;
  public method Keep(x: Widget): () _w := x; end Keep;
  public method KeepIn(x: Inner): () _i := x; end KeepIn;
  public method Peek(): int => _w!.Ping() + _i!.Ping();
end Holder;
public class Program
  public static method Take(owned x: Widget): () Console.WriteLine($"take {x.Name()}"); end Take;
  public static method Mk(): Outer => Outer.W(new Widget("a"), new Widget("b"));
  public static method Main(): int
    using var other := new Widget("other");
    var o := Mk();
    var r := (case move o of W(var w, _) => (if w.Ping() = 7 then w else other), X => other);
    Console.WriteLine($"r={r.Name()}");
    return 0;
  end Main;
end Program;
