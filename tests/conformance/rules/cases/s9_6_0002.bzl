//! case: s9_6_0002
//! doc: spec
//! section: 9.6
//! title: a record cannot derive from another record
//! rule: A record cannot derive from another record or from a class (other than
//!   the implicit Object root).
//! expect: reject
//! diagnostic: derive
module Conf;

public record Base2D(X: int, Y: int);

public record Point3D(Z: int): Base2D;
