//! case: l4_4_0007
//! doc: stdlib
//! section: 4.4
//! title: Sorted natural order is rejected for a non-IComparable element type
//! rule: The parameterless natural-order Sort/Sorted is a conditionally
//!   admitted member: admitted only where T: IComparable<T>, with the
//!   condition decided after substitution and the rejection reported at
//!   the call, never at the declaration.
//! cites: 4.4, 14.2
//! expect: reject
//! diagnostic: IComparable
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    using var arr: Array<Widget> := [];
    using var sorted := arr.Sorted();
  end Demo;
end Program;
