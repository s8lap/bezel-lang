//! case: s8_4_7201
//! doc: spec
//! section: 8.4
//! title: Object.ReferenceEquals called with its type name compares object
//!   identity of two class instances
//! rule: ignoring any `Equals` override. It is an ordinary static method
//!   with `Object?` parameters
//! cites: 8.1, 8.4
//! expect: run
//! output: False
//!   True
module Conf;

public class K
  public constructor()
  end constructor;
end K;

public class Program
  public static method Main()
    var k := new K();
    var k2 := new K();
    Console.WriteLine(Object.ReferenceEquals(k, k2));
    Console.WriteLine(Object.ReferenceEquals(k, k));
  end Main;
end Program;
