//! case: s8_6_4702
//! doc: spec
//! section: 8.6
//! title: a Guid? parameter narrowed by a null check boxes to a boxed Guid
//! rule: If the value is non-null, the result is a boxed `T` (the non-nullable form). The nullable
//!   wrapper is stripped during boxing.
//! cites: 7.4, 5.1
//! expect: run
//! output: 00000000-0000-0000-0000-000000000000
//!   1
//!   0
module Conf;

public class Program
  public static method F(n: Guid?): int
    if n <> null then
      var o: Object := n;
      Console.WriteLine(o.ToString());
      return 1;
    end if;
    return 0;
  end F;

  public static method Main()
    Console.WriteLine(F(Guid.Empty));
    Console.WriteLine(F(null));
  end Main;
end Program;
