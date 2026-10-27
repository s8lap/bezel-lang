//! case: l9_6_0029
//! doc: stdlib
//! section: 9.6
//! title: string.Join rejects a reordered named-argument call whose slots carry the wrong types
//! rule: public static method Join(separator: string, items: IEnumerable<string>):
//!   string; — a named argument binds the parameter it names (§22.5), so
//!   `items := "abc"` supplies a `string` to the `items: IEnumerable<string>`
//!   parameter and `separator := parts` supplies an `Array<string>` to the
//!   `separator: string` parameter; a value of one type implicitly converts to
//!   another when no programmer-visible information is lost, and §5.1's table
//!   lists neither conversion — so the call is a compile error.
//! cites: 9.6, 5.1, 22.5
//! expect: reject
//! issue: 7198
//! diagnostic: not implicitly convertible
module Conf;

public class Program
  public static method Main(): int
    var parts: Array<string> := ["a", "b", "c"];
    Console.WriteLine(string.Join(items := "abc", separator := parts));
    return 0;
  end Main;
end Program;
