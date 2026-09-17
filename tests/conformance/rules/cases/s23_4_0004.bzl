//! case: s23_4_0004
//! doc: spec
//! section: 23.4
//! title: an indexer may take several parameters
//! rule: An indexer is a property whose name is replaced by `[parameters]`;
//!   the list may hold more than one parameter.
//! cites: 23.4
//! expect: accept
module Conf;

public class Grid
  private owned _cells: Array<double> := new Array<double>();
  private _width: int;

  public constructor(width: int)
    _width := width;
  end constructor;

  public property [row: int, col: int]: double => _cells[row * _width + col];
end Grid;
