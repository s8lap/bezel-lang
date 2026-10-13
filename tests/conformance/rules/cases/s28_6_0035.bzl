//! case: s28_6_0035
//! doc: spec
//! section: 28.6
//! title: an early-disposed instance owned field is disposed once and the cascade is the no-op
//! rule: The call is an **early release point**, not a discharge of the slot's
//!   obligation: the slot keeps its reference and its K9 obligation, and the
//!   containing object's teardown still runs the cascade over it (§28.4 step
//!   2), where the second call is §28.2's no-op and the block is then
//!   reclaimed
//! cites: 28.6, 28.2, 28.4
//! expect: run
//! output: closed
//!   released early
//!   done
module Conf;

public class Resource
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Resource;

public class Holder
  private owned _m: Resource := new Resource();

  public method Close()
    _m.Dispose();
  end Close;
end Holder;

public class Program
  public static method Main()
    do
      using var h := new Holder();
      h.Close();
      Console.WriteLine("released early");
    end;
    Console.WriteLine("done");
  end Main;
end Program;
