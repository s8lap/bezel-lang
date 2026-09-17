//! case: s35_1_0002
//! doc: spec
//! section: 35.1
//! title: a case name colliding with a member name is rejected
//! rule: Case names must be distinct within the choice; a case name may not collide
//!   with any member name of the choice (the §11.3 member rules apply with cases
//!   counted as members).
//! expect: reject
//! diagnostic: collide
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public choice Outcome
  Ready(owned w: Widget),
  Empty;

  public method Ready(): bool => true;   // ERROR: collides with the case name `Ready`
end Outcome;
