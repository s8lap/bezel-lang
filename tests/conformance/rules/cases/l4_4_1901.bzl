//! case: l4_4_1901
//! doc: stdlib
//! section: 4.4
//! title: Array<Guid>.Sorted returns a fresh copy in canonical byte order
//! rule: public method Sorted(): owned Array<T> where T: IComparable<T>;
//! cites: 2.6, 14.2
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
    var s := a.Sorted();
    for var x in s do Console.WriteLine(x.ToString()); end;
  end Main;
end Program;
