//! case: l9_6_0028
//! doc: stdlib
//! section: 9.6
//! title: string.Join admits an array in the items slot
//! rule: public static method Join(separator: string, items: IEnumerable<string>):
//!   string; — an Array<string> is an IEnumerable<string> (§4.4), so the items
//!   argument converts to the parameter without loss and the call is admitted;
//!   this is the accepting twin of l9_6_0027, which differs by the items
//!   argument's type alone.
//! cites: 9.6, 5.1, 4.4
//! expect: accept
module Conf;

public class Program
  public static method Combine(parts: Array<string>): string
    return string.Join("abc", parts);
  end Combine;
end Program;
