//! case: s28_3_0936
//! doc: spec
//! section: 28.3
//! title: a store into a purely local collection of a plain parameter reports nothing
//! rule: The trust-admission report (§28.3 *Diagnostics*) names a parameter-,
//!   receiver- or field-path-sourced element at its store, and an element
//!   sourced in the body at a transfer of its source that the store reaches.
//!   A store into a collection that is a local of the body and does not escape
//!   it, of a parameter that is not `owned` or of `this` — the binding itself,
//!   not a value read through it — is not reported: the scope comparison bounds
//!   the element (K2.3), and the body holds no obligation for the object such a
//!   binding names, so no kill in it can transfer that object.
//! cites: 28.3, 28.11
//! expect: quiet
//! flags: --report-trust-admissions
//! issue: 7468
module Conf;

public class Item
  public constructor()
  end constructor;
end Item;

public class Program
  public static method Use(xs: Array<Item>): int => xs.Count;

  public static method Register(item: Item): int
    using var tmp := new Array<Item>();
    tmp.Add(item);
    return Use(tmp);
  end Register;
end Program;
