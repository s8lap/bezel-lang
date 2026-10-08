//! case: l4_20_0007
//! doc: stdlib
//! section: 4.20
//! title: Counter<string> is sendable and crosses as a channel element
//! rule: `Counter<K>` is sendable iff `K`'s element category (language spec
//!   §28.11) is sendable-value, `string` or `Type`, the borrowing maps' key
//!   condition. `Counter<string>` is sendable.
//! cites: 4.20, 4, 30.14, 6.9
//! expect: accept
//! issue: 3848
module Conf;

import System.Threading;

public sealed class Widget
  public Size: int := 2;
end Widget;

public class Program
  public method Demo()
    using var ch := new Channel<Counter<string>>();
    using var tx := ch.NewSender();
    var tally := new Counter<string>();
    tx.Send(move tally);
  end Demo;
end Program;
