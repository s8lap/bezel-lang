//! case: s7_4_0065
//! doc: spec
//! section: 7.4
//! title: a finally reads a string the block narrowed; its entry fact is null, so the read is nullable
//! rule: "every statement of the block is a possible throw point for every `catch` arm and for the
//!   `finally`" ([§7.4]). "A `catch` arm and the `finally` block therefore begin with the facts that held
//!   at entry to the protected block, less every fact the block invalidates at any point (by the forms
//!   above), and no fact the block establishes reaches them" ([§7.4]). The declaration `var s: string? :=
//!   null` establishes no narrowing, since its initializer is not non-nullable ("from a declaration whose
//!   initializer is such an `expr`" ([§7.4])). The block's `s := "x"` establishes narrowing that does not
//!   reach the `finally`, so `s` is nullable there and the read is a compile error.
//! cites: 7.4, 31.2
//! expect: reject
//! diagnostic: member access `.Length` on a nullable value requires narrowing
//! issue: 6673
module Conf;

public class Program
  public static method Log(m: string)
    Console.WriteLine(m);
  end Log;

  public static method Main()
    var s: string? := null;
    try
      s := "x";
      Log("a");
    finally
      Console.WriteLine(s.Length);
    end try;
  end Main;
end Program;
