if _field <> null then
  Use(_field);                     // ERROR: mutable fields and properties NEVER narrow
end;
