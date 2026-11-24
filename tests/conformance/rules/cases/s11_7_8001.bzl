//! case: s11_7_8001
//! doc: spec
//! section: 11.7
//! title: ReferenceEquals named through a static class is the inherited Object static
//! rule: is inherited by a static class as by every class
//! cites: 11.7, 8.1, 22.12
//! expect: run
//! output: True
//!   False
//! notes: Pins #8795: Object's static ReferenceEquals (8.1) inherited through a type name.
//!   Caveat recorded by the reviewer: the spec's value-kind reservation sentence (8.4)
//!   is read as a declaration reservation, not a rejection of this spelling.
module Conf;
public static class S
  public static method F(): int return 1; end F;
end S;
public class K end K;
public class Program
  public static method Main(): int
    var k := new K();
    var k2 := new K();
    Console.WriteLine(S.ReferenceEquals(k, k));
    Console.WriteLine(S.ReferenceEquals(k, k2));
    return 0;
  end Main;
end Program;
