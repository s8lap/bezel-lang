//! case: l4_14_0015
//! doc: stdlib
//! section: 4.14
//! title: OwningBuffer at a non-sendable value element is not sendable
//! rule: `OwningBuffer<T>` is sendable iff its element type is a sendable
//!   class type or a sendable value type.
//! cites: 4, 4.14, 30.14, 6.9
//! expect: reject
//! diagnostic: not sendable
module Conf;

import System.Threading;

public record Step(Apply: (int) -> int);

public class Program
  public method Demo()
    using var ch := new Channel<OwningBuffer<Step>>();
  end Demo;
end Program;
