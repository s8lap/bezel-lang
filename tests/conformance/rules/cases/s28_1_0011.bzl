//! case: s28_1_0011
//! doc: spec
//! section: 28.1
//! title: owned on an Object parameter is admitted
//! rule: `Object` is such a class type (§8.1), here and at an `owned` field
//!   (§28.5): the marker transfers whatever value the position holds, and a
//!   runtime-managed value there — a boxed value, a `string`, a function
//!   value — carries no obligation, the discharge points disposing nothing
//!   for it (§8.7).
//! cites: 28.1, 8.1, 17.13
//! expect: accept
//! issue: 7921
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Sink
  public static method Take(owned o: Object): string
    var s := o.ToString();
    _ := o;
    return s;
  end Take;

  public static method Use(): string => Take(new Widget());
end Sink;
