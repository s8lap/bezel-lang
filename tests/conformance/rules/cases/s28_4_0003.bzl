//! case: s28_4_0003
//! doc: spec
//! section: 28.4
//! title: Dispose cannot be overridden
//! rule: `Dispose()` cannot be overridden; the customization point is `destructor`.
//! expect: reject
//! diagnostic: override
module Conf;

public class Widget
  public override method Dispose()
  end Dispose;
end Widget;
