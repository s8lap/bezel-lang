//! case: s27_8_1408
//! doc: spec
//! section: 27.8
//! title: a generic alias whose own type parameter is spelled Guid binds it in its body
//! rule: Each instantiation of the alias is interchangeable with the corresponding instantiation of
//!   the aliased type.
//! cites: 27.8, 2.7
//! expect: run
//! stdout: 1
module Conf;
type L<Guid> := Array<Guid>;
public class U
  public static method F(d: L<int>): int
    return d.Count;
  end F;
end U;
public class Program
  public static method Main()
    var d := new Array<int>();
    d.Add(1);
    Console.WriteLine(U.F(d));
  end Main;
end Program;
