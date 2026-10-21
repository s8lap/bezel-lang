//! case: s7_4_0070
//! doc: spec
//! section: 7.4
//! title: a finally reads a string that a lambda nulls, after a method call in the try block
//! rule: The try block calls `P.Boom(false)`, "a method, constructor, accessor, or user-defined-operator
//!   call" ([§7.4]), which can transfer control to user code. The lambda formed later in the same block
//!   assigns `s`, and the rule is over "anywhere inside any lambda of the enclosing function body"
//!   ([§7.4]), so the position of the formation does not matter. "the variable is mutably captured"
//!   ([§7.4]) therefore holds in the block, and "less every fact the block invalidates at any point"
//!   ([§7.4]) leaves the `finally` unnarrowed, so `s.Length` is rejected.
//! cites: 7.4, 27.5, 31.2
//! expect: reject
//! diagnostic: cannot narrow `T?` accessed through a mutably-captured local
//! issue: 6673
module Conf;
static class S
  public static method F(x: string): int => x.Length;
end S;
public class P
  public static method MS(f: bool): string?
    if f then return "s"; end;
    return null;
  end MS;
  public static method Boom(f: bool): int
    if f then throw new InvalidOperationException("x"); end;
    return 1;
  end Boom;
  public static method Clear(ref v: string?)
    v := null;
  end Clear;
  public static method Log(m: string)
    Console.WriteLine(m);
  end Log;

  public static method Main(): int
    var s: string? := "x";
    try
      P.Boom(false);
      const f := lambda() => do s := null; end;
    finally
      Console.WriteLine(s.Length);
    end try;
    return 0;
  end Main;
end P;
