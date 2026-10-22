//! case: s7_4_0092
//! doc: spec
//! section: 7.4
//! title: a narrowed nullable value type or record is read at its non-nullable type
//! rule: "The compiler narrows local variables, parameters other than `ref` parameters, and `readonly` fields in the tracked shape below" ([§7.4]), and "`x <> null` or `x is not null` (narrowing the true branch)" ([§7.4]). The same test narrows a nullable `int`, a nullable record and a nullable `DateTime`.
//! expect: run
//! output: 5
//!   3
//!   2024
//!   z
module Conf;
public record R(X: int);
public class P
  static method I(n: int?): string if n <> null then return n.ToString(); end; return "z"; end I;
  static method Rr(n: R?): string if n <> null then return n.X.ToString(); end; return "z"; end Rr;
  static method Du(n: DateTime?): string if n <> null then var k: DateTime := n; return k.Year.ToString(); end; return "z"; end Du;
  public static method Main(): int
    Console.WriteLine(I(5)); Console.WriteLine(Rr(new R(3))); Console.WriteLine(Du(new DateTime(2024, 3, 15)));
    Console.WriteLine(I(null));
    return 0;
  end Main;
end P;
