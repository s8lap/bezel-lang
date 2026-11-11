//! case: s28_3_0938
//! doc: spec
//! section: 28.3
//! title: a static method reference stored into a borrowing collection reports nothing
//! rule: Method references work for static methods, instance methods (the
//!   instance is captured at formation) (§27.6), so a static method reference
//!   is a capture-free function value: a function value read from a field, a
//!   collection, a property or an indexer, or narrowed from `Object`, or from
//!   a value whose type is a bare type parameter, carries the empty set
//!   (§28.3 *Provenance* clause 5). Storing a class-typed borrow into a
//!   borrowing collection follows the same shape as §28.3's borrowed-field
//!   rule: parameter-sourced, receiver-sourced and field-sourced borrows are
//!   admitted, and the residual is a [K4.3]-recorded trust clause. A
//!   capture-free function value is no borrow: no element is admitted and no
//!   residual arises, so the trust-admission report (§28.3 *Diagnostics*) names
//!   nothing at its store.
//! cites: 28.3, 27.6, 28.11
//! expect: quiet
//! flags: --report-trust-admissions
//! issue: 7468
module Conf;

public class Demo
  public static method Twice(x: int): int => x * 2;
end Demo;

public class Program
  private owned _fns: Array<(int) -> int>;

  public constructor()
    _fns := new Array<(int) -> int>();
  end constructor;

  public method Note(): int
    _fns.Add(Demo.Twice);
    return _fns.Count;
  end Note;
end Program;
