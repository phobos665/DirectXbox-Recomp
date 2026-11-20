"""Just enough DER for Kerberos: parse into a tree, and encode."""
from dataclasses import dataclass, field
from typing import List, Optional


@dataclass
class Node:
    cls: int                 # 0 universal, 1 application, 2 context, 3 private
    constructed: bool
    tag: int
    value: bytes             # the content octets
    children: List["Node"] = field(default_factory=list)

    def child(self, ctx_tag: int) -> Optional["Node"]:
        """The context-tagged [n] child's single inner element, or None."""
        for c in self.children:
            if c.cls == 2 and c.tag == ctx_tag:
                return c.children[0] if c.children else c
        return None


def _read_len(b: bytes, i: int):
    first = b[i]
    i += 1
    if first < 0x80:
        return first, i
    n = first & 0x7F
    if n == 0 or n > 4 or i + n > len(b):
        raise ValueError("unsupported DER length")
    return int.from_bytes(b[i:i + n], "big"), i + n


def parse_one(b: bytes, i: int = 0):
    """One element at b[i:]; returns (node, index after it)."""
    if i + 2 > len(b):
        raise ValueError("truncated DER")
    t = b[i]
    i += 1
    cls, constructed, tag = t >> 6, bool(t & 0x20), t & 0x1F
    if tag == 0x1F:                                   # high tag number
        tag = 0
        while True:
            x = b[i]
            i += 1
            tag = (tag << 7) | (x & 0x7F)
            if not x & 0x80:
                break
    n, i = _read_len(b, i)
    if i + n > len(b):
        raise ValueError("DER element runs past its buffer")
    node = Node(cls, constructed, tag, b[i:i + n])
    if constructed:
        j = 0
        while j < n:
            c, j2 = parse_one(node.value, j)
            node.children.append(c)
            j = j2
    return node, i + n


def parse(b: bytes) -> Node:
    node, end = parse_one(b, 0)
    if end != len(b):
        raise ValueError(f"{len(b) - end} bytes after the DER element")
    return node


def as_int(n: Node) -> int:
    return int.from_bytes(n.value, "big", signed=True)


def as_str(n: Node) -> str:
    return n.value.decode("latin-1")


def _len(n: int) -> bytes:
    if n < 0x80:
        return bytes([n])
    body = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(body)]) + body


def tlv(tag_byte: int, content: bytes) -> bytes:
    """One element, its identifier octet given whole (tag < 31)."""
    return bytes([tag_byte]) + _len(len(content)) + content


def seq(*items: bytes) -> bytes:
    return tlv(0x30, b"".join(items))


def ctx(n: int, inner: bytes) -> bytes:
    return tlv(0xA0 | n, inner)


def app(n: int, inner: bytes) -> bytes:
    return tlv(0x60 | n, inner)


def integer(v: int) -> bytes:
    length = max(1, (v.bit_length() + 8) // 8)
    return tlv(0x02, v.to_bytes(length, "big", signed=True))


def octets(b: bytes) -> bytes:
    return tlv(0x04, b)


def gstring(s: str) -> bytes:
    return tlv(0x1B, s.encode("latin-1"))     # GeneralString


def gtime(s: str) -> bytes:
    return tlv(0x18, s.encode("ascii"))       # GeneralizedTime, "YYYYMMDDHHMMSSZ"


def bitstring(b: bytes) -> bytes:
    return tlv(0x03, b"\x00" + b)
