//! case: s7_4_0052
//! doc: spec
//! section: 7.4
//! title: a null declaration initializer does not narrow the local for a non-nullable parameter
//! rule: "The reverse requires a null check or `!`." ([§7.1]); a `null` initializer does not satisfy "where `expr`'s static type is non-nullable", so the declared `string?` local is not narrowed ([§7.4]).
//! cites: 7.4, 7.1
//! expect: reject
//! diagnostic: nullable
module Conf;

public static class S
  public static method F(x: string): int => x.Length;
end S;

public class Program
  public static method Main()
    var s: string? := null;
    Console.WriteLine(S.F(s));
  end Main;
end Program;
