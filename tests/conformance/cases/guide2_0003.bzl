var local := _field;               // copy
if local <> null then
  Use(local);                      // narrowed; safe
end;
