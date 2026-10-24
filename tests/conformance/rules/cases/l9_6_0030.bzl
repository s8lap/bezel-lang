//! case: l9_6_0030
//! doc: stdlib
//! section: 9.6
//! title: string.Join binds a named argument to the parameter it names
//! rule: public static method Join(separator: string, items: IEnumerable<string>):
//!   string; — a named argument binds the parameter it names (§22.5), so
//!   `items := parts` supplies the Array<string> to the `items: IEnumerable<string>`
//!   parameter and `separator := "abc"` supplies the string to the `separator: string`
//!   parameter; an Array<string> is an IEnumerable<string> (§4.4) and a string is a
//!   string, so both arguments convert to their parameters without loss and the
//!   call is admitted.
//! cites: 9.6, 5.1, 22.5, 4.4
//! expect: run
//! stdout: aabcbabcc
//! issue: 7191
module Conf;

public class Program
  public static method Main(): int
    var parts: Array<string> := ["a", "b", "c"];
    Console.WriteLine(string.Join(items := parts, separator := "abc"));
    return 0;
  end Main;
end Program;
