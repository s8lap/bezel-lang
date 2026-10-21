//! case: s7_4_0073
//! doc: spec
//! section: 7.4
//! title: a finally reads a narrowed string past a try that only forms nested lambdas
//! rule: The try block forms one lambda inside another and invokes neither, so it performs no operation that
//!   can transfer control to user code. "A `catch` arm and the `finally` block therefore begin with the
//!   facts that held at entry to the protected block" ([§7.4]), and nothing in the block invalidates the
//!   narrowing: forming a lambda is not among the operations listed in "a method, constructor, accessor,
//!   or user-defined-operator call, an invocation of a function value, or an `await`" ([§7.4]). The
//!   `finally` reads `s.Length` as 1.
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
    try
      const g := lambda() => do const f := lambda() => do s := null; end; end;
    finally
      Console.WriteLine(s.Length);
    end try;
    return 0;
  end Main;
end P;
