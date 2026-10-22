//! case: s22_6_0094
//! doc: spec
//! section: 22.6
//! title: a call whose argument fits no overload candidate is rejected with no applicable overload
//! rule: "If no candidate is applicable at all"
//! cites: 22.6, 5.1
//! expect: reject
//! diagnostic: no overload of `F` is applicable to these arguments
module Conf;

public class C
  public static method F(x: IReadOnlyList<int>): int => 1;
  public static method F(x: long?): int => 2;

  public static method Main(): int
    Console.WriteLine($"{C.F("a")}");
    return 0;
  end Main;
end C;
