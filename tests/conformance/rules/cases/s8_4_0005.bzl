//! case: s8_4_0005
//! doc: spec
//! section: 8.4
//! title: = and <> in a generic body over a record type parameter and Object compare by value
//! rule: "`a = b` invokes `a.Equals(b)`, with a null-safe wrapper: `null = null` is `true`; `null = nonNull` and `nonNull = null` are `false`." ([§8.4]). "`<>` is the negation." ([§8.4]). A positional record's default equality is "Field-by-field value equality" ([§8.3]), so a boxed `V(1)` equals a `V(1)` through the generic `=`, a `V(2)` does not, a null record equals only a null operand, and `<>` negates each result.
//! cites: 8.3, 9.2
//! expect: run
//! output: True False False True False
//!   False False
module Conf;
record V(X: int) end V;
public class H
  public static method A<T>(a: T?, b: Object?): bool => a = b;
  public static method N<T>(a: T?, b: Object?): bool => b <> a;
end H;
public class Program
  public static method Main(): int
    var ov: Object? := new V(1);
    var on: Object? := null;
    var vN: V? := null;
    var vV: V? := new V(1);
    var vW: V? := new V(2);
    Console.WriteLine($"{H.A<V>(vV, ov)} {H.A<V>(vW, ov)} {H.A<V>(vN, ov)} {H.A<V>(vN, on)} {H.A<V>(vV, on)}");
    Console.WriteLine($"{H.N<V>(vV, ov)} {H.N<V>(vN, on)}");
    return 0;
  end Main;
end Program;
