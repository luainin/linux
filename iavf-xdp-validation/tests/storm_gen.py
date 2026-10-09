#!/usr/bin/env python3
"""Achilles-style network storm generator (AF_PACKET, stdlib only).

Run on a sender on the same L2 segment as the DUT. Modes mirror the Achilles
storm tests used on DeltaV: eth-bcast, arp-req, arp-reply, arp-cache,
ip-unicast, ip-bcast, ip-frag, icmp, tcp-syn, tcp-zerowin, udp-bcast.

  storm_gen.py MODE --iface eth0 --dst-ip 10.4.0.50 --dst-mac 00:15:5d:.. \
                --pps 100000 --seconds 30 [--size 64] [--procs 4]
"""
import argparse
import os
import random
import socket
import struct
import time

BCAST = b"\xff" * 6


def mac(s):
    return bytes(int(x, 16) for x in s.split(":"))


def ip(s):
    return socket.inet_aton(s)


def csum(b):
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    s = (s >> 16) + (s & 0xFFFF)
    s += s >> 16
    return ~s & 0xFFFF


def eth(dst, src, typ):
    return dst + src + struct.pack("!H", typ)


def ipv4(src, dst, proto, payload, ident=0, flags_frag=0):
    h = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), ident,
                    flags_frag, 64, proto, 0, src, dst)
    h = h[:10] + struct.pack("!H", csum(h)) + h[12:]
    return h + payload


def pad(frame, size):
    return frame + b"\0" * max(0, size - len(frame))


def l4_csum(src, dst, proto, seg):
    ph = src + dst + struct.pack("!BBH", 0, proto, len(seg))
    return csum(ph + seg)


def tcp(src, dst, sport, dport, flags, win, payload=b""):
    seg = struct.pack("!HHIIBBHHH", sport, dport, random.getrandbits(32), 0,
                      5 << 4, flags, win, 0, 0) + payload
    c = l4_csum(src, dst, 6, seg)
    return seg[:16] + struct.pack("!H", c) + seg[18:]


def udp(src, dst, sport, dport, payload):
    seg = struct.pack("!HHHH", sport, dport, 8 + len(payload), 0) + payload
    c = l4_csum(src, dst, 17, seg) or 0xFFFF
    return seg[:6] + struct.pack("!H", c) + seg[8:]


def arp(op, smac, sip, dmac, dip):
    return struct.pack("!HHBBH", 1, 0x0800, 6, 4, op) + smac + sip + dmac + dip


def build(a, smac, sip, dmac, dip):
    """Return a callable producing the next frame for the chosen mode."""
    m, size = a.mode, a.size
    rnd = random.Random(os.getpid())

    def rmac():
        return bytes([0x02] + [rnd.getrandbits(8) for _ in range(5)])

    def rip():
        return struct.pack("!I", 0x0A000000 | rnd.getrandbits(24))

    if m == "eth-bcast":
        f = pad(eth(BCAST, smac, 0x88B5) + b"storm", size)
        return lambda: f
    if m == "arp-req":
        f = pad(eth(BCAST, rmac(), 0x0806) + arp(1, smac, rip(), b"\0" * 6, dip), size)
        return lambda: f
    if m == "arp-reply":
        f = pad(eth(dmac, smac, 0x0806) + arp(2, smac, sip, dmac, dip), size)
        return lambda: f
    if m == "arp-cache":
        return lambda: pad(eth(BCAST, rmac(), 0x0806) +
                           arp(1, rmac(), rip(), b"\0" * 6, dip), size)
    if m == "ip-unicast":
        pl = udp(sip, dip, 5000, 5001, b"x" * max(0, size - 42))
        f = pad(eth(dmac, smac, 0x0800) + ipv4(sip, dip, 17, pl), size)
        return lambda: f
    if m == "ip-bcast":
        b = ip("255.255.255.255")
        pl = udp(sip, b, 5000, 5001, b"x" * max(0, size - 42))
        f = pad(eth(BCAST, smac, 0x0800) + ipv4(sip, b, 17, pl), size)
        return lambda: f
    if m == "udp-bcast":
        b = ip(a.bcast)
        pl = udp(sip, b, 5000, a.port, b"x" * max(0, size - 42))
        f = pad(eth(BCAST, smac, 0x0800) + ipv4(sip, b, 17, pl), size)
        return lambda: f
    if m == "ip-frag":
        body = b"x" * 1480

        def frag():
            i = rnd.getrandbits(16)
            return [eth(dmac, smac, 0x0800) + ipv4(sip, dip, 17, body, i, 0x2000),
                    eth(dmac, smac, 0x0800) + ipv4(sip, dip, 17, body[:200], i, 185)]
        return frag
    if m == "icmp":
        def icmp():
            t, c = rnd.getrandbits(8), rnd.getrandbits(8)
            body = struct.pack("!BBHI", t, c, 0, rnd.getrandbits(32)) + b"x" * max(0, size - 46)
            body = body[:2] + struct.pack("!H", csum(body)) + body[4:]
            return pad(eth(dmac, smac, 0x0800) + ipv4(sip, dip, 1, body), size)
        return icmp
    if m == "tcp-syn":
        return lambda: pad(eth(dmac, smac, 0x0800) + ipv4(
            rip(), dip, 6, tcp(rip(), dip, rnd.randint(1024, 65535), a.port, 0x02, 8192)), size)
    if m == "tcp-zerowin":
        return lambda: pad(eth(dmac, smac, 0x0800) + ipv4(
            sip, dip, 6, tcp(sip, dip, 40000, a.port, 0x10, 0)), size)
    raise SystemExit("unknown mode " + m)


def worker(a, n):
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
    s.bind((a.iface, 0))
    smac = s.getsockname()[4]
    sip = ip(a.src_ip)
    dmac = mac(a.dst_mac)
    nxt = build(a, smac, sip, dmac, ip(a.dst_ip))
    per = a.pps / a.procs
    end = time.monotonic() + a.seconds
    sent = 0
    t0 = time.monotonic()
    while True:
        now = time.monotonic()
        if now >= end:
            break
        if sent >= per * (now - t0):
            time.sleep(0.0002)
            continue
        burst = max(1, int(per * 0.001))
        for _ in range(burst):
            f = nxt()
            for x in (f if isinstance(f, list) else [f]):
                try:
                    s.send(x)
                except OSError:
                    pass
                sent += 1
    print("worker %d sent %d" % (n, sent), flush=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("mode")
    p.add_argument("--iface", required=True)
    p.add_argument("--dst-ip", required=True)
    p.add_argument("--dst-mac", required=True)
    p.add_argument("--src-ip", default="10.4.0.99")
    p.add_argument("--bcast", default="10.4.255.255")
    p.add_argument("--port", type=int, default=80)
    p.add_argument("--pps", type=int, default=100000)
    p.add_argument("--seconds", type=int, default=30)
    p.add_argument("--size", type=int, default=64)
    p.add_argument("--procs", type=int, default=1)
    a = p.parse_args()
    pids = []
    for i in range(a.procs):
        pid = os.fork()
        if pid == 0:
            worker(a, i)
            os._exit(0)
        pids.append(pid)
    for pid in pids:
        os.waitpid(pid, 0)


if __name__ == "__main__":
    main()
