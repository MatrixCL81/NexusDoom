#!/usr/bin/env python3
"""UDP relay that simulates a bad internet connection, for testing NexusDoom
multiplayer locally.

The client joins the relay instead of the host:

    nexusdoom ... -host 2                          (UDP port 26101)
    tools/netimpair.py 26601 26101 --delay-ms 69 --loss 0.02
    nexusdoom ... -join 127.0.0.1 -port 26601

Every datagram, in both directions, is delayed by --delay-ms plus a random
0..--jitter-ms, dropped with probability --loss, sent twice with probability
--dup, and delayed by an extra --reorder-ms with probability --reorder (so it
arrives after later datagrams). Prints statistics on Ctrl-C or after
--duration seconds.
"""
import argparse
import asyncio
import random
import signal


class Direction:
    def __init__(self, name, args, rng):
        self.name = name
        self.args = args
        self.rng = rng
        self.sent = self.dropped = self.duplicated = self.reordered = 0

    def schedule(self, loop, deliver, data):
        a = self.args
        if self.rng.random() < a.loss:
            self.dropped += 1
            return
        copies = 1
        if self.rng.random() < a.dup:
            copies = 2
            self.duplicated += 1
        for _ in range(copies):
            delay = a.delay_ms + self.rng.uniform(0, a.jitter_ms)
            if self.rng.random() < a.reorder:
                delay += a.reorder_ms
                self.reordered += 1
            self.sent += 1
            loop.call_later(delay / 1000.0, deliver, data)

    def stats(self):
        return (f"{self.name}: delivered {self.sent}, dropped {self.dropped}, "
                f"duplicated {self.duplicated}, reordered {self.reordered}")


class ClientSide(asyncio.DatagramProtocol):
    """Faces the joining client; remembers its address."""

    def __init__(self, relay):
        self.relay = relay
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        self.relay.client_addr = addr
        self.relay.to_host.schedule(self.relay.loop, self.relay.send_to_host, data)


class HostSide(asyncio.DatagramProtocol):
    """Faces the host."""

    def __init__(self, relay):
        self.relay = relay
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        self.relay.to_client.schedule(self.relay.loop, self.relay.send_to_client, data)

    def error_received(self, exc):
        pass  # host not running (yet): ICMP port unreachable


class Relay:
    def __init__(self, args):
        self.args = args
        rng = random.Random(args.seed)
        self.to_host = Direction("client -> host", args, rng)
        self.to_client = Direction("host -> client", args, rng)
        self.client_addr = None
        self.loop = None
        self.client_side = None
        self.host_side = None

    def send_to_host(self, data):
        self.host_side.transport.sendto(data)

    def send_to_client(self, data):
        if self.client_addr:
            self.client_side.transport.sendto(data, self.client_addr)

    async def run(self):
        self.loop = asyncio.get_running_loop()
        _, self.client_side = await self.loop.create_datagram_endpoint(
            lambda: ClientSide(self), local_addr=("0.0.0.0", self.args.listen_port))
        _, self.host_side = await self.loop.create_datagram_endpoint(
            lambda: HostSide(self), remote_addr=(self.args.host, self.args.host_port))

        stop = asyncio.Event()
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                self.loop.add_signal_handler(sig, stop.set)
            except (NotImplementedError, RuntimeError):
                pass  # Windows: Ctrl-C raises KeyboardInterrupt instead
        if self.args.duration:
            self.loop.call_later(self.args.duration, stop.set)
        await stop.wait()

        print(self.to_host.stats())
        print(self.to_client.stats())


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("listen_port", type=int, help="port the client joins")
    p.add_argument("host_port", type=int, help="port the host listens on")
    p.add_argument("--host", default="127.0.0.1", help="host address (default 127.0.0.1)")
    p.add_argument("--delay-ms", type=float, default=0, help="one-way delay")
    p.add_argument("--jitter-ms", type=float, default=0, help="extra random delay 0..N")
    p.add_argument("--loss", type=float, default=0, help="drop probability (0..1)")
    p.add_argument("--dup", type=float, default=0, help="duplication probability")
    p.add_argument("--reorder", type=float, default=0, help="reorder probability")
    p.add_argument("--reorder-ms", type=float, default=50, help="extra delay of reordered datagrams")
    p.add_argument("--seed", type=int, default=None, help="random seed")
    p.add_argument("--duration", type=float, default=0, help="stop after N seconds")
    args = p.parse_args()
    try:
        asyncio.run(Relay(args).run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
