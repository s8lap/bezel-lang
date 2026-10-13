//! case: s28_9_0036
//! doc: spec
//! section: 28.9
//! title: the same class with a plain value field is trivial-dispose and its local is silent
//! rule: User-defined classes with no destructor, no owned fields of
//!   non-trivial-dispose type, and no fields of move-only value type
//!   (owning-inline-array or owning-choice) are automatically trivial-dispose.
//! cites: 28.9, 28.3
//! expect: quiet
module Conf;

public class Host
  private _n: int := 1;
  public property Count: int => _n;
end Host;

public class Program
  public static method Size(): int
    var h := new Host();
    return h.Count;
  end Size;
end Program;
