//! case: s28_9_0032
//! doc: spec
//! section: 28.9
//! title: an undischarged owned parameter of trivial-dispose type draws no must-use warning
//! rule: The exemption covers **every holder form the warning attaches to**:
//!   the must-use warning is not emitted at the implicit-drop site of an
//!   undischarged `owned` parameter — a method's, an iterator's, or an async
//!   method's — whose type is trivial-dispose; the auto-drop itself is
//!   unconditional at every one of them.
//! cites: 28.9, 28.3
//! expect: quiet
//! issue: 4400
module Conf;

public class Marker
  public constructor()
  end constructor;

  public method Tag(): string => "m";
end Marker;

public class Program
  public method Take(owned m: Marker): string
    return m.Tag();
  end Take;
end Program;
