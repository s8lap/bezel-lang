//! case: s7_4_0068
//! doc: spec
//! section: 7.4
//! title: a finally reads a nullable string after the try block stores null into it directly
//! rule: The try block stores the null literal into `s`, a nullable-typed expression, which is an
//!   invalidating form: "the variable is reassigned from a nullable-typed expression" ([§7.4]). The
//!   `finally` begins with "less every fact the block invalidates at any point" ([§7.4]), so `s` is not
//!   narrowed at the read, and "Member access, method calls, and indexing on a nullable value are compile
//!   errors unless the compiler has narrowed it to non-null" ([§7.4]) rejects `s.Length`.
//! cites: 7.4, 31.2
//! expect: reject
//! diagnostic: member access `.Length` on a nullable value requires narrowing
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
      s := null;
    finally
      Console.WriteLine(s.Length);
    end try;
    return 0;
  end Main;
end P;
