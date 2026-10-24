//! case: l9_6_0027
//! doc: stdlib
//! section: 9.6
//! title: string.Join rejects a string in the items slot
//! rule: public static method Join(separator: string, items: IEnumerable<string>):
//!   string; — the items parameter takes an IEnumerable<string>, and a value of
//!   one type implicitly converts to another when no programmer-visible
//!   information is lost; a `string` implements IEnumerable<char> (§4.6), not
//!   IEnumerable<string>, and §5.1's table lists no conversion from string to
//!   IEnumerable<string> — so the call is a compile error.
//! cites: 9.6, 5.1
//! expect: reject
//! issue: 7198
//! diagnostic: not implicitly convertible
module Conf;

public class Program
  public static method Main(): int
    Console.WriteLine(string.Join("abc", "def"));
    return 0;
  end Main;
end Program;
