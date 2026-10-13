//! case: s28_8_1_0002
//! doc: spec
//! section: 28.8.1
//! title: a terminal bare move of a non-nullable field is rejected even under an owned return
//! rule: Where the location's declared type is nullable ... the `with`
//!   clause may be omitted: `move x.f` stores `null`. Where it is not, the
//!   clause is required; its absence is a compile error naming the two
//!   remedies — `move x.f with fresh`, or a nullable declaration
//! cites: 28.8.1
//! expect: reject
//! diagnostic: not reassigned
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Holder
  private owned _resource: Widget;

  public constructor()
    _resource := new Widget();
  end constructor;

  public method Take(): owned Widget
    return move _resource;
  end Take;
end Holder;
