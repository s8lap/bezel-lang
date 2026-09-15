//! case: s2_1_0001
//! doc: spec
//! section: 2.1
//! title: a dotted module name is one unnested identifier
//! rule: A module name is a single identifier or a sequence of identifiers
//!   separated by dots. The dotted form is a single name; it does not
//!   establish nesting.
//! expect: accept
module App.Billing;

public class Ledger
end Ledger;
