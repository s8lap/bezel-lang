//! case: s26_5_0067
//! doc: spec
//! section: 26.5
//! title: an extension method named like a declared instance property is never consulted for the property read
//! rule: If `x`'s declared type (or any base type) *declares* any instance
//!   member named `Foo` — of any arity, applicability, or accessibility —
//!   instance resolution is committed and extension blocks are **never
//!   consulted** for that name on that receiver.
//! cites: 26.5
//! expect: run
//! issue: 7488
//! output: 3
module Conf;

extension Array<int>
  public method Count(): int => 42;
end;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    Console.WriteLine(a.Count);
  end Main;
end Program;
