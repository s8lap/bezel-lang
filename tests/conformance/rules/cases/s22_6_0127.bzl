//! case: s22_6_0127
//! doc: spec
//! section: 22.6
//! title: a class's own `this` is not a string argument of int.TryParse
//! rule: If no candidate is applicable at all — no candidate is reachable: a conversion §5.1 does not admit as implicit
//! cites: 22.6, 5.1, 12.3, 9.1
//! expect: reject
//! diagnostic: overload
//! issue: 7304
module Conf;

public class Counter
  public method Go(): int
    var v: int := 7;
    var ok: bool := int.TryParse(this, out v);
    if ok then
      return 1;
    end;
    return v;
  end Go;
end Counter;
