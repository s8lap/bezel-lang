//! case: l2_19_0004
//! doc: stdlib
//! section: 2.19
//! title: string conforms to none of INumber, ISteppable, IParsable
//! rule: string conforms to none of the three static abstractions —
//!   concatenation is not addition, and parsing a string to a string is
//!   meaningless.
//! cites: 2.19
//! expect: reject
//! diagnostic: IParsable
module Conf;

public class Program
  public static method ParseIt<T>(s: string): T where T: IParsable<T> => T.Parse(s);

  public method Demo()
    _ := ParseIt<string>("x");
  end Demo;
end Program;
