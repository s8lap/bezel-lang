//! case: s28_3_8_7202
//! doc: spec
//! section: 28.3.8
//! title: a capture-bearing function value converted to an Object-typed
//!   local is rejected
//! rule: At every set-erasing position a function value with a non-empty
//!   source set is rejected, and a capture-free one is admitted
//! cites: 28.3
//! expect: reject
//! issue: 7259
//! diagnostic: set-erasing
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public class Client
  public static method Run(p: Item)
    var f: () -> int := lambda() => p.Size();
    var o: Object := f;
  end Run;
end Client;
