//! case: s11_7_8401
//! doc: spec
//! section: 11.7
//! title: Object's static ReferenceEquals named through a static class forms a method reference
//! rule: is inherited by a static class as by every class
//! cites: 11.7, 8.1, 27.6
//! expect: run
//! stdout: True
//! notes: Pins #9028's static-class spelling. `S.ReferenceEquals` names the inherited static;
//!   calling the reference on one instance twice answers True.
module Conf;
import System;
public class K end K;
public static class S end S;
public class Program
  public static method Main(): int
    var k := new K();
    var f := S.ReferenceEquals;
    Console.WriteLine(f(k, k));
    return 0;
  end Main;
end Program;
