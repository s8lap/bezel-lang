//! case: s35_8_0002
//! doc: spec
//! section: 35.8
//! title: converting an owning choice to an interface is a rejected boxing conversion
//! rule: Owning choices cannot be boxed. The conformance declaration remains legal, but
//!   the conversion of an owning value to an interface or Object type is rejected
//!   at the conversion site.
//! expect: reject
//! diagnostic: boxed
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public interface IMarker
  method Tag(): string;
end IMarker;

public choice Outcome: IMarker
  Fetched(owned w: Widget),
  Failed;

  public method Tag(): string => "outcome";
end Outcome;

public class Program
  public static method Main()
    using var o := Outcome.Fetched(new Widget());
    var m: IMarker := o;   // ERROR: converting an owning choice to an interface is a boxing conversion
  end Main;
end Program;
