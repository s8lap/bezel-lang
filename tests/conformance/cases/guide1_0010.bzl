// A non-nullable owned field this time: private owned _log: Logfile;
method Rotate()
  var fresh := new Logfile(NextName());  // build the replacement first — it may throw
  using var old := move _log;            // then take — nothing throwable follows
  _log := fresh;                         // restore — a store into a moved-from slot cannot throw
  old.Flush();
end Rotate;                              // old disposed here
