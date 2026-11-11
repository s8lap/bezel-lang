//! case: s28_3_0937
//! doc: spec
//! section: 28.3
//! title: an enum constant stored into a borrowing collection reports nothing
//! rule: Storing a class-typed borrow into a borrowing collection follows the
//!   same shape as §28.3's borrowed-field rule: parameter-sourced (a parameter
//!   that is not `owned`), receiver-sourced, and field-sourced borrows are
//!   admitted; the residual is a [K4.3]-recorded trust clause: whether each
//!   admitted element's source actually outlives the collection's use of it is
//!   the programmer's obligation. The trust-admission report (§28.3
//!   *Diagnostics*) names a parameter-, receiver- or field-path-sourced element
//!   at its store. An enum constant is a VALUE, not a borrow: it is not
//!   parameter-, receiver- or field-sourced, so no element is admitted and no
//!   residual arises — the report names nothing at its store.
//! cites: 28.3, 28.11
//! expect: quiet
//! flags: --report-trust-admissions
//! issue: 7468
module Conf;

enum E
  A, B;
end E;

public class Program
  private owned _seen: Set<E>;

  public constructor()
    _seen := new Set<E>();
  end constructor;

  public method Note(): int
    _seen.Add(E.A);
    return _seen.Count;
  end Note;
end Program;
