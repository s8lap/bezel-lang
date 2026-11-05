//! case: l2_6_1901
//! doc: stdlib
//! section: 2.6
//! title: Array<Guid>.Sort orders by the first four canonical bytes, unsigned
//! rule: CompareTo = lexicographic over the 16 canonical big-endian
//! cites: 4.4, 14.2
//! expect: run
//! output: 00000001-0000-0000-0000-000000000000
//!   00000002-0000-0000-0000-000000000000
//!   00000100-0000-0000-0000-000000000000
//!   01000000-0000-0000-0000-000000000000
//!   7f000000-0000-0000-0000-000000000000
//!   80000000-0000-0000-0000-000000000000
//!   ff000000-0000-0000-0000-000000000000
//! issue: 8237
module Conf;

class Program
  public static method Main()
    var a: Array<Guid> := [
      Guid.Parse("01000000-0000-0000-0000-000000000000"),
      Guid.Parse("7f000000-0000-0000-0000-000000000000"),
      Guid.Parse("ff000000-0000-0000-0000-000000000000"),
      Guid.Parse("00000100-0000-0000-0000-000000000000"),
      Guid.Parse("00000001-0000-0000-0000-000000000000"),
      Guid.Parse("80000000-0000-0000-0000-000000000000"),
      Guid.Parse("00000002-0000-0000-0000-000000000000")];
    a.Sort();
    for var x in a do Console.WriteLine(x.ToString()); end;
  end Main;
end Program;
