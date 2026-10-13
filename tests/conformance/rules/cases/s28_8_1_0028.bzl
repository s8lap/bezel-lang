//! case: s28_8_1_0028
//! doc: spec
//! section: 28.8.1
//! title: a destructor's bare take of a non-nullable owning slot is rejected
//! rule: The exemption reaches only the fields of the object the destructor
//!   tears down: a field of another object, or an owning slot, takes the
//!   replacing form's rules in a destructor body as elsewhere.
//! cites: 28.8.1, 28.11
//! expect: reject
//! diagnostic: with
//! issue: 5923
module Conf;

public class Res
  public constructor()
  end constructor;
end Res;

public class Holder
  private owned _xs: OwningArray<Res> := new OwningArray<Res>();

  destructor
    using var t := move _xs[0];
  end destructor;
end Holder;
