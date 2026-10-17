//! case: s12_1_0075
//! doc: spec
//! section: 12.1
//! title: an override whose parameter type differs from the alias-expanded base parameter is rejected
//! rule: Visibility, return type, parameter types, and parameter modifiers must match the base member exactly
//! cites: 12.1, 27.8
//! notes: CONTROL ROW, not a defect pin. It rejects for the right reason
//!   (`parameter types` is the intended needle and the file parses), and it
//!   rejects on the fixed tree AND on a #6321-revert leg, both measured with
//!   provably fresh binaries -- so it pins neither fix. (The #6321 body
//!   concedes these shapes "pass with and without the fix".) It is a
//!   regression guard on §12.1's override-signature check, not harvested
//!   coverage for #6321/#6323. No `issue:` -- the column records a tracker a
//!   case PINS, and this row pins neither.
//! expect: reject
//! diagnostic: parameter types
module Conf;

type Id<T> := T;

public class B<X>
  public constructor() end constructor;
  public virtual method M(x: Id<X>): string => "base";
end B;

public class E<Q>: B<Q>
  public constructor() end constructor;
  public override method M(x: string): string => "derived";
end E;

public class M
  public static method Main(): int
    return 0;
  end Main;
end M;
