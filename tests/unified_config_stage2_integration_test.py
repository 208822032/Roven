#!/usr/bin/env python3

import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


ROOT = Path(sys.argv[1]).resolve()
KVSTORE = (ROOT / "kvstore").resolve()


def pick_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def encode_command(*parts):
    encoded = [f"*{len(parts)}\r\n".encode()]
    for part in parts:
        value = part if isinstance(part, bytes) else str(part).encode()
        encoded.extend((f"${len(value)}\r\n".encode(), value, b"\r\n"))
    return b"".join(encoded)


class RespClient:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.socket.settimeout(3)
        self.stream = self.socket.makefile("rb")

    def close(self):
        self.stream.close()
        self.socket.close()

    def command(self, *parts):
        self.socket.sendall(encode_command(*parts))
        return self.read()

    def pipeline(self, commands):
        self.socket.sendall(b"".join(encode_command(*command) for command in commands))
        return [self.read() for _ in commands]

    def read_line(self):
        line = self.stream.readline()
        if not line:
            raise EOFError("server closed connection")
        if not line.endswith(b"\r\n"):
            raise RuntimeError(f"invalid RESP line {line!r}")
        return line[:-2]

    def read(self):
        prefix = self.stream.read(1)
        if not prefix:
            raise EOFError("server closed connection")
        if prefix == b"+":
            return self.read_line().decode(errors="replace")
        if prefix == b"-":
            return {"error": self.read_line().decode(errors="replace")}
        if prefix == b":":
            return int(self.read_line())
        if prefix == b"$":
            length = int(self.read_line())
            if length < 0:
                return None
            value = self.stream.read(length)
            if self.stream.read(2) != b"\r\n":
                raise RuntimeError("invalid bulk terminator")
            return value.decode(errors="replace")
        if prefix == b"*":
            return [self.read() for _ in range(int(self.read_line()))]
        raise RuntimeError(f"unknown RESP prefix {prefix!r}")


def assert_ok(value, label):
    assert value in ("OK", "EXIST"), f"{label}: {value!r}"


def assert_error(value, needle, label):
    assert isinstance(value, dict) and needle in value.get("error", ""), f"{label}: {value!r}"


def base_config(port, mode="reactor", **overrides):
    values = {
        "listen_address": "127.0.0.1",
        "worker_threads": 1,
        "max_connections": 32,
        "listen_backlog": 32,
        "max_request_bytes": 8192,
        "max_response_bytes": 4096,
        "max_commands_per_read": 8,
        "default_engine": "array",
        "enabled_engines": "array,rbtree,hash,skiptable",
        "memory_backend": "system",
        "array_initial_capacity": 16,
        "hash_initial_capacity": 64,
        "max_value_bytes": 4096,
        "ttl_enabled": "true",
        "ttl_scan_enabled": "true",
        "ttl_scan_interval_ms": 50,
        "ttl_scan_budget": 16,
        "rdb_enabled": "false",
        "aof_enabled": "false",
        "aof_fsync": "everysec",
        "aof_rewrite_enabled": "false",
        "aof_rewrite_min_bytes": 1024,
        "aof_rewrite_min_growth_bytes": 512,
        "aof_rewrite_growth_percent": 10,
    }
    values.update(overrides)
    return f"""sync_mode=disabled
[server]
role=master
config_root=.
data_dir=data
test_mode=true

[network]
listen_address={values['listen_address']}
port={port}
mode={mode}
worker_threads={values['worker_threads']}
max_connections={values['max_connections']}
listen_backlog={values['listen_backlog']}
max_request_bytes={values['max_request_bytes']}
max_response_bytes={values['max_response_bytes']}
max_commands_per_read={values['max_commands_per_read']}

[storage]
default_engine={values['default_engine']}
enabled_engines={values['enabled_engines']}
memory_backend={values['memory_backend']}
array_initial_capacity={values['array_initial_capacity']}
hash_initial_capacity={values['hash_initial_capacity']}
max_value_bytes={values['max_value_bytes']}

[ttl]
enabled={values['ttl_enabled']}
scan_enabled={values['ttl_scan_enabled']}
scan_interval_ms={values['ttl_scan_interval_ms']}
scan_budget={values['ttl_scan_budget']}

[persistence]
rdb_enabled={values['rdb_enabled']}
rdb_path=stage2.rdb
aof_enabled={values['aof_enabled']}
aof_path=stage2.aof
aof_fsync={values['aof_fsync']}
aof_rewrite_enabled={values['aof_rewrite_enabled']}
aof_rewrite_min_bytes={values['aof_rewrite_min_bytes']}
aof_rewrite_min_growth_bytes={values['aof_rewrite_min_growth_bytes']}
aof_rewrite_growth_percent={values['aof_rewrite_growth_percent']}
legacy_dump_path=stage2.dat
startup_recovery_order=rdb_then_aof_then_index

[replication]
sync_listen_port={port + 1000 if port <= 64535 else port - 1000}

[vector]
index_type=core_flat
"""


