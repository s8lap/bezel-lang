"""Rule-family mapping for the model-vs-compiler differential (0047 §5.3, Q3: verdict identity is
compared at rule-family granularity, never message text).

MODEL_FAMILY maps the model's `reject:<rule>` names and warning names onto families.
COMPILER_FAMILY is an ordered list of (regex over the diagnostic message, family); the first match
wins.  A message no row matches is reported as `UNMAPPED:<normalised text>` so the table can grow.
IGNORED_WARNINGS are compiler warnings the model does not express (the K3.14 must-use lint, and
the §22.12 static-storage declaration-site warnings of the printer's preamble).
"""
import re

MODEL_FAMILY = {
    # rejections
    'K5.1-frozen-transfer': 'freeze',
    'K5.2-frozen-overwrite': 'freeze',
    'K5.3c-frozen-dispose': 'freeze',
    'K7.3-mixed-merge': 'mixed-join',
    'state-match': 'state-match',
    'K1.3-read-of-moved': 'read-of-moved',
    'K2.3-borrow-outlives-source': 'borrow-escape',
    '§28.10-unmarked-owned-into-using-var': 'using-unmarked',
    'A5-move-out-of-using-var': 'using-pin',
    'A5-reassign-using-var': 'using-pin',
    'K8.4-temporary-escape': 'temporary-escape',
    '§28.6-dispose-of-borrow': 'dispose-of-borrow',
    'K3-consume-of-non-owned': 'consume-of-borrow',
    'K5.3b-same-variable-dominated-use': 'use-after-dispose-error',
    'K5.3b-use-of-killed-borrow': 'use-after-dispose-error',
    'loop-fixpoint': 'other',
    # warnings
    '§28.2-use-after-dispose': 'use-after-dispose',
    '§28.2-second-dispose': 'use-after-dispose',
    'K5.3a-use-of-killed-borrow': 'use-after-dispose',
}

COMPILER_FAMILY = [
    # --- rejections
    (r'cannot consume `\w+` here: the borrow held by', 'freeze'),
    (r'consume-site freeze', 'freeze'),
    (r'cannot (reassign|overwrite) .* borrow .* live', 'freeze'),
    (r'branch ownership conflict|mixed (owned/borrowed|ownership) (merge|state)|owned on one (arm|path).*borrow', 'mixed-join'),
    (r'ownership state.*(merge|join)|merge.*ownership', 'mixed-join'),
    (r'conditional-discharge join|already moved out|moved-from|was moved|maybe-consumed|may have (already )?been (moved|disposed|consumed)', 'read-of-moved'),
    (r'(use|read) of .*(unassigned|uninitialized)|definite(ly)? assign', 'read-of-moved'),
    (r'cannot call `Dispose\(\)` on', 'dispose-of-borrow'),
    (r'the thrown value must be an OWNED exception', 'consume-of-borrow'),
    (r'would outlive its source|outlives? (its|the) source|borrow .* escape|lives in an enclosing|lifetime is the catch arm', 'borrow-escape'),
    (r'does not say whether it adopts or views|unmarked `using var`|adopt .* or .* view', 'using-unmarked'),
    (r'cannot move out of `using var`|`using var`.*(scope-bound|reassign)|reassign.*`using var`', 'using-pin'),
    (r'temporary.*(escape|outlive|boundary)|borrow of (an? )?(unbound )?temporary', 'temporary-escape'),
    (r'Dispose\(\).*(borrow|not owned|borrowed)|dispose .* borrow|only an owner', 'dispose-of-borrow'),
    (r'(move|consume|transfer) .*(borrow|not owned|borrowed)|is borrowed', 'consume-of-borrow'),
    (r'owned-state|state match|state-match|borrowed-state (local|binding)|owned RHS|borrow RHS', 'state-match'),
    (r'would be dropped here without disposal', 'state-match'),      # an owned RHS into a borrowed-state LHS (the compiler words it as a drop)
    (r'a borrow cannot be passed where an `owned` value is required', 'consume-of-borrow'),
    (r'read of unassigned local|read while uninitialized|definite-assignment', 'read-of-moved'),
    (r'assign.*(borrow|owned).*to.*(owned|borrow)', 'state-match'),
    # --- warnings
    (r'after a dominating explicit Dispose\(\)|use-after-dispose|already disposed at this point|already disposed|second Dispose|is disposed again here', 'use-after-dispose'),
    (r'retired pre-v0.208 record-update spelling|init-assignment', 'PARSE:replacing-move-with'),
]

IGNORED_WARNINGS = [
    r'implicitly disposed at the end of (its scope|the method)',    # K3.14 must-use lint (not modelled)
    r'mutable static field|static readonly field .* reachable from every domain',   # §22.12 preamble
]

NOTE_RE = re.compile(r'(was here|declared here|declared at|is here|here\)?$|^note:)')


def normalise(msg):
    msg = re.sub(r'`[^`]*`', '`_`', msg)
    msg = re.sub(r'\d+', 'N', msg)
    return msg[:160]


def compiler_family(msg):
    for rx, fam in COMPILER_FAMILY:
        if re.search(rx, msg):
            return fam
    return 'UNMAPPED:' + normalise(msg)


def ignored_warning(msg):
    return any(re.search(rx, msg) for rx in IGNORED_WARNINGS)
