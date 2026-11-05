//! case: l2_6_1908
//! doc: stdlib
//! section: 2.6
//! title: Guid.CompareTo drives a descending comparer-taking Sort
//! rule: public method Sort(comparer: (T, T) -> int);
//! cites: 2.6, 4.4
//! expect: run
//! output: 22222222-2222-2222-2222-222222222222
//!   11111111-1111-1111-1111-111111111111
//!   00000001-0000-0000-0000-000000000000
module Conf;

class Program
  public static method Main()
    var a: Array<Guid> := [
      Guid.Parse("22222222-2222-2222-2222-222222222222"),
      Guid.Parse("11111111-1111-1111-1111-111111111111"),
      Guid.Parse("00000001-0000-0000-0000-000000000000")];
    a.Sort(lambda(x: Guid, y: Guid): int => y.CompareTo(x));
    for var x in a do Console.WriteLine(x.ToString()); end;
  end Main;
end Program;
