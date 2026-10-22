//! case: s22_6_0096
//! doc: spec
//! section: 22.6
//! title: a narrowed nullable argument selects the overload taking the non-nullable parameter
//! rule: "`F(x: T)` beside `F(x: T?)` on a narrowed `T?` argument selects `F(T)`" ([§22.6]). "The argument's static type is its type at the call" ([§22.6]): the unnarrowed call selects the nullable overload.
//! expect: run
//! issue: 6593
//! output: nullable/value
//!   nullable
module Conf;
public class P
  static method F(x: int): string return "value"; end F;
  static method F(x: int?): string return "nullable"; end F;
  static method Pick(n: int?): string
    var a := F(n);
    if n <> null then return a + "/" + F(n); end;
    return a;
  end Pick;
  public static method Main(): int
    Console.WriteLine(Pick(7));
    Console.WriteLine(Pick(null));
    return 0;
  end Main;
end P;
