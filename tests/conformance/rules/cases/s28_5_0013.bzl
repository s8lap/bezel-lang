//! case: s28_5_0013
//! doc: spec
//! section: 28.5
//! title: a plain inline-array field (no owned slot marker) on a record is admitted
//! rule: Records cannot have inline-array fields whose slots carry the `owned`
//!   marker; an inline-array field without that marker holds per-slot borrows
//!   or value-typed copies, which a record may hold.
//! cites: 9.7, 28.5
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

record Bag
  private slots: [4]Widget?;

  public constructor()
  end constructor;
end Bag;
