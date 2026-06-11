method Rotate()
  using var old := move _log;          // moved out
  _log := new Logfile(NextName());     // restored before method exits — required
  old.Flush();
end Rotate;                            // old disposed here
