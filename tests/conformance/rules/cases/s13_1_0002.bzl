//! case: s13_1_0002
//! doc: spec
//! section: 13.1
//! title: a private member is visible within its own declaring class
//! rule: `private` — Visible to the declaring class only.
//! cites: 13.1
//! expect: accept
module Conf;

public class Box
  private _value: int;

  public constructor(value: int)
    _value := value;
  end constructor;

  public method Peek(): int => _value;
end Box;
