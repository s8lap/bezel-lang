method DetachLog(): owned Logfile
  return move _log;     // _log is now in moved-from state
end DetachLog;
