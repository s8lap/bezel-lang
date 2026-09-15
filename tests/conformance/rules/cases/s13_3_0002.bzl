//! case: s13_3_0002
//! doc: spec
//! section: 13.3
//! title: a field with an explicit visibility keyword is admitted
//! rule: Every class member must declare its visibility explicitly, except
//!   the four exempted kinds.
//! cites: 13.3, C.2
//! expect: accept
module Conf;

public class Box
  private _value: int;

  public constructor(value: int)
    _value := value;
  end constructor;
end Box;
