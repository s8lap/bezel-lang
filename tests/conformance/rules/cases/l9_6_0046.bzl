//! case: l9_6_0046
//! doc: stdlib
//! section: 9.6
//! title: string.Join over arrays of wide, narrow and empty strings
//! rule: string.Join(separator, items) concatenates the items with the separator between each adjacent pair (stdlib 9.6); an Array<string> is an IEnumerable<string>. Non-ASCII text joins like any other.
//! cites: 9.6
//! expect: run
//! output: 日,abc,x
//!   a日b日c
//!   😀😀a
//!   []
//!   only
//!   日
//!   abc
//!   [,,]
//!   日/abc/x
//!   日日abc日x
module Conf;

public class Program
  public static method Main()
    using var a: Array<string> := ["日", "abc", "x"];
    Console.WriteLine(string.Join(",", a));
    using var b: Array<string> := ["a", "b", "c"];
    Console.WriteLine(string.Join("日", b));
    using var c: Array<string> := ["😀", "a"];
    Console.WriteLine(string.Join("😀", c));
    using var e: Array<string> := new Array<string>();
    Console.WriteLine("[" + string.Join(",", e) + "]");
    using var s1: Array<string> := ["only"];
    Console.WriteLine(string.Join("--", s1));
    using var s2: Array<string> := ["日"];
    Console.WriteLine(string.Join("--", s2));
    Console.WriteLine(string.Join("", b));
    using var em: Array<string> := ["", "", ""];
    Console.WriteLine("[" + string.Join(",", em) + "]");
    Console.WriteLine(string.Join("/", a.Where(lambda(x) => x.Length > 0)));
    Console.WriteLine(string.Join("日", a.Select(lambda(x) => x)));
  end Main;
end Program;
