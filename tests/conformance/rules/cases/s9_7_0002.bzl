//! case: s9_7_0002
//! doc: spec
//! section: 9.7
//! title: a record cannot declare a destructor
//! rule: Records cannot have destructors, owned fields, or implement
//!   IDisposable.
//! expect: reject
//! diagnostic: destructor
module Conf;

public record Handle
  public readonly property Id: int;

  public constructor(id: int)
    Id := id;
  end constructor;

  destructor
  end destructor;
end Handle;
