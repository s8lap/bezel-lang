//! case: s11_2_0162
//! doc: spec
//! section: 11.2
//! title: an override's own method type parameter renamed by position is admitted
//! rule: **Generic members.** A member's own type parameters take part in the match by number and position, not by name.
//! cites: 11.2, 12.1
//! expect: accept
module Conf;

public class B
  public constructor() end constructor;
  public virtual method Echo<T>(x: T): string => "base";
end B;

public class D: B
  public constructor() end constructor;
  public override method Echo<U>(x: U): string => "derived";
end D;
