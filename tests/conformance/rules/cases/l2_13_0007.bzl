//! case: l2_13_0007
//! doc: stdlib
//! section: 2.13
//! title: Type is sendable, so Channel<Type> is an admitted instantiation
//! rule: `Type` is **sendable**: it crosses a domain boundary as a shared
//!   reference, as `string` does.
//! cites: 2.13, 30.14, 6.9
//! expect: accept
//! issue: 5587
module Conf;

import System.Threading;

public class Program
  public method Demo()
    using var ch := new Channel<Type>();
    using var tx := ch.NewSender();
  end Demo;
end Program;
