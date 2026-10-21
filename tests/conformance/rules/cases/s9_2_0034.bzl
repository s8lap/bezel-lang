//! case: s9_2_0034
//! doc: spec
//! section: 9.2
//! title: = between two unrelated record types compares unequal through the Object slot
//! rule: "`a = b` invokes `a.Equals(b)`, with a null-safe wrapper" ([§8.4]), and "`<>` is the negation."
//!   ([§8.4]). The argument `q` is a `Q`, not a `P`, so the call reaches the Object slot, where "the
//!   member runs only on a `T` payload and any other argument compares unequal without the body running"
//!   ([§9.2]). So `p = q` is False and `p <> q` is True.
//! cites: 8.4, 9.2
//! expect: run
//! output: False
//!   True
//! issue: 6686
module Conf;
record P(X: int) end P;
record Q(X: int) end Q;
public class Program
  public static method Main(): int
    var p := new P(1); var q := new Q(1);
    Console.WriteLine(p = q); Console.WriteLine(p <> q); return 0;
  end Main;
end Program;
