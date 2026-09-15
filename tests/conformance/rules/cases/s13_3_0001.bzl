//! case: s13_3_0001
//! doc: spec
//! section: 13.3
//! title: a field with no visibility keyword is rejected
//! rule: Every class member must declare its visibility explicitly, except
//!   the four exempted kinds. On every other member, omitting the visibility
//!   keyword is a compile error.
//! cites: 13.3, C.2
//! expect: reject
//! diagnostic: visibility
module Conf;

public class Box
  _value: int;

  public constructor(value: int)
    _value := value;
  end constructor;
end Box;
