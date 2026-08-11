public static class Sessions
  private static owned readonly _map: Locked<OwningDictionary<string, Session>>
      := new Locked<OwningDictionary<string, Session>>(new OwningDictionary<string, Session>());

  public static async method Renew(id: string): Task
    using var g := await _map.LockAsync();           // guard: owned, Pattern B award
    g.Value[id] := Session.Renewed(g.Value[id]);     // owning indexer-set disposes the old one
  end Renew;                                         // guard disposed ⇒ lock released
end Sessions;
