//! case: s7_4_0069
//! doc: spec
//! section: 7.4
//! title: a finally reads a string that a lambda invoked in the try block nulls
//! rule: The try block invokes the lambda, "an invocation of a function value" ([§7.4]), which is one of the
//!   operations that can transfer control to user code. The lambda assigns `s`: "assigned, or passed as
//!   `ref`/`out`, anywhere inside any lambda of the enclosing function body" ([§7.4]). So "the variable
//!   is mutably captured" ([§7.4]) holds in the block, and "less every fact the block invalidates at any
//!   point" ([§7.4]) removes the narrowing from the `finally`, which rejects `s.Length`.
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
      const f := lambda() => do s := null; end;
      f();
    finally
      Console.WriteLine(s.Length);
    end try;
    return 0;
  end Main;
end P;
