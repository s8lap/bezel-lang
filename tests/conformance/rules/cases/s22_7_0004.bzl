//! case: s22_7_0004
//! doc: spec
//! section: 22.7
//! title: a this-chain argument may read the delegating constructor's own parameter
//! rule: "The only reads a chaining argument may make are of the
//!   constructor's own parameters, constants, and statics" (§22.10); and
//!   "for a `this(...)` chain, the chained-to constructor runs to
//!   completion (its own steps of this sequence) before control returns
//!   here" (§22.8 step 1).
//! cites: 22.8, 22.10, 12.8
//! expect: run
//! output: hello
module Conf;

public class Label
  private _text: string;
  private _n: int;

  public constructor(text: string, n: int)
    _text := text;
    _n := n;
  end constructor;

  public constructor(text: string)
    this(text, 0);
  end constructor;

  public method Text(): string => _text;
end Label;

public class Program
  public static method Main()
    using var lbl := new Label("hello");
    Console.WriteLine(lbl.Text());
  end Main;
end Program;
