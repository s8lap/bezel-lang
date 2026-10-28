module Conf;
// The replacing move `move _f with new Q()` at a non-nullable field, as the spec writes it (§28.8.1). Measure r35's parse (the #5923 note).
public class Q
  public constructor()
  end constructor;
end Q;
public class R
  private owned _f: Q;
  public constructor()
    _f := new Q();
  end constructor;
  public method Take(): owned Q
    return move _f with new Q();
  end Take;
end R;
