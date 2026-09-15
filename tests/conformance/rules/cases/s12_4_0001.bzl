//! case: s12_4_0001
//! doc: spec
//! section: 12.4
//! title: inheriting from a sealed class is rejected
//! rule: sealed on a class prevents inheritance from that class.
//! cites: 12.4
//! expect: reject
//! diagnostic: sealed
module Conf;

public sealed class Bulldog
  public constructor()
  end constructor;
end Bulldog;

public class SuperBulldog: Bulldog
  public constructor()
  end constructor;
end SuperBulldog;
