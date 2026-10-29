"""py -3 -m tools.xbl decode <capture.pcap>

decode: every Kerberos message in a RECOMP_NET_PCAP capture, what it holds,
with lengths in place of anything encrypted and the serial number masked.
Other UDP (the security gateway on 3074, DNS) is listed by size."""
import argparse
import sys

from . import kerberos, pcap


def cmd_decode(args) -> int:
    t0 = None
    for d in pcap.udp_datagrams(args.pcap):
        t0 = d.time if t0 is None else t0
        head = f"{d.time - t0:7.2f}  {d.src}:{d.sport} -> {d.dst}:{d.dport}  {len(d.payload)} bytes"
        if 88 in (d.sport, d.dport):
            try:
                lines = kerberos.describe(d.payload)
            except ValueError as e:
                lines = [f"(not parsed: {e})"]
            print(head + "  " + lines[0])
            for line in lines[1:]:
                print("          " + line)
        elif args.all or 53 not in (d.sport, d.dport) and 67 not in (d.sport, d.dport):
            print(head)
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="python -m tools.xbl")
    sub = p.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("decode", help="what a capture's Kerberos messages hold")
    d.add_argument("pcap")
    d.add_argument("--all", action="store_true", help="DNS and DHCP too")
    d.set_defaults(func=cmd_decode)
    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