class Server:
    def __init__(self, case_dir, config_text, cwd=None):
        self.case_dir = case_dir
        self.config = case_dir / "kvstore.conf"
        self.log_path = case_dir / "server.log"
        self.config.write_text(config_text)
        (case_dir / "data").mkdir(exist_ok=True)
        self.log = self.log_path.open("ab")
        self.process = subprocess.Popen(
            [str(KVSTORE), "--config", str(self.config)],
            cwd=str(cwd or case_dir),
            stdout=self.log,
            stderr=subprocess.STDOUT,
        )
        self.port = int(next(
            line.split("=", 1)[1] for line in config_text.splitlines()
            if line.startswith("port=")
        ))
        self.wait_ready()

    def wait_ready(self):
        deadline = time.time() + 12
        while time.time() < deadline:
            if self.process.poll() is not None:
                self.log.flush()
                raise RuntimeError(
                    f"server exited {self.process.returncode}:\n{self.log_path.read_text(errors='replace')}"
                )
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.2):
                    return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError(f"server port {self.port} did not become ready")

    def client(self):
        return RespClient(self.port)

    def stop(self, graceful=True):
        if self.process.poll() is None and graceful:
            try:
                client = self.client()
                client.command("QUIT")
                client.close()
            except (OSError, EOFError):
                pass
        if self.process.poll() is None:
            try:
                self.process.wait(timeout=12)
            except subprocess.TimeoutExpired:
                self.process.send_signal(signal.SIGTERM)
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
        self.log.close()
        assert self.process.returncode == 0, (
            self.process.returncode,
            self.log_path.read_text(errors="replace"),
        )


def new_case(root, name):
    path = root / name
    path.mkdir()
    return path


def test_network_models_and_engines(root):
    cases = [
        ("reactor", "array", "SET", "RSET"),
        ("proactor", "rbtree", "RSET", "HSET"),
        ("ntyco", "hash", "HSET", "SSET"),
        ("reactor", "skiptable", "SSET", "RSET"),
    ]
    for index, (mode, engine, explicit_set, disabled_set) in enumerate(cases):
        case = new_case(root, f"network-{index}-{mode}-{engine}")
        port = pick_port()
        server = Server(case, base_config(
            port,
            mode,
            default_engine=engine,
            enabled_engines=engine,
        ))
        client = server.client()
        assert_ok(client.command("SET", "plain", f"{mode}-{engine}"), "plain SET")
        assert client.command("GET", "plain") == f"{mode}-{engine}"
        assert_ok(client.command(explicit_set, "explicit", "value"), explicit_set)
        assert_error(client.command(disabled_set, "disabled", "value"), "storage engine disabled", disabled_set)
        replies = client.pipeline((("SET", "pipe", "one"), ("GET", "pipe")))
        assert replies == ["OK", "one"], (mode, replies)
        client.close()
        server.stop()


