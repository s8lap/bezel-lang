var u := _user;
if u <> null then
  u.DoStuff();          // u is narrowed; safe regardless of _user changes
end;
