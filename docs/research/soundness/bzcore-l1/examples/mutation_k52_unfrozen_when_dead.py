"""Example of a user-defined rule change for `bzcore.py check --define FILE`.

A definition file sets any of:
  NAME            a title for the report
  RULES_OVERRIDE  a dict of RULES knobs to flip (see bzcore.RULES)
  HOOKS_OVERRIDE  a dict of predicates that replace a kernel clause's text:
                    frozen(ids, G, live) -> bool    K5's criterion; `ids` are the consumed
                                                    binding's current value ids, `G` the state
                                                    map, `live` the live-out bindings
                    meet(a, b) -> state             K7's cell for two K1 states (raise
                                                    bzcore.Reject(rule) to reject the merge)

This one asks a sharper question than deleting K5.2: what if drop-on-overwrite were frozen
only when the surviving borrow is itself read through *after* the overwrite through a
re-borrow chain of length one? It is deliberately silly — the point is that a predicate
over (ids, G, live) is the whole interface, so any clause rewording can be tried in a few lines.
"""
NAME = 'example: K5 freeze ignores borrows that are not direct re-borrows of the binding'

RULES_OVERRIDE = {}

def frozen(ids, G, live):
    # the kernel's text: any live borrow whose source set meets the consumed value's ids freezes.
    # the variant: only a borrow whose source set is EXACTLY the consumed value's ids freezes
    # (a borrow carrying a union of sources from a join no longer counts).
    for y in live:
        st = G.get(y)
        if st and st[0] == 'brw' and st[1] == ids:
            return True
    return False

HOOKS_OVERRIDE = {'frozen': frozen}
