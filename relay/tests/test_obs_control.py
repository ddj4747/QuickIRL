"""End-to-end control relay test with a simulated OBS WebSocket client.

Set RELAY_TEST_BINARY to a built obs-relay executable and put its shared
libraries on the platform's loader path before running this script.
"""

import base64
import concurrent.futures
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import struct
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.request


def unused_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class WebSocket:
    def __init__(self, port, credential):
        context = ssl._create_unverified_context()
        self.socket = context.wrap_socket(socket.create_connection(("127.0.0.1", port), timeout=5),
                                          server_hostname="localhost")
        self.socket.settimeout(5)
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        request = (f"GET /_relay/obs HTTP/1.1\r\nHost: localhost:{port}\r\n"
                   f"Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                   f"Sec-WebSocket-Key: {key}\r\nAuthorization: {credential}\r\n\r\n")
        self.socket.sendall(request.encode("ascii"))
        header = b""
        while not header.endswith(b"\r\n\r\n"):
            chunk = self.socket.recv(1)
            if not chunk:
                raise RuntimeError("Connection closed during WebSocket handshake")
            header += chunk
        self.status = int(header.split(b" ", 2)[1])

    def read_exact(self, length):
        data = b""
        while len(data) < length:
            chunk = self.socket.recv(length - len(data))
            if not chunk:
                raise RuntimeError("WebSocket closed")
            data += chunk
        return data

    def receive(self):
        while True:
            first, second = self.read_exact(2)
            length = second & 127
            if length == 126:
                length = struct.unpack("!H", self.read_exact(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", self.read_exact(8))[0]
            payload = self.read_exact(length)
            if first & 15 == 9:
                self.send_frame(payload, 10)
                continue
            if first & 15 == 1:
                return json.loads(payload)
            raise RuntimeError(f"Unexpected WebSocket opcode: {first & 15}")

    def send_frame(self, payload, opcode=1):
        mask = os.urandom(4)
        length = len(payload)
        if length < 126:
            header = bytes([0x80 | opcode, 0x80 | length])
        elif length < 65536:
            header = bytes([0x80 | opcode, 0x80 | 126]) + struct.pack("!H", length)
        else:
            header = bytes([0x80 | opcode, 0x80 | 127]) + struct.pack("!Q", length)
        masked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
        self.socket.sendall(header + mask + masked)

    def respond(self, request, status=200, body=b"{}"):
        message = {"type": "response", "id": request["id"], "status": status,
                   "content_type": "application/json", "body_base64": base64.b64encode(body).decode("ascii")}
        self.send_frame(json.dumps(message).encode("utf-8"))

    def close(self):
        self.socket.close()


class ControlRelayTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="obs-relay-test-")
        self.addCleanup(self.temp.cleanup)
        self.api_port, self.control_port = unused_port(), unused_port()
        while self.api_port == self.control_port:
            self.control_port = unused_port()
        config = Path(self.temp.name)
        (config / "relay.yml").write_text(f"ports:\n  api: {self.api_port}\n  obs_control: {self.control_port}\n")
        self.process = None
        self.addCleanup(self.stop_process)
        self.start_relay("127.0.0.1")
        self.api_key = (config / "api-key").read_text().strip()
        self.obs_key = (config / "obs-key").read_text().strip()

    def start_relay(self, public_host):
        env = os.environ.copy()
        env["RELAY_CONFIG_DIR"] = self.temp.name
        if public_host is None:
            env.pop("RELAY_PUBLIC_HOST", None)
            if os.name == "nt":
                shutil.copyfile(Path(os.environ["SystemRoot"]) / "System32" / "where.exe",
                                Path(self.temp.name) / "curl.exe")
            else:
                fake_curl = Path(self.temp.name) / "curl"
                fake_curl.write_text("#!/bin/sh\nexit 1\n")
                fake_curl.chmod(0o755)
            env["PATH"] = self.temp.name + os.pathsep + env.get("PATH", "")
        else:
            env["RELAY_PUBLIC_HOST"] = public_host
        binary = os.environ.get("RELAY_TEST_BINARY")
        if not binary:
            self.skipTest("Set RELAY_TEST_BINARY to the built relay executable")
        self.process = subprocess.Popen([binary], env=env, cwd=self.temp.name,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if self.process.poll() is not None:
                self.fail(f"Relay exited during startup: {self.process.returncode}")
            try:
                with socket.create_connection(("127.0.0.1", self.control_port), timeout=.2):
                    break
            except OSError:
                time.sleep(.1)
        else:
            self.fail("Control port did not start")

    def stop_process(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=10)

    def http(self, path, key=None, method="GET", body=None):
        request = urllib.request.Request(f"https://127.0.0.1:{self.control_port}{path}", data=body,
                                         method=method, headers={"Authorization": key or "",
                                                                 "Content-Type": "application/json"})
        try:
            response = urllib.request.urlopen(request, context=ssl._create_unverified_context(), timeout=20)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.status, response.read(), response.headers.get("Content-Type")

    def api_address(self, key, host="different.example:9999"):
        request = urllib.request.Request(f"https://127.0.0.1:{self.api_port}/obs-control",
                                         headers={"Authorization": key, "Host": host})
        try:
            response = urllib.request.urlopen(request, context=ssl._create_unverified_context(), timeout=5)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.status, response.read()

    def test_public_control_address(self):
        self.assertEqual(self.api_address("")[0], 401)
        status, body = self.api_address(self.api_key)
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body), {
            "address": f"https://127.0.0.1:{self.control_port}",
            "websocket": f"wss://127.0.0.1:{self.control_port}/_relay/obs",
        })

        self.stop_process()
        self.start_relay("2001:db8::1")
        status, body = self.api_address(self.api_key)
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body), {
            "address": f"https://[2001:db8::1]:{self.control_port}",
            "websocket": f"wss://[2001:db8::1]:{self.control_port}/_relay/obs",
        })

        self.stop_process()
        self.start_relay(None)
        status, body = self.api_address(self.api_key, "relay.example:1234")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body), {
            "address": f"https://relay.example:{self.control_port}",
            "websocket": f"wss://relay.example:{self.control_port}/_relay/obs",
        })
        status, body = self.api_address(self.api_key, "[2001:db8::2]:1234")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body), {
            "address": f"https://[2001:db8::2]:{self.control_port}",
            "websocket": f"wss://[2001:db8::2]:{self.control_port}/_relay/obs",
        })
        self.assertEqual(self.api_address(self.api_key, "invalid@relay.example")[0], 400)

    def test_authenticated_forwarding_and_disconnect(self):
        self.assertEqual(self.http("/getSceneList")[0], 401)
        self.assertEqual(self.http("/getSceneList", self.obs_key)[0], 401)
        self.assertEqual(self.http("/getSceneList", self.api_key)[0], 503)
        self.assertEqual(self.http("/not-an-obs-route", self.api_key)[0], 404)
        bad = WebSocket(self.control_port, self.api_key)
        self.assertEqual(bad.status, 401)
        bad.close()
        obs = WebSocket(self.control_port, self.obs_key)
        self.addCleanup(obs.close)
        self.assertEqual(obs.status, 101)
        extra = WebSocket(self.control_port, self.obs_key)
        self.addCleanup(extra.close)
        self.assertEqual(extra.status, 101)
        self.assertEqual(extra.read_exact(2)[0] & 15, 8)
        extra.close()

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            first = pool.submit(self.http, "/getSceneList?filter=all", self.api_key)
            second = pool.submit(self.http, "/setActiveScene?sceneName=Main", self.api_key, "POST", b'{"x":1}')
            requests = [obs.receive(), obs.receive()]
            by_method = {request["method"]: request for request in requests}
            self.assertEqual(by_method["GET"]["target"], "/getSceneList?filter=all")
            self.assertEqual(by_method["POST"]["target"], "/setActiveScene?sceneName=Main")
            self.assertEqual(base64.b64decode(by_method["POST"]["body_base64"]), b'{"x":1}')
            self.assertEqual(by_method["POST"]["content_type"], "application/json")
            obs.send_frame(b'{"type":"response","id":"unknown","status":200,"body_base64":"","content_type":""}')
            obs.respond(by_method["POST"], 202, b'{"posted":true}')
            obs.respond(by_method["GET"], 200, b'["Main"]')
            self.assertEqual(first.result(), (200, b'["Main"]', "application/json"))
            self.assertEqual(second.result(), (202, b'{"posted":true}', "application/json"))

            waiting = pool.submit(self.http, "/streamState", self.api_key)
            obs.receive()
            obs.close()
            self.assertEqual(waiting.result()[0], 503)

        replacement = WebSocket(self.control_port, self.obs_key)
        self.addCleanup(replacement.close)
        self.assertEqual(replacement.status, 101)
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            after_reconnect = pool.submit(self.http, "/streamState", self.api_key)
            replacement.respond(replacement.receive(), 200, b'{"status":true}')
            self.assertEqual(after_reconnect.result()[0], 200)

    def test_timeout(self):
        obs = WebSocket(self.control_port, self.obs_key)
        self.addCleanup(obs.close)
        self.assertEqual(obs.status, 101)
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            waiting = pool.submit(self.http, "/startStream", self.api_key, "POST", b"")
            obs.receive()
            self.assertEqual(waiting.result(timeout=20)[0], 504)


if __name__ == "__main__":
    unittest.main()
