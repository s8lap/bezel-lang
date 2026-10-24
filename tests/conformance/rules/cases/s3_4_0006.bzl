//! case: s3_4_0006
//! doc: spec
//! section: 3.4
//! title: a local declared with the PascalCase spelling String is the same type as a string local
//! rule: The lowercase keyword `string` and the standard-library type name
//!   `String` (`System.String`) name the same type — they are interchangeable in
//!   source — so the undeclared member `Empty` is rejected on a `String`-declared
//!   local exactly as on a `string`-declared one.
//! cites: 3.4, 9.6, 22.6
//! expect: reject
//! diagnostic: Empty
module Conf;

public class Program
  public static method Main(): int
    var s: String := "abc";
    Console.WriteLine(s.Empty.Length);
    return 0;
  end Main;
end Program;
