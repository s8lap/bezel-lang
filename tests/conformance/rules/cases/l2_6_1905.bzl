//! case: l2_6_1905
//! doc: stdlib
//! section: 2.6
//! title: Array<Guid>.Sort keeps equal Guids adjacent and orders around them
//! rule: CompareTo = lexicographic over the 16 canonical big-endian
//! cites: 4.4, 14.2
//! expect: run
//! output: 00000000-0000-0000-0000-000000000000
//!   00000000-0000-0000-0000-000000000000
//!   00000000-0001-0000-0000-000000000000
//!   00000000-0001-0000-0000-000000000000
//!   00000000-0100-0000-0000-000000000000
//! issue: 8237
module Conf;

class Program
  public static method Main()
    var a: Array<Guid> := [
      Guid.Parse("00000000-0001-0000-0000-000000000000"),
      Guid.Parse("00000000-0000-0000-0000-000000000000"),
      Guid.Parse("00000000-0000-0000-0000-000000000000"),
      Guid.Parse("00000000-0001-0000-0000-000000000000"),
      Guid.Parse("00000000-0100-0000-0000-000000000000")];
    a.Sort();
    for var x in a do Console.WriteLine(x.ToString()); end;
  end Main;
end Program;
