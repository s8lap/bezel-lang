//! case: s11_2_0002
//! doc: spec
//! section: 11.2
//! title: an interface member may not carry a visibility keyword
//! rule: Interface members are implicitly public, have no body, and end with `;`.
//! cites: 11.2
//! expect: reject
//! diagnostic: visibility
module Conf;

public interface IBankAccount
  public method Deposit(amount: decimal);
end IBankAccount;
