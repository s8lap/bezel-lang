//! case: s7_4_0091
//! doc: spec
//! section: 7.4
//! title: a null test inside a while loop narrows a local copy on each iteration before its read
//! rule: "`x <> null` or `x is not null` (narrowing the true branch)" ([§7.4]) and "after `x := expr;` where `expr`'s static type is non-nullable" ([§7.4]). Each iteration re-tests the local before the read, and a reassignment to a non-nullable value narrows it again.
//! expect: run
//! output: 2024,2030,
module Conf;
public class P
  static method L(n: DateTime?): string
    var s := ""; var m: DateTime? := n; var i := 0;
    while i < 3 do
      if m <> null then s := s + m.Year.ToString() + ","; m := null; end;
      if i = 1 then m := new DateTime(2030, 1, 1); end;
      i := i + 1;
    end;
    return s;
  end L;
  public static method Main(): int
    Console.WriteLine(L(new DateTime(2024, 3, 15)));
    return 0;
  end Main;
end P;
