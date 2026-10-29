"""tools.xbl: DER both ways, and what decode says about a message, on a
made-up AS-REQ (no real console's data)."""
import unittest

from tools.xbl import der, kerberos


def _principal(kind, *names):
    return der.seq(der.ctx(0, der.integer(kind)),
                   der.ctx(1, der.seq(*[der.gstring(n) for n in names])))


def _as_req():
    pa = der.seq(der.ctx(1, der.integer(206)), der.ctx(2, der.octets(b"\x01" * 5)))
    body = der.seq(der.ctx(0, der.bitstring(b"\x00\x01\x00\x00")),
                   der.ctx(1, _principal(10, "SN.123456789012@insignia.live")),
                   der.ctx(2, der.gstring("passport.net")),
                   der.ctx(3, _principal(2, "krbtgt", "XBOX.COM")),
                   der.ctx(7, der.integer(0x1234)),
                   der.ctx(8, der.seq(der.integer(23))))
    return der.app(10, der.seq(der.ctx(1, der.integer(5)), der.ctx(2, der.integer(10)),
                               der.ctx(3, der.seq(pa)), der.ctx(4, body)))


class DerTest(unittest.TestCase):
    def test_round_trip(self):
        msg = _as_req()
        root = der.parse(msg)
        self.assertEqual((root.cls, root.tag), (1, 10))
        body = root.children[0]
        self.assertEqual(der.as_int(body.child(2)), 10)
        self.assertEqual(der.as_str(body.child(4).child(2)), "passport.net")

    def test_long_lengths(self):
        big = der.octets(b"\xAA" * 300)
        self.assertEqual(big[:4], b"\x04\x82\x01\x2c")
        self.assertEqual(len(der.parse(big).value), 300)

    def test_integers(self):
        for v in (0, 1, 127, 128, 255, 0x30C8D010, -1):
            self.assertEqual(der.as_int(der.parse(der.integer(v))), v)

    def test_trailing_bytes_refused(self):
        with self.assertRaises(ValueError):
            der.parse(der.integer(5) + b"\x00")


class DescribeTest(unittest.TestCase):
    def test_as_req(self):
        lines = kerberos.describe(_as_req())
        self.assertEqual(lines[0], "AS-REQ")
        text = "\n".join(lines)
        self.assertIn("padata 206 PA_XBOX_CLIENT_VERSION: 5 bytes", text)
        self.assertIn("realm  passport.net", text)
        self.assertIn("krbtgt/XBOX.COM", text)
        self.assertNotIn("123456789012", text)      # the serial is masked
        self.assertIn("nonce  0x00001234", text)


if __name__ == "__main__":
    unittest.main()
