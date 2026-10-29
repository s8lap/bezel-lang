//! case: s26_5_0040
//! doc: spec
//! section: 26.5
//! title: an extension on Object is found for a StringBuilder chain and its receiver argument runs once
//! rule: for extension blocks on `x`'s type, base types, or implemented interfaces ... - Classes (including `Object`).
//! cites: 26.5, 26.2
//! expect: run
//! output: eval a
//!   5
//! issue: 7775
module Conf;

extension Object
  public method ObjTag(): int => 5;
end;

public class Program
  public static method Tag(s: string): string
    Console.WriteLine("eval " + s);
    return s;
  end Tag;

  public static method Main()
    var n := new StringBuilder(Tag("a")).Append("b").ObjTag();
    Console.WriteLine(n);
  end Main;
end Program;
