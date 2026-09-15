//! case: s22_2_0001
//! doc: spec
//! section: 22.2
//! title: an expression body replaces the block when the body is one expression
//! rule: When the body is a single expression, `=> expression` replaces the
//!   block.
//! cites: 22.2
//! expect: accept
module Conf;

public class Rectangle
  private _width: decimal;
  private _height: decimal;

  public constructor(width: decimal, height: decimal)
    _width := width;
    _height := height;
  end constructor;

  public method Area(): decimal => _width * _height;
end Rectangle;
