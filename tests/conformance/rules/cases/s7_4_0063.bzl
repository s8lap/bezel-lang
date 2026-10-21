//! case: s7_4_0063
//! doc: spec
//! section: 7.4
//! title: a read after a try with a catch arm is nullable when the arm exit carries the block's null store
//! rule: "every statement of the block is a possible throw point for every `catch` arm and for the
//!   `finally`" ([§7.4]). "The point after the statement is reached only through the block's normal
//!   completion or an arm's exit, each through the `finally`: it holds the facts common to those exits —
//!   the every-path rule" ([§7.4]). The block's normal completion ends with `s := "y"`, a non-nullable
//!   store, so `s` is non-null there. The arm's entry loses the declaration narrowing, because the block
//!   reassigns `s` "from a nullable-typed expression" ([§7.4]) (`s := null`). The arm's exit holds no
//!   narrowing of `s`, and the common facts omit it, so the read after the `try` is nullable.
//! cites: 7.4, 31.2
//! expect: reject
//! diagnostic: member access `.Length` on a nullable value requires narrowing
//! issue: 6673
module Conf;

public class Program
  public static method Main()
    var s: string? := "x";
    try
      s := null;
      s := "y";
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end try;
    Console.WriteLine(s.Length);
  end Main;
end Program;
