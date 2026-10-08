//! case: s27_6_0008
//! doc: spec
//! section: 27.6
//! title: the same reference against a comparer-taking target selects the unconditioned overload
//! rule: The overload is selected against the target first; the condition is
//!   read on the overload selected and takes no part in the selection ... the
//!   comparer-taking overload, which the same reference against a
//!   comparer-taking target selects and no row conditions
//! cites: 27.6, 14.2
//! expect: accept
module Conf;

public record Widget(Weight: int);

public class Program
  public static method Run(xs: Array<Widget>)
    var s: ((Widget, Widget) -> int) -> () := xs.Sort;
    s(lambda(a, b) => a.Weight - b.Weight);
  end Run;
end Program;
