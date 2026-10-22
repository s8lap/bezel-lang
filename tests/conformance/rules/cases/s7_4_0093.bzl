//! case: s7_4_0093
//! doc: spec
//! section: 7.4
//! title: a negated null equality narrows the true branch of not
//! rule: "The `not`-swap applies to **null facts**" ([§7.4]): "`not` swaps the fact sets" ([§7.4]), so the true branch of `not (n = null)` carries the non-null fact of `n`.
//! expect: run
//! output: 2024
//!   z
module Conf;
public class P
  static method Nb(n: DateTime?): string
    if not (n = null) then return n.Year.ToString(); end;
    return "z";
  end Nb;
  public static method Main(): int
    Console.WriteLine(Nb(new DateTime(2024, 3, 15)));
    Console.WriteLine(Nb(null));
    return 0;
  end Main;
end P;
