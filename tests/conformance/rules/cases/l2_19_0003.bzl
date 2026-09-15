//! case: l2_19_0003
//! doc: stdlib
//! section: 2.19
//! title: bool conforms to none of INumber, ISteppable, IParsable
//! rule: bool conforms to none — deliberately: its Parse/TryParse are plain
//!   static members, not witnesses.
//! cites: 2.19
//! expect: reject
//! diagnostic: INumber
module Conf;

public class Program
  public static method Zero<T>(): T where T: INumber<T> => T.Zero();

  public method Demo()
    _ := Zero<bool>();
  end Demo;
end Program;
