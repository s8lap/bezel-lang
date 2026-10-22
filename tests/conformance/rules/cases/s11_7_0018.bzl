//! case: s11_7_0018
//! doc: spec
//! section: 11.7
//! cites: 12.2
//! title: a static class's static method may be named GetType
//! rule: "its static members may take any name of that surface, `Equals`, `GetHashCode`, `ToString` and `GetType` included" ([§11.7]). S is a static class, so its static method GetType is not a hiding declaration of [§12.2], and the call S.GetType() returns the declared value 1.
//! expect: run
//! output: 1
module Conf;
public static class S
  public static method GetType(): int => 1;
end S;
public class Program
  public static method Main(): int
    Console.WriteLine(S.GetType());
    return 0;
  end Main;
end Program;
