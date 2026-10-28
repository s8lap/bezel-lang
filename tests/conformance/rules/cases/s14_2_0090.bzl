//! case: s14_2_0090
//! doc: spec
//! section: 14.2
//! title: a record implementing the named interface satisfies where T: IFoo
//! rule: Language spec §14.2 (v0.345), the interface conformance rule 2: "T is a record that
//!   lists I (or any interface extending I) in its inheritance clause and declares matching
//!   members" — and the rule's closing sentence: "`where T: IFoo` admits exactly the
//!   substitutions for which T conforms to `IFoo` per the rule above". A record listing a user
//!   interface therefore satisfies `where T: IFoo`.
//! cites: 14.2
//! expect: accept
module Conf;

public interface IFoo
  method Ping(): int;
end IFoo;

public record R: IFoo
  public method Ping(): int => 7;
end R;

public class Box<T> where T: IFoo
  public constructor()
  end constructor;
end Box;

public class Holder
  private _b: Box<R>?;

  public constructor()
  end constructor;
end Holder;
