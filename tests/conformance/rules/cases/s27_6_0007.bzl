//! case: s27_6_0007
//! doc: spec
//! section: 27.6
//! title: a method reference to Sort at an unordered element type is rejected at the formation
//! rule: A method reference, bound or unbound, or a property reference formed
//!   over a **conditionally-admitted member** (§14.2 ...) is a use of the
//!   member: at a receiver instantiation that does not satisfy the member's
//!   condition, the formation is rejected **at the formation site** with the
//!   condition's diagnostic and remedy, exactly as the call would be ...
//!   So `var s: () -> () := xs.Sort;` at `xs: Array<Widget>`, for a `Widget`
//!   conforming to no `IComparable<Widget>`, is rejected naming the
//!   comparer-taking overload
//! cites: 27.6, 14.2
//! expect: reject
//! diagnostic: comparer
//! issue: 5656
module Conf;

public record Widget(Weight: int);

public class Program
  public static method Run(xs: Array<Widget>)
    var s: () -> () := xs.Sort;
    s();
  end Run;
end Program;
