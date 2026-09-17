//! case: l4_3_0002
//! doc: stdlib
//! section: 4.3
//! title: OwningArray<T> does not implicitly convert to Array<T>
//! rule: The two hierarchies do not share mutating interfaces, and there
//!   is no implicit conversion between the families — an OwningArray<T> is
//!   not an Array<T>.
//! cites: 4.3, 28.11
//! expect: reject
//! diagnostic: Array
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    using var pool := new OwningArray<Widget>();
    var view: Array<Widget> := pool;
  end Demo;
end Program;
