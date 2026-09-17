//! case: s7_7_0001
//! doc: spec
//! section: 7.7
//! title: null-coalescing yields a non-nullable result when the right operand is non-nullable
//! rule: `a ?? b` returns `a` if non-null, else `b`; the result type is `T`
//!   when the right operand is non-nullable.
//! expect: run
//! stdout: Anonymous
module Conf;

public class Program
  public static method Main()
    var name: string? := null;
    var display: string := name ?? "Anonymous";
    Console.WriteLine(display);
  end Main;
end Program;
