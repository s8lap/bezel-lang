//! case: l2_6_1907
//! doc: stdlib
//! section: 2.6
//! title: Array<System.Guid>.Sort orders by canonical bytes under the qualified spelling
//! rule: CompareTo = lexicographic over the 16 canonical big-endian
//! cites: 4.4, 14.2
//! expect: run
//! output: 00000001-0000-0000-0000-000000000000
//!   11111111-1111-1111-1111-111111111111
//!   22222222-2222-2222-2222-222222222222
//! issue: 8237
module Conf;

class Program
  public static method Main()
    var a: Array<System.Guid> := [
      Guid.Parse("22222222-2222-2222-2222-222222222222"),
      Guid.Parse("11111111-1111-1111-1111-111111111111"),
      Guid.Parse("00000001-0000-0000-0000-000000000000")];
    a.Sort();
    for var x in a do Console.WriteLine(x.ToString()); end;
  end Main;
end Program;
