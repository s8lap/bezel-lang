// Field declared nullable so that null is a valid post-move restoration.
// private owned _log: Logfile?;
method DetachLog(): owned Logfile?
  var d := move _log;   // _log is now in moved-from state
  _log := null;         // restore the field — required by §28.8.1
  return d;
end DetachLog;
