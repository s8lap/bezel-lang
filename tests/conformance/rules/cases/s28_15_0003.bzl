//! case: s28_15_0003
//! doc: spec
//! section: 28.15
//! title: outside the report mode, a transfer of a carried source compiles quietly in this revision
//! rule: In this revision the rules of this section define the classification the
//!   trust-admission report prints (§28.3, clause 4) and change no verdict: a
//!   program is accepted or rejected as the rest of this specification decides
//!   (§28.15 *Staging*); the report "is informative ... outside the mode it is
//!   not emitted" (§28.3.12)
//! cites: 28.3
//! expect: quiet
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method N(): int => 1;
end Widget;

public class Tag
  private _w: Widget? := null;

  public constructor(w: Widget)
  end constructor;

  public method N(): int => 2;
end Tag;

public class Program
  public static method Take(owned w: Widget)
  end Take;

  public static method Run(): int
    var a := new Widget();
    var t := new Tag(a);
    Take(move a);
    return t.N();
  end Run;
end Program;
