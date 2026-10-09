//! case: l4_14_0017
//! doc: stdlib
//! section: 4.14
//! title: OwningBuffer at a borrow-bearing record element is not sendable
//! rule: `OwningBuffer<T>` is sendable iff its element type is a sendable
//!   class type or a sendable value type: a buffer of a borrow-bearing record
//!   is not sendable.
//! cites: 4.14, 30.14
//! expect: reject
//! diagnostic: not sendable
module Conf;

import System.Threading;

public class Widget
  public constructor()
  end constructor;
end Widget;

public record Node(Owner: Widget);  // borrow-bearing: class field

public class Program
  public method Demo()
    using var ch := new Channel<OwningBuffer<Node>>();
  end Demo;
end Program;