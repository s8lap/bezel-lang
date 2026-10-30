//! case: s15_3_0069
//! doc: spec
//! section: 15.3
//! title: reading an owning-choice local after a local binding moved it through ! is rejected
//! rule: After the move, `x` is in moved-from state and may not be read until reassigned.
//! cites: 17.8, 35.3, 28.3
//! expect: reject
//! diagnostic: moved-from
module Conf;

public class Res
  public constructor() end constructor;
  destructor end destructor;
end Res;

public choice OC
  Text(owned r: Res),
  Empty;
end OC;

public class Program
  public static method T()
    var o: OC? := OC.Text(new Res());
    var x := o!;
    var n := (case o of Text(_) => 1, _ => 2);
  end T;

  public static method Main(): int
    T();
    return 0;
  end Main;
end Program;
