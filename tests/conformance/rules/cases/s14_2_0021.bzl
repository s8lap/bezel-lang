//! case: s14_2_0021
//! doc: spec
//! section: 14.2
//! title: an interface implementation comparing its own unconstrained type parameter is rejected at its body
//! rule: Every body of a dispatched generic method ... is checked under its
//!   constraint set (above) as any generic body is, and a requirement of its
//!   own type parameter that the set does not establish is a **compile error
//!   at that body** ... The diagnostic names the requirement and the
//!   remedies: a constraint of this section on the **declaring** interface or
//!   base method, which every implementation then inherits (`where T:
//!   hashable` for an equality use ...).
//! cites: 14.2, 9.2, 3.6
//! expect: reject
//! diagnostic: hashable
//! issue: 5639
module Conf;

public interface IFinder
  method Find<T>(xs: Array<T>, x: T): int;
end IFinder;

public class LinearFinder: IFinder
  public constructor()
  end constructor;

  public method Find<T>(xs: Array<T>, x: T): int
    for var i in 0..<xs.Count do
      if xs[i] = x then
        return i;
      end if;
    end for;
    return -1;
  end Find;
end LinearFinder;
