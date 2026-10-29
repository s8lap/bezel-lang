//! case: l9_6_0047
//! doc: stdlib
//! section: 9.6
//! title: string.Join with a Latin-1 separator and a wide-string result compares equal to its literal
//! rule: string.Join(separator, items) concatenates the items with the separator between each adjacent pair (stdlib 9.6); an Array<string> is an IEnumerable<string>. The result equals the literal with the same code points.
//! cites: 9.6
//! expect: run
//! output: éñÿñabc
//!   True
//!   True
//!   1
module Conf;

public class Program
  public static method Main()
    using var a: Array<string> := ["é", "ÿ", "abc"];
    var r := string.Join("ñ", a);
    Console.WriteLine(r);
    Console.WriteLine(r = "éñÿñabc");
    using var b: Array<string> := ["日", "x"];
    var r2 := string.Join("", b);
    Console.WriteLine(r2 = "日x");
    Console.WriteLine(r2.IndexOf("x"));
  end Main;
end Program;
