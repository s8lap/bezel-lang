//! case: l9_6_0048
//! doc: stdlib
//! section: 9.6
//! title: string.Join called in a loop with named arguments and a user method taking IEnumerable<string>
//! rule: string.Join(separator, items) concatenates the items with the separator between each adjacent pair (stdlib 9.6); an Array<string> is an IEnumerable<string>. Named arguments bind by the declared parameter names separator and items (spec 22.5).
//! cites: 9.6, 22.5
//! expect: run
//! output: 日,abc,😀
//!   日😀abc😀😀
//!   日日abc日😀
//!   abc
//!   日日abc日😀
//!   日,abc,😀
//!   日😀abc😀😀
//!   日日abc日😀
//!   abc
//!   日日abc日😀
module Conf;

public class Program
  public static method Take(xs: IEnumerable<string>): string => string.Join("日", xs);
  public static method Main()
    using var a: Array<string> := ["日", "abc", "😀"];
    for var i in 0..<2 do
      var r1 := string.Join(",", a);
      var r2 := string.Join("😀", a);
      var r3 := Program.Take(a);
      var r4 := string.Join("-", a.Where(lambda(x) => x.Length > 2));
      var r5 := string.Join(items := a, separator := "日");
      Console.WriteLine(r1);
      Console.WriteLine(r2);
      Console.WriteLine(r3);
      Console.WriteLine(r4);
      Console.WriteLine(r5);
    end;
  end Main;
end Program;
