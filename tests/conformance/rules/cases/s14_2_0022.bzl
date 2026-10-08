//! case: s14_2_0022
//! doc: spec
//! section: 14.2
//! title: the same equality implementation is admitted under where T hashable on the declaring interface
//! rule: The diagnostic names the requirement and the remedies: a constraint
//!   of this section on the **declaring** interface or base method, which
//!   every implementation then inherits (`where T: hashable` for an equality
//!   use ...).
//! cites: 14.2, 9.2
//! expect: accept
module Conf;

public interface IFinder
  method Find<T>(xs: Array<T>, x: T): int where T: hashable;
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
