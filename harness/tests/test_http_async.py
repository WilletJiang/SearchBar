#!/usr/bin/env python3
"""Concurrent transport checks against a threaded loopback server, without credentials."""
import collections
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def event(value):
    return b"data: " + json.dumps(value).encode() + b"\r\n\r\n"


class Server(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 32


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.counts = collections.Counter()
        self.errors = []
        self.overlap = threading.Barrier(8, timeout=2)
        self.children = threading.Barrier(2, timeout=3)
        self.cli_root = None
        self.cli_requests = []
        self.cli_counts = collections.Counter()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def handle(self):
        try:
            super().handle()
        except ConnectionResetError:
            pass

    def do_POST(self):
        try:
            self.respond()
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception:
            with self.server.state.lock:
                self.server.state.errors.append(traceback.format_exc(limit=3))
            self.close_connection = True

    def respond(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.path == "/cli":
            self.respond_cli(body)
            return
        tag = body["tag"]
        index = int(tag.removeprefix("request-"))
        route = f"route-{index}" if index % 2 == 0 else None
        expected = {"model": "test", "input": [], "stream": True, "tag": tag}
        if index % 2 == 0 or index % 4 == 3:
            expected["prompt_cache_key"] = route
        with self.server.state.lock:
            self.server.state.counts[self.path, tag] += 1
            attempt = self.server.state.counts[self.path, tag]
            if body != expected or self.headers.get("session-id") != route:
                self.server.state.errors.append(f"crossed request or route: {body!r}")
            if self.headers.get("Authorization") != "Bearer local-test-token":
                self.server.state.errors.append("crossed authorization")
        mode = self.path.removeprefix("/")
        if mode == "overlap":
            self.server.state.overlap.wait()
            time.sleep(0.18)
        elif index == 1:
            time.sleep(0.4 if mode == "deadline" else 0.09)
        response = {"id": tag, "status": "completed", "output": []}
        delta = event({"type": "response.output_text.delta", "delta": tag})
        terminal = event({"type": "response.completed", "response": response})
        payload = delta + terminal
        status = 200
        extra = 0
        slow = False
        if index == 0:
            if mode == "retry" and attempt == 1:
                status = 429
                payload = b'{"error":"retry"}'
            elif mode == "before-output" and attempt < 3:
                payload = event({"type": "response.created", "response": {"status": "in_progress"}})
                extra = 100
            elif mode == "malformed":
                payload = b"data: {bad-json}\n\n"
            elif mode == "after-delta":
                payload = delta
                extra = 100
            elif mode == "after-terminal":
                payload = terminal
                extra = 100
            elif mode in ("cancel", "release", "deadline"):
                slow = True
        self.send_response(status)
        self.send_header("Content-Type", "text/event-stream" if status == 200 else "application/json")
        self.send_header("Content-Length", str(len(payload) + extra))
        self.end_headers()
        if slow:
            if mode != "deadline":
                self.wfile.write(delta)
                self.wfile.flush()
                payload = terminal
            time.sleep(2.5 if mode == "deadline" else 0.4)
        self.wfile.write(payload)
        self.wfile.flush()
        if extra:
            self.close_connection = True

    def respond_cli(self, body):
        state = self.server.state
        session = body["prompt_cache_key"]
        assert self.headers.get("session-id") == session
        assert self.headers.get("Authorization") == "Bearer local-test-token"
        assert body["model"] == "cli-agents" and body["store"] is False and body["stream"] is True
        assert body["instructions"] == "inherited-cli-instructions"
        names = {tool["name"] for tool in body["tools"][0]["tools"]}
        assert names >= {"spawn", "send", "join", "stop", "agents"}
        with state.lock:
            state.cli_requests.append(body)
            state.cli_counts[session] += 1
            attempt = state.cli_counts[session]
        if session == state.cli_root:
            if attempt == 1:
                assert body["input"] == [{"role": "user", "content": "ROOT_PRIVATE_HISTORY"}]
                output = [self.function(f"spawn-{name}", "spawn",
                                       {"name": name, "message": f"cli-child-{name}"})
                          for name in ("alpha", "beta")]
            elif attempt == 2:
                receipts = {item["call_id"]: json.loads(item["output"])["result"]
                            for item in body["input"] if item.get("type") == "function_call_output"}
                assert all("id" in receipts[f"spawn-{name}"] for name in ("alpha", "beta")), receipts
                ids = [receipts[f"spawn-{name}"]["id"] for name in ("alpha", "beta")]
                assert len(set(ids + [session])) == 3
                output = [self.function("join-children", "join", {"ids": ids, "timeout_ms": 3000})]
            else:
                assert any(item.get("call_id") == "join-children" for item in body["input"])
                output = [self.message(f"cli-tree-done-{attempt}")]
        else:
            inputs = body["input"]
            first = inputs[0]
            name = first["content"].rsplit("cli-child-", 1)[-1]
            assert name in ("alpha", "beta")
            expected = {"role": "user", "content": (
                f"Task from agent root ({state.cli_root}). Follow the existing host instructions "
                f"and granted tool permissions.\n\ncli-child-{name}")}
            assert first == expected, (first, expected)
            assert "ROOT_PRIVATE_HISTORY" not in json.dumps(inputs)
            if attempt == 1:
                assert inputs == [expected], inputs
                state.children.wait()
                time.sleep(0.05)
            else:
                assert attempt == 2 and name == "alpha"
                assert inputs[-1]["content"].endswith("\n\ncli-follow-up")
            output = [self.message(f"cli-child-done-{name}-{attempt}")]
        payload = event({"type": "response.completed", "response": {
            "id": f"cli-{session}-{attempt}", "status": "completed", "output": output,
            "usage": {"input_tokens": 10, "output_tokens": 5, "total_tokens": 15}}})
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)
        self.wfile.flush()

    @staticmethod
    def function(call_id, name, arguments):
        return {"type": "function_call", "id": call_id, "call_id": call_id,
                "namespace": "local", "name": name, "arguments": json.dumps(arguments),
                "status": "completed"}

    @staticmethod
    def message(text):
        return {"type": "message", "id": text, "role": "assistant", "status": "completed",
                "content": [{"type": "output_text", "text": text, "annotations": []}]}


def run_cli(executable, endpoint, state):
    with tempfile.TemporaryDirectory(prefix="ah-async-cli-") as directory:
        base = ["--db", f"{directory}/state.db", "--endpoint", f"{endpoint}/cli",
                "--allow-http-loopback", "--agents", "4"]
        environment = dict(os.environ, AH_ACCESS_TOKEN="local-test-token")

        def invoke(command, *arguments):
            result = subprocess.run([executable, command, *base, *arguments], env=environment,
                                    capture_output=True, text=True, timeout=10)
            assert result.returncode == 0, (command, result.stdout, result.stderr, state.errors)
            assert "local-test-token" not in result.stdout + result.stderr
            return json.loads(result.stdout.splitlines()[-1])

        state.cli_root = invoke("new", "--model", "cli-agents", "--instructions",
                                "inherited-cli-instructions")["id"]
        arguments = ["--session", state.cli_root, "--prompt", "ROOT_PRIVATE_HISTORY",
                     "--command-id", "root-command"]
        assert invoke("run", *arguments)["state"] == "completed"
        tree = invoke("agents", "--session", state.cli_root)
        assert len(tree["agents"]) == 3 and all(a["state"] == "completed" for a in tree["agents"])
        assert tree["model_used"] == 5 and tree["tool_used"] == 3, tree
        assert len(state.cli_requests) == 5, state.cli_requests
        assert invoke("run", *arguments)["state"] == "completed"
        assert len(state.cli_requests) == 5, "duplicate command redispatched a model"
        alpha = next(agent["id"] for agent in tree["agents"] if agent["name"] == "alpha")
        invoke("send", "--session", state.cli_root, "--target", alpha, "--wake",
               "--prompt", "cli-follow-up", "--command-id", "child-follow-up")
        assert invoke("run", "--session", state.cli_root)["state"] == "completed"
        assert state.cli_counts[alpha] == 2 and state.cli_counts[state.cli_root] == 4
        assert len(state.cli_requests) == 7 and not state.errors, state.errors
        print("http_async CLI: fresh child contexts, overlapping requests, join, dedupe and follow-up passed")


def main():
    server = Server(("127.0.0.1", 0), Handler)
    server.state = State()
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    endpoint = f"http://127.0.0.1:{server.server_port}"

    def run(mode, first_code=0, attempts=1):
        completed = subprocess.run([sys.argv[1], f"{endpoint}/{mode}", mode],
                                   capture_output=True, text=True, check=True, timeout=8)
        value = json.loads(completed.stdout)
        requests = value["requests"]
        for i, request in enumerate(requests):
            code = first_code if i == 0 else 0
            assert request["code"] == code, (mode, value)
            assert server.state.counts[f"/{mode}", f"request-{i}"] == (attempts if i == 0 else 1)
            if code:
                assert request["response"] is None, (mode, value)
            else:
                assert request["response"] == {"id": f"request-{i}", "status": "completed", "output": []}
                assert request["delta"] == f"request-{i}", (mode, value)
        assert len(value["metrics"]) == len(requests) + attempts - 1, (mode, value)
        assert not server.state.errors, server.state.errors
        return value

    try:
        overlap = run("overlap")
        # The server barrier requires all eight requests before responding.
        for mode in ("cancel", "release"):
            value = run(mode, first_code=5)
            assert value["requests"][0]["delta"] == "request-0", value
        for mode, attempts in (("retry", 2), ("before-output", 3)):
            value = run(mode, attempts=attempts)
            assert sum(m["retry"] for m in value["metrics"]) == attempts - 1, value
        deadline = run("deadline", first_code=4)
        expired, surviving = deadline["requests"]
        first_deadline = expired["started"] + deadline["timeout_ms"]
        assert surviving["started"] > expired["started"], deadline
        assert expired["finished"] >= first_deadline, deadline
        # The later request succeeds after the first request's deadline. Finish
        # observations may share a poll, particularly on instrumented CI hosts.
        assert surviving["finished"] >= first_deadline, deadline
        run("malformed", first_code=3)
        run("after-delta", first_code=10)
        run("after-terminal", first_code=10)
        if len(sys.argv) > 2:
            run_cli(sys.argv[2], endpoint, server.state)
        print(f"http_async: eight requests overlap in {overlap['elapsed']} ms; isolated cancellation, "
              "release, retries, deadlines, routing and terminal validation passed")
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


if __name__ == "__main__":
    main()
