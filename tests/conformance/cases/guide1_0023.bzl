var committed := false;
try
  Register(id);
  DoWork();
  committed := true;
finally
  if not committed then Unregister(id); end;
end;
