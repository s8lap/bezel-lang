//! case: s22_3_0014
//! doc: spec
//! section: 22.3
//! title: out owned is the only modifier compound; others are mutually exclusive
//! rule: The combination `out owned` is the only modifier compound; the
//!   others are mutually exclusive.
//! cites: 22.3
//! expect: reject
//! diagnostic: mutually exclusive
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Registry
  public constructor()
  end constructor;

  public method Store(scoped owned item: Widget)
  end Store;
end Registry;
