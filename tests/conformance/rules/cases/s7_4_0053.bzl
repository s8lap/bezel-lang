//! case: s7_4_0053
//! doc: spec
//! section: 7.4
//! title: a nullable call result as declaration initializer does not narrow the local for an argument
//! rule: "The reverse requires a null check or `!`." ([§7.1]); the call's static type is `string?`, which fails "where `expr`'s static type is non-nullable", so the declared local is not narrowed ([§7.4]).
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
    var s: string? := P.MS(false);
    Console.WriteLine(S.F(s));
  end Main;
end Program;
