"""Kerberos messages as Xbox Live uses them: reading their structure.

Field numbers are RFC 4120's; the padata types past 128 are Microsoft's and
Xbox Live's (names as Xombie gives them)."""
from . import der

MSG = {10: "AS-REQ", 11: "AS-REP", 12: "TGS-REQ", 13: "TGS-REP", 30: "KRB-ERROR"}
PADATA = {
    1: "PA_TGS_REQ", 2: "PA_ENC_TIMESTAMP", 3: "PA_PW_SALT", 11: "PA_ETYPE_INFO",
    19: "PA_ETYPE_INFO2", 128: "PA_PAC_REQUEST", 131: "PA_MSKILE_FOR_CHECK_DUPS",
    203: "PA_XBOX_ACCOUNT_CREATION", 204: "PA_XBOX_PPA", 206: "PA_XBOX_CLIENT_VERSION",
}


def mask(s: str) -> str:
    """A name with its digits hidden: a machine account is named after the
    console's serial number."""
    return "".join("#" if c.isdigit() else c for c in s)


def principal(n: der.Node) -> str:
    kind = der.as_int(n.child(0))
    names = [der.as_str(c) for c in n.child(1).children]
    return f"{mask('/'.join(names))} (type {kind})"


def padata(n: der.Node):
    """[(type, value bytes)] of a SEQUENCE OF PA-DATA."""
    out = []
    for pa in n.children:
        out.append((der.as_int(pa.child(1)), pa.child(2).value))
    return out


def describe(payload: bytes) -> list:
    """Lines saying what a Kerberos message holds, lengths only for anything
    encrypted or opaque."""
    root = der.parse(payload)
    if root.cls != 1:
        return [f"not a Kerberos application message (tag class {root.cls})"]
    kind = MSG.get(root.tag, f"application {root.tag}")
    body = root.children[0]
    lines = [kind]
    if root.tag in (10, 12):
        pa = body.child(3)
        for t, v in padata(pa) if pa else []:
            lines.append(f"  padata {t} {PADATA.get(t, '?')}: {len(v)} bytes")
        rb = body.child(4)
        if rb.child(1):
            lines.append(f"  cname  {principal(rb.child(1))}")
        lines.append(f"  realm  {der.as_str(rb.child(2))}")
        if rb.child(3):
            lines.append(f"  sname  {principal(rb.child(3))}")
        if rb.child(7):
            lines.append(f"  nonce  {der.as_int(rb.child(7)) & 0xFFFFFFFF:#010x}")
        if rb.child(8):
            lines.append("  etype  " + ", ".join(str(der.as_int(e)) for e in rb.child(8).children))
        if rb.child(0):
            lines.append(f"  kdc-options {rb.child(0).value.hex()}")
    elif root.tag in (11, 13):
        pa = body.child(2)
        for t, v in padata(pa) if pa else []:
            lines.append(f"  padata {t} {PADATA.get(t, '?')}: {len(v)} bytes")
        lines.append(f"  crealm {der.as_str(body.child(3))}")
        lines.append(f"  cname  {principal(body.child(4))}")
        tkt = body.child(5)
        tb = tkt.children[0]
        lines.append(f"  ticket for {principal(tb.child(2))} in {der.as_str(tb.child(1))}, "
                     f"{len(tb.child(3).child(2).value)} bytes sealed")
        ep = body.child(6)
        lines.append(f"  enc-part etype {der.as_int(ep.child(0))}, "
                     f"{len(ep.child(2).value)} bytes")
    elif root.tag == 30:
        code = body.child(6)
        lines.append(f"  error-code {der.as_int(code) if code else '?'}")
    return lines
