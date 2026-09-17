//! case: s35_4_0003
//! doc: spec
//! section: 35.4
//! title: an owning choice that holds borrows may not be a class field
//! rule: An owning choice that holds borrows may not be a class field: it is the only
//!   move-only kind that can hold borrows, and the store would let the borrows
//!   outlive their sources.
//! expect: reject
//! diagnostic: holds borrows
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Logger
  public constructor()
  end constructor;
end Logger;

public choice LoadOutcome
  Loaded(owned resource: Widget, log: Logger),
  Failed;
end LoadOutcome;

public class Holder
  private owned _outcome: LoadOutcome;   // ERROR: LoadOutcome is owning and holds a borrow (`log`)

  public constructor(owned outcome: LoadOutcome)
    _outcome := outcome;
  end constructor;
end Holder;
