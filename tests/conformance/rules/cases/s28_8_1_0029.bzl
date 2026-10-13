//! case: s28_8_1_0029
//! doc: spec
//! section: 28.8.1
//! title: a replacing move of a static readonly owning inline array's slot is rejected
//! rule: Static fields are not eligible ... static owned storage — a `static
//!   owned` field, and the slots of a `static readonly` owning inline array
//!   ... — admits no `move`, no slot store and no explicit disposal: it is
//!   the always-live source ... so it is neither taken nor replaced
//! cites: 28.8.1, 28.3, 22.12, 28.11
//! expect: reject
//! diagnostic: K9.4
module Conf;

public class Res
  public constructor()
  end constructor;
end Res;

public class Registry
  public static readonly Pool: [2 owned]Res?;

  public static method Swap(): owned Res?
    return move Registry.Pool[0] with new Res();
  end Swap;
end Registry;
