//! case: l4_5_0001
//! doc: stdlib
//! section: 4.5
//! title: OwningArray<T>.Add transfers ownership; a fresh producer is warning-free
//! rule: OwningArray<T> owns each element; Add takes ownership. On the
//!   owning family a fresh producer is idiomatic and warning-free:
//!   pool.Add(new Connection("a"));
//! cites: 4.5, 28.11
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Fill()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget());
  end Fill;
end Program;
