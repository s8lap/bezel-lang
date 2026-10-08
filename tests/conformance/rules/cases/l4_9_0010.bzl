//! case: l4_9_0010
//! doc: stdlib
//! section: 4.9
//! title: OwningDictionary at a string key and a sendable class value is sendable
//! rule: `OwningDictionary<K, V>`'s element type is `V`. Its keys are held as
//!   a borrowing map's are (spec §28.11), so the borrowing condition applies
//!   to them as well: `K`'s category is sendable-value, `string` or `Type`.
//!   `OwningDictionary<string, Session>` is sendable at a sendable `Session`.
//! cites: 4, 4.9, 30.14, 6.9
//! expect: accept
module Conf;

import System.Threading;

public sealed class Key
  public Id: int := 1;
end Key;

public sealed class Widget
  public Size: int := 2;
end Widget;

public class Program
  public method Demo()
    using var ch := new Channel<OwningDictionary<string, Widget>>();
    using var tx := ch.NewSender();
    var d := new OwningDictionary<string, Widget>();
    tx.Send(move d);
  end Demo;
end Program;
