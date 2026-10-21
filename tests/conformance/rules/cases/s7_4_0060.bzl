//! case: s7_4_0060
//! doc: spec
//! section: 7.4
//! title: a catch arm reads a string narrowed at entry to the try, past non-null stores in the block
//! rule: "a fact holds at a point only when it holds on every path that reaches it" ([§7.4]). "after `x
//!   := expr;` where `expr`'s static type is non-nullable" ([§7.4]) "`x` is narrowed until
//!   invalidated" ([§7.4]). "every statement of the block is a possible throw point for every `catch`
//!   arm and for the `finally`" ([§7.4]). "A `catch` arm and the `finally` block therefore begin with
//!   the facts that held at entry to the protected block, less every fact the block invalidates at any
//!   point (by the forms above), and no fact the block establishes reaches them" ([§7.4]). The two
//!   stores in the block, `s := "z"` and `s := "y"`, are non-nullable, so neither invalidates
//!   anything: the reassignment form invalidates only from a nullable-typed expression, and no `ref` or
//!   `out` argument or lambda capture is involved. No block-established fact reaches an arm, so the
//!   arm's read is narrowed only because the entry fact, the declaration narrowing of `var s: string? :=
//!   "x"`, survives into the arm.
//! cites: 7.4, 31.2, 31.3
//! expect: run
//! stdout: 1
//! issue: 6673
module Conf;

public class Program
  public static method Boom(f: bool)
    if f then throw new InvalidOperationException("x"); end;
  end Boom;

  public static method Main()
    var s: string? := "x";
    try
      s := "z";
      Boom(true);
      s := "y";
    catch
      Exception => Console.WriteLine(s.Length)
    end try;
  end Main;
end Program;
