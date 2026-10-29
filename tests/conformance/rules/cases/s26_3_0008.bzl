//! case: s26_3_0008
//! doc: spec
//! section: 26.3
//! title: an extension property on a StringBuilder receiver is found and its receiver runs once
//! rule: An extension property is **expression-bodied only** (`property Name: Type => expr;`) ... Inside the block, methods and computed properties are written as if they were instance members. `this` refers to the receiver.
//! cites: 26.3, 26.1
//! expect: run
//! output: eval r
//!   2
module Conf;

extension StringBuilder
  public property Dbl: int => this.Length * 2;
end;

public class Program
  public static method Tag(s: string): string
    Console.WriteLine("eval " + s);
    return s;
  end Tag;

  public static method Main()
    Console.WriteLine(new StringBuilder(Tag("r")).Dbl);
  end Main;
end Program;
