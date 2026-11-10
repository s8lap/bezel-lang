//! case: s28_15_0010
//! doc: spec
//! section: 28.15
//! title: under the report mode, the same instance returned at its own carrier class reports nothing
//! rule: An owned value whose source set is not empty may not reach an **owning
//!   position whose declared type is not a carrier type**. ... an `owned` return
//!   slot ... — §28.15 *Owned erasure*; an interface is borrow-bearing if it
//!   declares or inherits a store member (*The property*). An `owned` return of
//!   an instance is admitted where every source is one the callers re-establish:
//!   the receiver's contribution, a parameter's, or static storage (*The bound*,
//!   *Returns*).
//! cites: 28.3
//! expect: quiet
//! flags: --report-trust-admissions
//! issue: 8422
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method N(): int => 1;
end Widget;

public interface IShow
  method N(): int;
end IShow;

public class Tag: IShow
  private _w: Widget? := null;

  public constructor(w: Widget)
  end constructor;

  public method N(): int => 2;
end Tag;

public class Program
  public static method Make(w: Widget): owned Tag => new Tag(w);
end Program;
