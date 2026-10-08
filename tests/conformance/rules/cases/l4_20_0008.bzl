//! case: l4_20_0008
//! doc: stdlib
//! section: 4.20
//! title: Counter at a class-typed key is not sendable, though the key class is
//! rule: `Counter<string>` is sendable; a `Counter<K>` at a class- or
//!   interface-typed `K` carries borrows and is not.
//! cites: 4.20, 4, 30.14, 6.9
//! expect: reject
//! diagnostic: not sendable
//! issue: 3848
module Conf;

import System.Threading;

public sealed class Widget
  public Size: int := 2;
end Widget;

public class Program
  public method Demo()
    using var ch := new Channel<Counter<Widget>>();
    using var tx := ch.NewSender();
    var tally := new Counter<Widget>();
    tx.Send(move tally);
  end Demo;
end Program;
