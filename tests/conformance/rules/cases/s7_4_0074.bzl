//! case: s7_4_0074
//! doc: spec
//! section: 7.4
//! title: a finally reads a string after a try block passes it by ref to a method
//! rule: "the variable is passed as a `ref` or `out` argument" ([§7.4]): `P.Clear(ref s)` in the try block
//!   invalidates the narrowing, and so does the lambda formed in the block that assigns `s`, since "the
//!   variable is mutably captured" ([§7.4]) and the block performs a call. "less every fact the block
//!   invalidates at any point" ([§7.4]) removes the narrowing from the `finally`, so `s.Length` is
//!   rejected.
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
      P.Clear(ref s);
    finally
      Console.WriteLine(s.Length);
    end try;
    return 0;
  end Main;
end P;
