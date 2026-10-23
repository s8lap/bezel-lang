//! case: s5_1_0134
//! doc: spec
//! section: 5.1
//! title: an argument not implicitly convertible to the element type is rejected at a constraint head
//! rule: A value of one type implicitly converts to another when no
//!   programmer-visible information is lost — the §5.1 conversions are the
//!   only ones admitted at an argument position, so an argument whose type
//!   does not convert to the parameter's type is not applicable to the
//!   candidate (§22.6).
//! cites: 5.1, 22.6
//! expect: reject
//! diagnostic: implicitly convert
module Conf;

public class Program
  public static method G<T>(x: T): int where T: IList<int>
    x.Add("not an int");
    return 0;
  end G;

  public static method Main(): int
    var a: Array<int> := [1, 2, 3];
    Console.WriteLine(Program.G(a));
    return 0;
  end Main;
end Program;
