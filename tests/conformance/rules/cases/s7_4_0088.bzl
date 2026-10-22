//! case: s7_4_0088
//! doc: spec
//! section: 7.4
//! title: a right operand of and reads a value narrowed by the left operand's null test
//! rule: "in `A and B`, `B` (and the true branch) is checked under `A`'s true-facts" ([§7.4]): the left operand's null test narrows the right operand and the true branch, so `n.Year` is read under `n <> null`.
//! expect: run
//! output: 2024
//!   z
//!   z
module Conf;
public class P
  static method And(n: DateTime?): string
    if n <> null and n.Year > 2000 then return n.Year.ToString(); end;
    return "z";
  end And;
  public static method Main(): int
    Console.WriteLine(And(new DateTime(2024, 3, 15)));
    Console.WriteLine(And(new DateTime(1999, 1, 1)));
    Console.WriteLine(And(null));
    return 0;
  end Main;
end P;
