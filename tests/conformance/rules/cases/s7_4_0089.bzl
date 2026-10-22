//! case: s7_4_0089
//! doc: spec
//! section: 7.4
//! title: a null equality test narrows the else branch of an if to non-null
//! rule: "`x = null` or `x is null` (narrowing the *false* branch)" ([§7.4]): on the else path of `q = null` the local `q` is non-null, so `q.Year` reads without a further test.
//! expect: run
//! output: 2024
//!   z
module Conf;
public class P
  static method Bang(n: DateTime?): string
    var q := n;
    if q = null then return "z"; else return q.Year.ToString(); end;
  end Bang;
  public static method Main(): int
    Console.WriteLine(Bang(new DateTime(2024, 3, 15)));
    Console.WriteLine(Bang(null));
    return 0;
  end Main;
end P;
