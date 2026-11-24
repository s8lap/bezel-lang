//! case: s22_12_8001
//! doc: spec
//! section: 22.12
//! title: ReferenceEquals named through a class type name compares identity
//! rule: A static member declared on a base class is a member of every class derived from it
//! cites: 22.12, 8.1, 2.7
//! expect: run
//! output: True
//!   False
//!   True
//!   False
//! notes: Pins #8795 (the call form through a class type name; the argument parameters are
//!   Object?).
module Conf;
public class K end K;
public class Program
  public static method Main(): int
    var k := new K();
    var k2 := new K();
    Console.WriteLine(K.ReferenceEquals(k, k));
    Console.WriteLine(K.ReferenceEquals(k, k2));
    Console.WriteLine(K.ReferenceEquals(null, null));
    Console.WriteLine(K.ReferenceEquals(k, "x"));
    return 0;
  end Main;
end Program;
