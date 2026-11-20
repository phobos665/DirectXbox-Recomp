"""Reading a RECOMP_NET_PCAP file: classic pcap, Ethernet, IPv4, UDP."""
import struct
from dataclasses import dataclass


@dataclass
class Datagram:
    time: float
    src: str
    sport: int
    dst: str
    dport: int
    payload: bytes


def _ip(b: bytes) -> str:
    return ".".join(str(x) for x in b)


def udp_datagrams(path: str):
    """Every UDP datagram in the file, in order."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 24 or struct.unpack_from("<I", data, 0)[0] != 0xA1B2C3D4:
        raise ValueError(f"{path}: not a little-endian classic pcap file")
    off = 24
    while off + 16 <= len(data):
        sec, usec, incl, _orig = struct.unpack_from("<IIII", data, off)
        off += 16
        frame = data[off:off + incl]
        off += incl
        if len(frame) < 34 or frame[12:14] != b"\x08\x00" or frame[23] != 17:
            continue
        ihl = (frame[14] & 0x0F) * 4
        u = 14 + ihl
        if len(frame) < u + 8:
            continue
        sport, dport, length = struct.unpack_from(">HHH", frame, u)
        yield Datagram(sec + usec / 1e6, _ip(frame[26:30]), sport, _ip(frame[30:34]),
                       dport, frame[u + 8:u + length])