def test_limits_and_atomic_write_gate(root):
    case = new_case(root, "limits")
    port = pick_port()
    server = Server(case, base_config(port, max_value_bytes=16, max_response_bytes=1024))
    client = server.client()
    commands = [
        ("SET", "GET"),
        ("RSET", "RGET"),
        ("HSET", "HGET"),
        ("SSET", "SGET"),
        ("PAPERSET", "PAPERGET"),
    ]
    for set_name, get_name in commands:
        key = set_name.lower()
        assert_ok(client.command(set_name, key, "x" * 16), f"{set_name} boundary")
        assert client.command(get_name, key) == "x" * 16
        rejected_key = key + "-rejected"
        assert_error(
            client.command(set_name, rejected_key, "y" * 17),
            "storage.max_value_bytes",
            f"{set_name} overflow",
        )
        assert client.command(get_name, rejected_key) == "NO EXIST"
    replies = client.pipeline((
        ("SET", "pipeline-ok", "z" * 16),
        ("SET", "pipeline-rejected", "z" * 17),
    ))
    assert replies[0] == "OK"
    assert_error(replies[1], "storage.max_value_bytes", "pipeline overflow")
    assert client.command("GET", "pipeline-ok") == "z" * 16
    assert client.command("GET", "pipeline-rejected") == "NO EXIST"
    client.close()
    server.stop()

    request_case = new_case(root, "request-limit")
    port = pick_port()
    server = Server(request_case, base_config(
        port,
        max_value_bytes=4096,
        max_request_bytes=1024,
    ))
    client = server.client()
    client.socket.sendall(encode_command("SET", "oversized-request", "q" * 1500))
    try:
        client.read()
        raise AssertionError("request byte overflow was accepted")
    except (EOFError, ConnectionResetError, OSError):
        pass
    client.close()
    verify = server.client()
    assert verify.command("GET", "oversized-request") == "NO EXIST"
    verify.close()
    server.stop()

    connection_case = new_case(root, "connection-limit")
    port = pick_port()
    server = Server(connection_case, base_config(port, max_connections=1))
    first = server.client()
    assert_ok(first.command("SET", "first", "alive"), "first connection")
    second = server.client()
    try:
        second.command("GET", "first")
        raise AssertionError("connection limit overflow was accepted")
    except (EOFError, ConnectionResetError, OSError):
        pass
    second.close()
    assert first.command("GET", "first") == "alive"
    first.close()
    server.stop()

    response_case = new_case(root, "response-limit")
    port = pick_port()
    server = Server(response_case, base_config(
        port,
        max_value_bytes=2048,
        max_request_bytes=4096,
        max_response_bytes=1024,
    ))
    client = server.client()
    assert_ok(client.command("SET", "large-response", "v" * 1500), "large response seed")
    assert_error(client.command("GET", "large-response"), "network.max_response_bytes", "response limit")
    client.close()
    server.stop()

    command_case = new_case(root, "command-limit")
    port = pick_port()
    server = Server(command_case, base_config(port, max_commands_per_read=1))
    client = server.client()
    client.socket.sendall(encode_command("SET", "a", "1") + encode_command("SET", "b", "2"))
    try:
        client.read()
        raise AssertionError("commands-per-read overflow was accepted")
    except (EOFError, ConnectionResetError, OSError):
        pass
    client.close()
    verify = server.client()
    assert verify.command("GET", "a") == "NO EXIST"
    assert verify.command("GET", "b") == "NO EXIST"
    verify.close()
    server.stop()


