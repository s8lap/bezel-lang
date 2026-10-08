//! case: l4_9_0009
//! doc: stdlib
//! section: 4.9
//! title: OwningDictionary at a class-typed key is not sendable, whatever the key's own sendability
//! rule: Its keys are held as a borrowing map's are (spec §28.11), so the
//!   borrowing condition applies to them as well: `K`'s category is
//!   sendable-value, `string` or `Type`. At a class- or interface-typed `K`
//!   the map carries borrows and is not sendable, whatever `K`'s own
//!   sendability.
//! cites: 4, 4.9, 30.14, 6.9
//! expect: reject
//! diagnostic: not sendable
//! issue: 5597
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
    using var ch := new Channel<OwningDictionary<Key, Widget>>();
    using var tx := ch.NewSender();
    var d := new OwningDictionary<Key, Widget>();
    tx.Send(move d);
  end Demo;
end Program;
