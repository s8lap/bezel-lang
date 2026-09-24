//! case: s35_5_0005
//! doc: spec
//! section: 35.5
//! title: the move marker on a borrowed scrutinee (`this`) is rejected
//! rule: What does not trigger an owning match: an unmarked named-binding scrutinee; a
//!   borrowed scrutinee of any provenance (field read, parameter, this).
//! expect: reject
//! diagnostic: borrowed
//! issue: 4763
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public choice Outcome
  Fetched(owned w: Widget),
  Failed(status: int);

  public method Peek()
    case move this of      // ERROR: `this` is a borrow of the receiver value
      Fetched(_) => Console.WriteLine("ok"),
      Failed(_)  => Console.WriteLine("failed")
    end case;
  end Peek;
end Outcome;
