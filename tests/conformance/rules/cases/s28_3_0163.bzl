//! case: s28_3_0163
//! doc: spec
//! section: 28.3
//! title: outside the report mode, transferring a local held in a local collection is silent
//! rule: The report is informative: it changes no verdict and no generated code, and
//!   outside the mode it is not emitted
//! cites: 28.3, 28.11
//! expect: quiet
module Conf;

public class Item
  public constructor()
  end constructor;

  public method N(): int => 1;
end Item;

public class Program
  public static method Take(owned i: Item)
  end Take;

  public static method Run()
    var item := new Item();
    var coll := new Array<Item>();
    coll.Add(item);
    Take(move item);
  end Run;
end Program;
