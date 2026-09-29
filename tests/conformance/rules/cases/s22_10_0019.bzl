//! case: s22_10_0019
//! doc: spec
//! section: 22.10
//! title: the holder of a scoped primary-constructor borrow is disposed before its source
//! rule: At the call site the argument is admitted under §28.14's
//!   bounding-object forms exactly as at a declared constructor: the holder
//!   is a `using var` local, the source is declared before the holder, and
//!   the source is frozen until the holder's scope exits. With declared-before
//!   admission, the freeze, and reverse-declaration-order disposal (§21.6,
//!   §28.10), the source provably outlives the bounding object including its
//!   destructor.
//! cites: 22.10, 28.14, 21.6, 28.10, 28.4
//! expect: run
//! output: box
//!   source
//! issue: 4601
module Conf;

public class Source
  public constructor()
  end constructor;

  public method Ping()
  end Ping;

  destructor
    Console.WriteLine("source");
  end destructor;
end Source;

public class Box(scoped src: Source)
  public method Touch()
    src.Ping();
  end Touch;

  destructor
    Console.WriteLine("box");
  end destructor;
end Box;

public class Program
  public static method Main()
    using var s := new Source();
    using var b := new Box(scoped s);
    b.Touch();
  end Main;
end Program;
