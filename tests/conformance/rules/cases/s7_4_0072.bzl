//! case: s7_4_0072
//! doc: spec
//! section: 7.4
//! title: a finally calls a lambda that nulls a string, then reads the string
//! rule: The `finally` invokes the lambda, "an invocation of a function value" ([§7.4]), before the read, and
//!   the lambda assigns `s`, so "the variable is mutably captured" ([§7.4]). "the `finally`'s own
//!   statements then establish and invalidate as any straight-line code does" ([§7.4]): the invocation
//!   invalidates the narrowing before `s.Length` is read, and the read is rejected.
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
    const f := lambda() => do s := null; end;
    try
      var k := 1;
    finally
      f();
      Console.WriteLine(s.Length);
    end try;
    return 0;
  end Main;
end P;
