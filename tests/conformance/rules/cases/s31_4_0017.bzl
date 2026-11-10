//! case: s31_4_0017
//! doc: spec
//! section: 31.4
//! title: a record implementing an interface that extends IFailure is rejected
//! rule: An interface may extend `IFailure`; both restrictions then bind its
//!   implementers. … Only an `Exception`-derived class: a record, an enum, a
//!   choice, or a class outside the `Exception` family that lists `IFailure`,
//!   or an interface extending it, is a compile error at the declaration.
//! cites: 31.4
//! expect: reject
//! diagnostic: exception-derived class
//! issue: 8416
module Conf;

public interface ITransient: IFailure
end ITransient;

public record Timeout(Seconds: int): ITransient;
