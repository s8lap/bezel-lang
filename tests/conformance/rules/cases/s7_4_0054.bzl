//! case: s7_4_0054
//! doc: spec
//! section: 7.4
//! title: a copy of a nullable local that was never narrowed does not narrow, so passing the copy is rejected
//! rule: "The reverse requires a null check or `!`." ([§7.1]); the initializer `t` has the nullable static type `string?`, which fails "where `expr`'s static type is non-nullable", so the copy is not narrowed ([§7.4]).
//! cites: 7.4, 7.1
//! expect: reject
//! diagnostic: nullable
module Conf;

public static class S
  public static method F(x: string): int => x.Length;
end S;

public class P
  public static method MS(f: bool): string?
    if f then return "s"; end;
    return null;
  end MS;
end P;

public class Program
  public static method Main()
    var t: string? := P.MS(false);
    var s: string? := t;
    Console.WriteLine(S.F(s));
  end Main;
end Program;