def test_ttl_modes(root):
    disabled_case = new_case(root, "ttl-disabled")
    port = pick_port()
    server = Server(disabled_case, base_config(
        port,
        ttl_enabled="false",
        ttl_scan_enabled="false",
    ))
    client = server.client()
    assert_ok(client.command("SET", "ttl-disabled", "value"), "TTL disabled seed")
    assert_error(client.command("EXPIRE", "ttl-disabled", "1"), "TTL disabled", "EXPIRE disabled")
    assert client.command("GET", "ttl-disabled") == "value"
    client.close()
    server.stop()

    scan_case = new_case(root, "ttl-scan")
    port = pick_port()
    server = Server(scan_case, base_config(
        port,
        ttl_enabled="true",
        ttl_scan_enabled="true",
        ttl_scan_interval_ms=25,
        ttl_scan_budget=3,
    ))
    client = server.client()
    assert_ok(client.command("SET", "scan", "value"), "scan seed")
    assert client.command("EXPIRE", "scan", "1") == "OK"
    time.sleep(1.3)
    assert client.command("DEL", "scan") == "NO EXIST"
    client.close()
    server.stop()

    lazy_case = new_case(root, "ttl-lazy")
    port = pick_port()
    server = Server(lazy_case, base_config(
        port,
        ttl_enabled="true",
        ttl_scan_enabled="false",
        ttl_scan_interval_ms=25,
        ttl_scan_budget=3,
    ))
    client = server.client()
    assert_ok(client.command("SET", "lazy", "value"), "lazy seed")
    assert client.command("EXPIRE", "lazy", "1") == "OK"
    time.sleep(1.2)
    assert client.command("GET", "lazy") == "NO EXIST"
    client.close()
    server.stop()


def seed_persistence(client):
    assert_ok(client.command("SET", "persist", "old"), "persistence SET")
    assert_ok(client.command("MOD", "persist", "new"), "persistence MOD")
    assert_ok(client.command("SET", "gone", "value"), "persistence gone SET")
    assert_ok(client.command("DEL", "gone"), "persistence DEL")


def verify_persistence(client, expected):
    assert client.command("GET", "persist") == expected
    assert client.command("GET", "gone") == "NO EXIST"


def test_persistence_modes_and_cwd(root):
    modes = [
        ("rdb-only", "true", "false", "everysec", "new"),
        ("aof-only", "false", "true", "always", "new"),
        ("rdb-aof", "true", "true", "no", "new"),
        ("disabled", "false", "false", "everysec", "NO EXIST"),
    ]
    foreign_cwd = new_case(root, "foreign-cwd")
    for name, rdb_enabled, aof_enabled, fsync, expected in modes:
        case = new_case(root, name)
        port = pick_port()
        config = base_config(
            port,
            rdb_enabled=rdb_enabled,
            aof_enabled=aof_enabled,
            aof_fsync=fsync,
        )
        server = Server(case, config, cwd=foreign_cwd)
        client = server.client()
        seed_persistence(client)
        client.close()
        server.stop()
        data_dir = case / "data"
        assert (data_dir / "stage2.rdb").exists() == (rdb_enabled == "true")
        assert (data_dir / "stage2.aof").exists() == (aof_enabled == "true")
        server = Server(case, config, cwd=foreign_cwd)
        client = server.client()
        verify_persistence(client, expected)
        client.close()
        server.stop()


def test_aof_rewrite_switch(root):
    disabled_case = new_case(root, "rewrite-disabled")
    port = pick_port()
    server = Server(disabled_case, base_config(
        port,
        aof_enabled="true",
        aof_rewrite_enabled="false",
        aof_fsync="always",
    ))
    client = server.client()
    assert_error(client.command("AOFREWRITE"), "AOF rewrite disabled", "rewrite disabled")
    client.close()
    server.stop()

    enabled_case = new_case(root, "rewrite-enabled")
    port = pick_port()
    server = Server(enabled_case, base_config(
        port,
        aof_enabled="true",
        aof_rewrite_enabled="true",
        aof_fsync="everysec",
        aof_rewrite_min_bytes=1,
        aof_rewrite_min_growth_bytes=1,
        aof_rewrite_growth_percent=1,
    ))
    client = server.client()
    assert_ok(client.command("SET", "rewrite", "value"), "rewrite seed")
    assert client.command("AOFREWRITE") == "OK"
    client.close()
    server.stop()


def main():
    assert KVSTORE.is_file(), f"missing binary: {KVSTORE}"
    with tempfile.TemporaryDirectory(prefix="kvlite-unified-config-stage2-integration-") as tmp:
        root = Path(tmp)
        test_network_models_and_engines(root)
        test_limits_and_atomic_write_gate(root)
        test_ttl_modes(root)
        test_persistence_modes_and_cwd(root)
        test_aof_rewrite_switch(root)
    print("unified config Stage 2 integration tests: PASS")


if __name__ == "__main__":
    main()
