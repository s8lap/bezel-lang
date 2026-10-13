//! case: s28_3_0167
//! doc: spec
//! section: 28.3
//! title: under the report mode, overwriting a collection-held local is not reported
//! rule: The claim reaches exactly the recorded clauses — not the flag-backstopped
//!   kills (the site-announced kills, K4.2, and a reassignment's
//!   drop-on-overwrite, K3.11, each runtime-checked under §28.2)
//! cites: 28.3, 28.11
//! expect: quiet
//! flags: --report-trust-admissions
//! issue: 4605
module Conf;

public class Item
  public constructor()
  end constructor;

  public method N(): int => 1;
end Item;

public class Program
  public static method Run(): int
    var item := new Item();
    var coll := new Array<Item>();
    coll.Add(item);
    item := new Item();
    return item.N();
  end Run;
end Program;
