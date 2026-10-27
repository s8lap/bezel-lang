//! case: s3_4_0006
//! doc: spec
//! section: 3.4
//! title: the keyword string and the standard-library name String name one type
//! rule: Each primitive keyword aliases a standard library type, and the two
//!   forms are interchangeable: the lowercase keyword `string` and the
//!   standard-library type name `String` (`System.String`) name the same type,
//!   and the dual-form rule makes the PascalCase form interchangeable with the
//!   keyword everywhere a type is named. So a local declared `String` takes a
//!   `string` literal and reaches the standard library's `String` members —
//!   `Length`, the number of code points (stdlib §9.6).
//! cites: 3.4, 1.3, 9.6
//! expect: run
//! stdout: 3
module Conf;

public class Program
  public static method Main()
    var s: String := "abc";
    Console.WriteLine(s.Length);
  end Main;
end Program;
