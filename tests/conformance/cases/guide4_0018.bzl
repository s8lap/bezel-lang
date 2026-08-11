private static _warm: bool := false;    // admitted, but WARNED (§22.12 form (iii)):
                                        //   another domain's read may be stale forever —
                                        //   form (iii) stores carry no publication promise
private static owned readonly _ready: Atomic<bool>
    := new Atomic<bool>(false);         // warning-free: the synchronized spelling
