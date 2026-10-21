//! case: s7_4_0067
//! doc: spec
//! section: 7.4
//! title: a finally reads a string its declaration narrowed, past a try that only forms a lambda
//! rule: The mutable-capture invalidation needs the block to perform an operation that can transfer control
//!   to user code: "a method, constructor, accessor, or user-defined-operator call, an invocation of a
//!   function value, or an `await`" ([§7.4]). The try block only forms a lambda that assigns `s`, which
//!   is "assigned, or passed as `ref`/`out`, anywhere inside any lambda of the enclosing function body"
//!   ([§7.4]), and it invokes nothing, so it invalidates nothing. The `finally` begins with the
//!   declaration narrowing "from a declaration whose initializer is such an `expr`" ([§7.4]), less "less
//!   every fact the block invalidates at any point" ([§7.4]), so `s.Length` reads 1.
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
      const f := lambda() => do s := null; end;
    finally
      Console.WriteLine(s.Length);
    end try;
    return 0;
  end Main;
end P;
