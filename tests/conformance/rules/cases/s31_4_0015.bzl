//! case: s31_4_0015
//! doc: spec
//! section: 31.4
//! title: a class outside the Exception family listing IFailure is rejected
//! rule: Only an `Exception`-derived class: a record, an enum, a choice, or a class
//!   outside the `Exception` family that lists `IFailure`, or an interface
//!   extending it, is a compile error at the declaration.
//! cites: 31.4
//! expect: reject
//! diagnostic: exception-derived class
//! issue: 8416
module Conf;

public class Reading: IFailure
end Reading;
