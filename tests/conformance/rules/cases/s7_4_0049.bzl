//! case: s7_4_0049
//! doc: spec
//! section: 7.4
//! title: a declaration whose initializer is a non-null string narrows a nullable local for an argument
//! rule: "from a declaration whose initializer is such an `expr`" ([§7.4]), and "where `expr`'s static type is non-nullable": a string literal initializer narrows the local, so passing it where a non-nullable string is required is admitted.
//! cites: 7.4
//! expect: run
//! stdout: 1
//! issue: 6223
module Conf;

public static class S
  public static method F(x: string): int => x.Length;
end S;

public class Program
  public static method Main()
    var a: string? := "s";
    Console.WriteLine(S.F(a));
  end Main;
end Program;
