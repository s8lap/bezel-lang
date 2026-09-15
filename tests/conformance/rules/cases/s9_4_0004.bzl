//! case: s9_4_0004
//! doc: spec
//! section: 9.4
//! title: constructor bodies are not receiver positions
//! rule: Constructor bodies are not receiver positions; a record constructor
//!   initializes the value under construction, and Width := width; is section
//!   9.2's own canonical body-record initialization form.
//! cites: 9.2, 22.8
//! expect: accept
module Conf;

public record Rectangle
  public readonly property Width: decimal;

  public constructor(width: decimal)
    Width := width;
  end constructor;
end Rectangle;
