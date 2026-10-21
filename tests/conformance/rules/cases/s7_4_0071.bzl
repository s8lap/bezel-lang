//! case: s7_4_0071
//! doc: spec
//! section: 7.4
//! title: a finally reads a narrowed string before it invokes a lambda that nulls it
//! rule: The lambda is formed before the `try` and invoked only in the `finally`, after the read. "A `catch`
//!   arm and the `finally` block therefore begin with the facts that held at entry to the protected
//!   block" ([§7.4]), and the protected block here is `var k := 1;`, which performs no operation that
//!   transfers control, so the entry fact survives. "Straight-line check-then-use of a captured local"
//!   ([§7.4]) is admitted, and the read comes before the call `f()`: "the `finally`'s own statements then
//!   establish and invalidate as any straight-line code does" ([§7.4]) puts the invalidation after the
//!   read. So `s.Length` prints 1.
//! cites: 7.4, 27.5, 31.2
//! expect: run
//! stdout: 1
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
      Console.WriteLine(s.Length);
      f();
    end try;
    return 0;
  end Main;
end P;
