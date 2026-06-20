// §28.8.1 / #825: nullable-detach pattern — field declared as Logfile? so that
// null is a valid post-move restoration.  Both `move _log` and `_log := null`
// use the unqualified (bare) form; the restoration satisfies the field-restoration
// rule.
// private owned _log: Logfile?;
method DetachLog(): owned Logfile?
  var d := move _log;   // _log is now in moved-from state
  _log := null;         // restore the field — required by §28.8.1
  return d;
end DetachLog;
