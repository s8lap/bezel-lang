//! case: s7_4_0101
//! doc: spec
//! section: 7.4
//! title: a with composes with and-narrowing, ??= narrowing and a while-loop test
//! rule: "The compiler tracks, per local variable and per parameter other than a `ref` parameter, whether its value is known non-null" and member access on a nullable value is a compile error "unless the compiler has narrowed it to non-null" (§7.4, v0.340); a `with` "result has the same static type as the source" (§9.5, v0.340), so in `A and B`, `B` is checked under `A`'s true facts; after `x ??= expr` with a non-nullable `expr` the `x` is narrowed; and a `while x <> null` body is under the test's true fact.
//! cites: 9.5, 7.7
//! expect: run
//! output: ok1
//!   8
//!   6
//!   4
module Conf;

public record P(Sku: string, Stock: int);
public record G<T>(V: T, N: int);

public class Program
  public static method Main()
    var d: Dictionary<string, P> := new Dictionary<string, P>();
    d["a"] := new P("a", 5);
    var p := d.GetOrNull("a");
    if p <> null and (p with(Stock := 1)).Stock = 1 then
      Console.WriteLine("ok1");
    end;
    var x: P? := new P("b", 2);
    var y: P := x with(Stock := 8);
    Console.WriteLine(y.Stock);
    var q := d.GetOrNull("a");
    var cnt := 0;
    while q <> null do
      var z: P := q with(Stock := q.Stock + 1);
      cnt := cnt + z.Stock;
      q := null;
    end;
    Console.WriteLine(cnt);
    var o: P? := null;
    o ??= new P("c", 3);
    var oz: P := o with(Stock := 4);
    Console.WriteLine(oz.Stock);
  end Main;
end Program;
