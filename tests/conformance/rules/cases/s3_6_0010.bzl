//! case: s3_6_0010
//! doc: spec
//! section: 3.6
//! title: the nullable form of a non-hashable record is not hashable
//! rule: hashable ... every type except (a) a record that is neither
//!   auto-hashable nor declares user equality, ... (c) a nullable type `T?`,
//!   or an inline array of any §15.1 form whose element type is `T` or `T?`,
//!   where `T` is itself not hashable
//! cites: 3.6, 14.2
//! expect: reject
//! diagnostic: hashable
module Conf;

public record Odd
  private _x: int;

  public constructor(x: int)
    _x := x;
  end constructor;
end Odd;

public class Bag<T> where T: hashable
  public constructor()
  end constructor;
end Bag;

public class Holder
  public static method Make(): int
    using var b := new Bag<Odd?>();
    return 1;
  end Make;
end Holder;
