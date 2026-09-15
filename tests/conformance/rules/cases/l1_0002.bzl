//! case: l1_0002
//! doc: stdlib
//! section: 1
//! title: ReadKeyOrNull is the *OrNull twin, answering the same absence with null
//! rule: The *OrNull convenience tier answers the same expected-absence condition
//!   its throwing twin raises on; Console.ReadKeyOrNull() returns null at end of
//!   input where Console.ReadKey() raises EndOfStreamException.
//! cites: 2.1
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var k := Console.ReadKeyOrNull();
    Console.WriteLine(k = null);
  end Main;
end Program;
