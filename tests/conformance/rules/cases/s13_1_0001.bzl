//! case: s13_1_0001
//! doc: spec
//! section: 13.1
//! title: a private member is not visible outside the declaring class
//! rule: `private` — Visible to the declaring class only.
//! cites: 13.1
//! expect: reject
//! diagnostic: private
module Conf;

public class Box
  private _value: int;

  public constructor(value: int)
    _value := value;
  end constructor;
end Box;

public class Reader
  public constructor()
  end constructor;

  public method Peek(b: Box): int => b._value;
end Reader;
