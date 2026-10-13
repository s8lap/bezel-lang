//! case: l4_14_0019
//! doc: stdlib
//! section: 4.14
//! title: OwningBuffer at a sendable class element is sendable
//! rule: `OwningBuffer<T>` is sendable iff its element type is a sendable
//!   class type or a sendable value type: `OwningBuffer<sealed_class>` accepts.
//! cites: 4.14, 30.14
//! expect: accept
module Conf;

import System.Threading;

public sealed class Widget
  public Size: int := 2;
end Widget;

public class Program
  public method Demo()
    using var ch := new Channel<OwningBuffer<Widget>>();
    using var tx := ch.NewSender();
  end Demo;
end Program;