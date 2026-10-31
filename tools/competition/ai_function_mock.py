#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Deterministic AI-service mock for the seekdb ``ai_function`` mysqltest suite.

The AI functions exercised by ``test_suite/ai_function`` need a model provider.
This module speaks the four wire protocols they use, so the suite can run with
no network access and no real provider::

    POST /compatible-mode/v1/chat/completions   OpenAI chat completions
    POST /compatible-mode/v1/embeddings         OpenAI embeddings (768 dims)
    POST /api/generate                          Ollama completion
    POST /api/embed                             Ollama embeddings (768 dims)
    POST /v1/rerank                             SiliconFlow rerank

Everything is deterministic: the same request always produces the same
response, so ``.result`` files stay stable between runs.

Sub commands::

    ai_function_mock.py start       [PORT]   # start in background (pid + log)
    ai_function_mock.py stop        [PORT]   # stop the background server
    ai_function_mock.py serve       [PORT]   # run the server in the foreground
    ai_function_mock.py reset       [PORT]   # truncate the request log
    ai_function_mock.py dump-events [PORT]   # print the request log
    ai_function_mock.py selftest    [PORT]   # start, probe all endpoints, stop

The state directory is per port::

    %TEMP%/seekdb_ai_function_mock_<port>/          (Windows)
    $TMPDIR/seekdb_ai_function_mock_<port>/         (POSIX)

Override it with ``SEEKDB_AI_FUNCTION_MOCK_STATE_DIR``.  The bind address can
be overridden with ``SEEKDB_AI_FUNCTION_MOCK_HOST`` (default: try ``0.0.0.0``
so that both ``http://127.0.0.1:PORT`` and ``http://0.0.0.0:PORT`` work, then
fall back to ``127.0.0.1``).

Only the Python 3 standard library is used.
"""

from __future__ import annotations

import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

DEFAULT_PORT = 8000
#: `ai_embed` / `json_length(...)` assertions in the suite expect 768.
EMBEDDING_DIMENSION = 768
#: Truncate long request fields in events.jsonl / stdout.
MAX_LOGGED_TEXT = 240

CHAT_PATHS = ("/compatible-mode/v1/chat/completions",)
EMBEDDING_PATHS = ("/compatible-mode/v1/embeddings",)
GENERATE_PATHS = ("/api/generate",)
OLLAMA_EMBED_PATHS = ("/api/embed",)
RERANK_PATHS = ("/v1/rerank",)

CONTROL_HEALTH = "/__control/health"
CONTROL_SHUTDOWN = "/__control/shutdown"


# --------------------------------------------------------------------------- #
# state directory helpers
# --------------------------------------------------------------------------- #
def state_dir(port: int) -> Path:
    """Return the per-port state directory (created on demand)."""
    override = os.environ.get("SEEKDB_AI_FUNCTION_MOCK_STATE_DIR")
    if override:
        return Path(override)
    return Path(tempfile.gettempdir()) / ("seekdb_ai_function_mock_%d" % port)


def state_path(port: int, name: str) -> Path:
    return state_dir(port) / name


def read_events(port: int) -> list:
    """Read ``events.jsonl`` as a list of dicts (unparsable lines are kept raw)."""
    path = state_path(port, "events.jsonl")
    events = []
    if path.exists():
        with path.open("r", encoding="utf-8", errors="replace") as stream:
            for line in stream:
                line = line.strip()
                if not line:
                    continue
                try:
                    events.append(json.loads(line))
                except ValueError:
                    events.append({"raw": line})
    return events


# --------------------------------------------------------------------------- #
# deterministic payload helpers
# --------------------------------------------------------------------------- #
def _clip(value, limit: int = MAX_LOGGED_TEXT):
    """Shorten a value for logging without losing its shape."""
    if isinstance(value, str) and len(value) > limit:
        return value[:limit] + "...<len=%d>" % len(value)
    if isinstance(value, list):
        return [_clip(item, limit) for item in value[:8]] + (
            ["...<%d more>" % (len(value) - 8)] if len(value) > 8 else []
        )
    if isinstance(value, dict):
        return {key: _clip(item, limit) for key, item in value.items()}
    return value


def message_text(content) -> str:
    """Flatten an OpenAI ``content`` field (string or list of parts) to text."""
    if content is None:
        return ""
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts = []
        for part in content:
            if isinstance(part, dict):
                if isinstance(part.get("text"), str):
                    parts.append(part["text"])
                elif isinstance(part.get("content"), str):
                    parts.append(part["content"])
            elif isinstance(part, str):
                parts.append(part)
        return "".join(parts)
    if isinstance(content, dict):
        return json.dumps(content, ensure_ascii=False, sort_keys=True)
    return str(content)


def last_user_text(messages) -> str:
    """Text of the last ``role == "user"`` message (falls back to the last one)."""
    text = None
    if isinstance(messages, list):
        for message in messages:
            if isinstance(message, dict) and message.get("role") == "user":
                text = message_text(message.get("content"))
        if text is None and messages:
            last = messages[-1]
            text = message_text(last.get("content")) if isinstance(last, dict) else ""
    return text or ""


def input_texts(body) -> list:
    """Normalise an embeddings ``input`` (string / list / dict) to a list of str."""
    value = body.get("input")
    if value is None:
        value = body.get("prompt", "")
    if not isinstance(value, list):
        value = [value]
    texts = []
    for item in value:
        if isinstance(item, dict):
            texts.append(message_text(item.get("text", item)))
        elif isinstance(item, list):
            # pre-tokenised input: keep it stable and printable
            texts.append(" ".join(str(token) for token in item))
        else:
            texts.append("" if item is None else str(item))
    return texts


def embedding_vector(index: int) -> list:
    """Deterministic 768-dim vector: values cycle through 0.5 .. 3.5."""
    return [round(float(((index + element) % 7) + 1) * 0.5, 3)
            for element in range(EMBEDDING_DIMENSION)]


def document_text(document, doc_key=None) -> str:
    """Render a rerank document (string or object) as plain text."""
    if isinstance(document, str):
        return document
    if isinstance(document, dict):
        if doc_key and isinstance(document.get(doc_key), str):
            return document[doc_key]
        for key in ("text", "content", "document", "sentence", "passage"):
            value = document.get(key)
            if isinstance(value, str):
                return value
        return json.dumps(document, ensure_ascii=False, sort_keys=True)
    if document is None:
        return ""
    return str(document)


_QUERY_TOKEN_RE = re.compile(r"[0-9A-Za-z_\u00c0-\uffff]+")


def relevance_score(query: str, text: str) -> float:
    """Generic score: 1.0 substring hit, 0.5 any-query-word hit, else 0.1."""
    query = query or ""
    haystack = text.casefold()
    needle = query.casefold().strip()
    if needle and needle in haystack:
        return 1.0
    words = [word for word in _QUERY_TOKEN_RE.findall(needle) if word]
    if any(word in haystack for word in words):
        return 0.5
    return 0.1


# --------------------------------------------------------------------------- #
# HTTP server
# --------------------------------------------------------------------------- #
def _match(path: str, exact: tuple) -> bool:
    if path in exact:
        return True
    # tolerate a proxy prefix and the /v1 aliases the C++ client may build
    for candidate in exact:
        if path.endswith(candidate):
            return True
    return False


def serve(port: int, host: str = None) -> None:
    """Run the mock server in the foreground until it is stopped."""
    state_dir(port).mkdir(parents=True, exist_ok=True)
    log_path = state_path(port, "events.jsonl")
    lock = threading.Lock()
    ctx = {"server": None}

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_version = "seekdb-ai-function-mock/1.0"
        sys_version = ""

        # keep the test output clean; requests go to stdout/events.jsonl instead
        def log_message(self, fmt, *args):
            return

        # -- helpers -------------------------------------------------------
        def _record(self, event):
            event = dict(event)
            event["com"] = "seekdb_ai_function_mock"
            with lock:
                with log_path.open("a", encoding="utf-8") as stream:
                    stream.write(json.dumps(event, ensure_ascii=False,
                                            sort_keys=True) + "\n")
            detail = event.get("detail") or ""
            print("[mock] %s %s -> %s%s" % (
                event.get("method"), event.get("path"), event.get("status"),
                (" " + detail) if detail else ""), flush=True)

        def _send_json(self, status: int, payload, extra_headers=None):
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            for name, value in (extra_headers or {}).items():
                self.send_header(name, value)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)
            return body

        def _read_body(self):
            try:
                size = int(self.headers.get("Content-Length") or 0)
            except ValueError:
                size = 0
            raw = self.rfile.read(size) if size > 0 else b""
            return raw

        # -- routing -------------------------------------------------------
        def do_GET(self):
            self._dispatch()

        def do_HEAD(self):
            self._dispatch()

        def do_POST(self):
            self._dispatch()

        def do_PUT(self):
            self._dispatch()

        def do_DELETE(self):
            self._dispatch()

        def _dispatch(self):
            path = urlparse(self.path).path
            method = self.command

            if path == CONTROL_HEALTH:
                self._record({"method": method, "path": path, "status": 200,
                              "detail": "control/health"})
                self._send_json(200, {"status": "ok", "port": port,
                                      "dimension": EMBEDDING_DIMENSION})
                return
            if path == CONTROL_SHUTDOWN:
                self._record({"method": method, "path": path, "status": 200,
                              "detail": "control/shutdown"})
                self._send_json(200, {"status": "shutting down"})
                server = ctx.get("server")
                if server is not None:
                    threading.Thread(target=server.shutdown, daemon=True).start()
                return

            known = (CHAT_PATHS + EMBEDDING_PATHS + GENERATE_PATHS
                     + OLLAMA_EMBED_PATHS + RERANK_PATHS)
            is_known = _match(path, known)

            if not is_known:
                self._record({"method": method, "path": path, "status": 404,
                              "detail": "unknown path"})
                self._send_json(404, {"error": {"message": "not found",
                                                "path": path}})
                return
            if method != "POST":
                self._record({"method": method, "path": path, "status": 405,
                              "detail": "method not allowed"})
                self._send_json(405, {"error": {"message": "method not allowed"}},
                                {"Allow": "POST"})
                return

            raw = self._read_body()
            body, body_error = {}, None
            if raw:
                try:
                    body = json.loads(raw.decode("utf-8"))
                except (ValueError, UnicodeDecodeError) as exc:
                    body, body_error = {}, "invalid JSON body: %s" % exc
            if body_error is None and not isinstance(body, dict):
                body_error = "request body must be a JSON object"

            base_event = {"method": method, "path": path, "body_bytes": len(raw)}
            if body_error is not None:
                self._record(dict(base_event, status=400, detail=body_error))
                self._send_json(400, {"error": {"type": "invalid_request_error",
                                                "message": body_error}})
                return

            try:
                if _match(path, CHAT_PATHS):
                    status, payload, event = self._chat_completions(body)
                elif _match(path, EMBEDDING_PATHS):
                    status, payload, event = self._embeddings(body, ollama=False)
                elif _match(path, GENERATE_PATHS):
                    status, payload, event = self._generate(body)
                elif _match(path, OLLAMA_EMBED_PATHS):
                    status, payload, event = self._embeddings(body, ollama=True)
                else:
                    status, payload, event = self._rerank(body)
            except Exception as exc:  # never let a bug look like a client error
                status, payload, event = 500, {
                    "error": {"type": "mock_error", "message": repr(exc)}
                }, {"detail": "internal error: %r" % exc}

            self._record(dict(base_event, status=status, **event))
            self._send_json(status, payload)

        # -- protocol implementations --------------------------------------
        def _chat_completions(self, body):
            messages = body.get("messages")
            if not isinstance(messages, list):
                return 400, {"error": {"type": "invalid_request_error",
                                       "message": "missing required field 'messages'"}}, {
                    "detail": "missing 'messages'",
                    "model": body.get("model"),
                }
            model = body.get("model")
            temperature = body.get("temperature")
            max_tokens = body.get("max_tokens", body.get("max_completion_tokens"))
            user_text = last_user_text(messages)
            response = {
                "id": "mock",
                "object": "chat.completion",
                "model": model if isinstance(model, str) else "mock-model",
                "choices": [{
                    "index": 0,
                    "message": {"role": "assistant",
                                "content": "mock: " + user_text},
                    "finish_reason": "stop",
                }],
                "usage": {"prompt_tokens": 1, "completion_tokens": 1,
                          "total_tokens": 2},
            }
            event = {
                "model": model,
                "temperature": temperature,
                "max_tokens": max_tokens,
                "top_p": body.get("top_p"),
                "stream": body.get("stream"),
                "message_count": len(messages),
                "request_messages": _clip(messages),
                "user": _clip(user_text),
                "detail": "model=%r messages=%d temperature=%r max_tokens=%r" % (
                    model, len(messages), temperature, max_tokens),
            }
            return 200, response, event

        def _generate(self, body):
            model = body.get("model")
            prompt = body.get("prompt")
            prompt_text = message_text(prompt)
            response = {
                "model": model if isinstance(model, str) else "mock-model",
                "response": "mock: " + prompt_text,
                "done": True,
                "done_reason": "stop",
            }
            event = {
                "model": model,
                "prompt": _clip(prompt_text),
                "options": _clip(body.get("options")),
                "system": _clip(body.get("system")),
                "detail": "model=%r prompt_len=%d" % (model, len(prompt_text)),
            }
            return 200, response, event

        def _embeddings(self, body, ollama=False):
            texts = input_texts(body)
            model = body.get("model")
            vectors = [embedding_vector(index) for index in range(len(texts))]
            event = {
                "model": model,
                "inputs": _clip(texts),
                "dimension": EMBEDDING_DIMENSION,
                "vector_count": len(vectors),
                "detail": "model=%r inputs=%d dim=%d" % (
                    model, len(texts), EMBEDDING_DIMENSION),
            }
            if ollama:
                return 200, {
                    "model": model if isinstance(model, str) else "mock-model",
                    "embeddings": vectors,
                    "total_duration": 1,
                    "prompt_eval_count": 1,
                }, event
            tokens = sum(max(len(text), 1) for text in texts)
            return 200, {
                "object": "list",
                "data": [{"object": "embedding",
                          "index": index,
                          "embedding": vector}
                         for index, vector in enumerate(vectors)],
                "model": model if isinstance(model, str) else "mock-model",
                "usage": {"prompt_tokens": tokens, "total_tokens": tokens},
            }, event

        def _rerank(self, body):
            query = body.get("query")
            if not isinstance(query, str):
                query = message_text(query)
            documents = body.get("documents")
            if isinstance(documents, str):
                # tolerate clients that forward the JSON text unchanged
                try:
                    decoded = json.loads(documents)
                except ValueError:
                    decoded = None
                if isinstance(decoded, list):
                    documents = decoded
            if not isinstance(documents, list):
                return 400, {"error": {"type": "invalid_request_error",
                                       "message": "missing required field 'documents'"}}, {
                    "detail": "missing 'documents'",
                    "model": body.get("model"),
                }
            doc_key = body.get("doc_key") or body.get("document_key")
            texts = [document_text(document, doc_key) for document in documents]
            scored = []
            for index, text in enumerate(texts):
                scored.append((relevance_score(query, text), index, text))
            scored.sort(key=lambda item: (-item[0], item[1]))
            top_n = body.get("top_n")
            if isinstance(top_n, int) and 0 < top_n < len(scored):
                scored = scored[:top_n]
            response = {
                "id": "mock",
                "results": [{
                    "index": index,
                    "relevance_score": score,
                    "document": {"text": text},
                } for score, index, text in scored],
                "meta": {
                    "tokens": {"input_tokens": len(query) + sum(len(t) for t in texts),
                               "output_tokens": 0},
                    "billed_units": {"search_units": 1},
                },
            }
            event = {
                "model": body.get("model"),
                "query": _clip(query),
                "documents": _clip(texts),
                "doc_key": doc_key,
                "top_n": top_n,
                "scores": [score for score, _, _ in scored],
                "detail": "model=%r query=%r documents=%d scores=%s" % (
                    body.get("model"), _clip(query, 60), len(texts),
                    [score for score, _, _ in scored]),
            }
            return 200, response, event

    bind_error = None
    candidates = []
    for candidate in (host, "0.0.0.0", "127.0.0.1"):
        if candidate and candidate not in candidates:
            candidates.append(candidate)

    httpd = None
    for candidate in candidates:
        try:
            httpd = ThreadingHTTPServer((candidate, port), Handler)
            break
        except OSError as exc:  # address in use / not permitted
            bind_error = exc
    if httpd is None:
        raise SystemExit("cannot bind port %d: %s" % (port, bind_error))

    httpd.daemon_threads = True
    ctx["server"] = httpd
    bound_host = httpd.server_address[0]
    print("[mock] listening on %s:%d (events: %s)" % (bound_host, port, log_path),
          flush=True)
    try:
        httpd.serve_forever(poll_interval=0.1)
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
        print("[mock] stopped", flush=True)


# --------------------------------------------------------------------------- #
# process control
# --------------------------------------------------------------------------- #
def port_open(port: int, host: str = "127.0.0.1", timeout: float = 0.2) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def _read_pid(port: int):
    pid_path = state_path(port, "server.pid")
    try:
        return int(pid_path.read_text(encoding="ascii").strip())
    except (OSError, ValueError):
        return None


def _http_get(url: str, timeout: float = 1.0):
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return response.read()


def stop_server(port: int, quiet: bool = False) -> None:
    """Stop a server started by ``start`` (graceful, then SIGTERM)."""
    pid = _read_pid(port)
    if pid is None and not port_open(port):
        if not quiet:
            print("[mock] no server recorded for port %d" % port)
        return

    stopped = False
    try:
        request = urllib.request.Request(
            "http://127.0.0.1:%d%s" % (port, CONTROL_SHUTDOWN),
            data=b"", method="POST")
        urllib.request.urlopen(request, timeout=2.0).read()
        stopped = True
    except Exception:
        stopped = False

    if not stopped and pid is not None:
        try:
            os.kill(pid, signal.SIGTERM)
            stopped = True
        except OSError:
            stopped = False

    for _ in range(150):
        if not port_open(port):
            break
        time.sleep(0.02)

    try:
        state_path(port, "server.pid").unlink()
    except OSError:
        pass
    if not quiet:
        print("[mock] server on port %d stopped%s"
              % (port, "" if stopped else " (nothing to stop)"))


def start_server(port: int, host: str = None, quiet: bool = False) -> None:
    """Start the mock server in the background and wait until it answers."""
    state_dir(port).mkdir(parents=True, exist_ok=True)
    stop_server(port, quiet=True)
    for name in ("events.jsonl", "server.out"):
        state_path(port, name).write_bytes(b"")

    output = state_path(port, "server.out").open("a", encoding="utf-8")
    command = [sys.executable, os.path.abspath(__file__), "serve", str(port)]
    if host:
        command += ["--host", host]
    kwargs = {}
    if os.name == "nt":
        kwargs["creationflags"] = (subprocess.CREATE_NEW_PROCESS_GROUP
                                   | subprocess.DETACHED_PROCESS)
    else:
        kwargs["start_new_session"] = True
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                               stdout=output, stderr=subprocess.STDOUT, **kwargs)
    state_path(port, "server.pid").write_text(str(process.pid), encoding="ascii")

    deadline = time.time() + 10.0
    while time.time() < deadline:
        if port_open(port):
            try:
                _http_get("http://127.0.0.1:%d%s" % (port, CONTROL_HEALTH), 1.0)
            except Exception:
                pass
            if not quiet:
                print("[mock] started on 127.0.0.1:%d (pid %d, log %s)"
                      % (port, process.pid, state_path(port, "server.out")))
            return
        if process.poll() is not None:
            break
        time.sleep(0.02)
    raise RuntimeError("AI function mock did not start on port %d (see %s)"
                       % (port, state_path(port, "server.out")))


# --------------------------------------------------------------------------- #
# self test
# --------------------------------------------------------------------------- #
def _post(port: int, path: str, payload):
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        "http://127.0.0.1:%d%s" % (port, path), data=body, method="POST",
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.status, json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        return exc.code, json.loads(exc.read().decode("utf-8") or "{}")


def summarize(port: int) -> int:
    """Print which endpoints were hit, with counts and status codes."""
    events = read_events(port)
    if not events:
        print("[mock] no requests recorded for port %d" % port, flush=True)
        return 1
    print("[mock] %d request(s) recorded for port %d" % (len(events), port),
          flush=True)
    buckets = {}
    models = set()
    failures = {}
    for event in events:
        path = str(event.get("path", "<unknown>"))
        status = event.get("status")
        key = (path, status)
        buckets[key] = buckets.get(key, 0) + 1
        model = event.get("model")
        if isinstance(model, str) and model:
            models.add(model)
        if isinstance(status, int) and status >= 400:
            failures[status] = failures.get(status, 0) + 1
    for (path, status), count in sorted(buckets.items(),
                                        key=lambda item: (str(item[0][0]),
                                                          str(item[0][1]))):
        print("  %-46s %3s x%d" % (path, status, count), flush=True)
    if models:
        print("[mock] model(s) seen: %s" % ", ".join(sorted(models)), flush=True)
    total_failures = sum(failures.values())
    if total_failures:
        print("[mock] %d non-2xx response(s): %s"
              % (total_failures,
                 ", ".join("%dx%s" % (count, status)
                           for status, count in sorted(failures.items()))),
              flush=True)
    else:
        print("[mock] no 4xx/5xx responses", flush=True)
    return 0


def selftest(port: int, host: str = None, keep_running: bool = False) -> int:
    """Start the mock, probe every endpoint, verify shapes, then stop it."""
    started_here = not port_open(port)
    if started_here:
        start_server(port, host=host, quiet=True)
    failures = []

    def check(name, condition, detail=""):
        status = "ok" if condition else "FAIL"
        print("  %-34s %s %s" % (name, status, detail), flush=True)
        if not condition:
            failures.append(name)

    try:
        print("[selftest] port %d" % port, flush=True)

        code, body = _post(port, "/compatible-mode/v1/chat/completions", {
            "model": "qwen3-32b",
            "messages": [{"role": "system", "content": "be brief"},
                         {"role": "user", "content": "hello world"}],
            "temperature": 0.2, "max_tokens": 16})
        content = ((body.get("choices") or [{}])[0]
                   .get("message", {}).get("content"))
        check("chat status", code == 200, code)
        check("chat shape", body.get("id") == "mock"
              and body.get("object") == "chat.completion"
              and body.get("usage", {}).get("total_tokens") == 2, repr(_clip(body)))
        check("chat content", content == "mock: hello world", repr(content))

        code, body = _post(port, "/compatible-mode/v1/embeddings", {
            "model": "text-embedding-v1", "input": ["first", "second"]})
        data = body.get("data") or []
        check("openai embed status", code == 200, code)
        check("openai embed count", len(data) == 2, len(data))
        check("openai embed dim", all(len(item.get("embedding") or []) == 768
                                      for item in data),
              [len(item.get("embedding") or []) for item in data])
        check("openai embed indices", [item.get("index") for item in data] == [0, 1],
              [item.get("index") for item in data])

        code, body = _post(port, "/api/generate",
                           {"model": "qwen2.5", "prompt": "hi", "stream": False})
        check("ollama generate", code == 200 and body.get("response") == "mock: hi"
              and body.get("done") is True, repr(_clip(body)))

        code, body = _post(port, "/api/embed",
                           {"model": "nomic-embed-text", "input": "hello"})
        vectors = body.get("embeddings") or []
        check("ollama embed", code == 200 and len(vectors) == 1
              and len(vectors[0]) == 768, [len(v) for v in vectors])

        code, body = _post(port, "/v1/rerank", {
            "model": "bge-reranker", "query": "apple",
            "documents": ["apple", "banana", "apple pie", "fruit"]})
        results = body.get("results") or []
        scores = [item.get("relevance_score") for item in results]
        check("rerank status", code == 200, code)
        check("rerank count", len(results) == 4, len(results))
        check("rerank sorted", scores == sorted(scores, reverse=True), scores)
        check("rerank best is apple", results
              and results[0].get("index") == 0
              and results[0].get("relevance_score") == 1.0, repr(_clip(results[:1])))
        check("rerank tail low", results
              and results[-1].get("index") == 3
              and results[-1].get("relevance_score") == 0.1, repr(_clip(results[-1:])))

        code, body = _post(port, "/v1/rerank", {
            "query": "apple",
            "documents": [{"id": "a", "text": "apple"},
                          {"id": "b", "text": "banana"}]})
        results = body.get("results") or []
        check("rerank objects", [item.get("index") for item in results] == [0, 1]
              and results[0].get("relevance_score") == 1.0
              and results[1].get("relevance_score") == 0.1, repr(_clip(results)))

        code, _ = _post(port, "/compatible-mode/v1/chat/completions", {})
        check("chat missing messages -> 400", code == 400, code)
        try:
            request = urllib.request.Request(
                "http://127.0.0.1:%d/compatible-mode/v1/chat/completions" % port,
                data=b"{not json", method="POST",
                headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(request, timeout=5) as response:
                code = response.status
        except urllib.error.HTTPError as exc:
            code = exc.code
        check("chat bad json -> 400", code == 400, code)

        try:
            with urllib.request.urlopen(
                    "http://127.0.0.1:%d/no/such/path" % port, timeout=5) as response:
                code = response.status
        except urllib.error.HTTPError as exc:
            code = exc.code
        check("unknown path -> 404", code == 404, code)

        events = read_events(port)
        check("events recorded", len(events) >= 6, len(events))
        check("events have model+detail", all("detail" in event for event in events),
              "%d events" % len(events))
    finally:
        if started_here and not keep_running:
            stop_server(port, quiet=True)

    if failures:
        print("[selftest] FAILED: %s" % ", ".join(failures), flush=True)
        return 1
    print("[selftest] PASS", flush=True)
    return 0


# --------------------------------------------------------------------------- #
# entry point
# --------------------------------------------------------------------------- #
def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    host = None
    if "--host" in argv:
        index = argv.index("--host")
        host = argv[index + 1]
        del argv[index:index + 2]
    keep_running = "--keep-running" in argv
    if keep_running:
        argv.remove("--keep-running")

    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__)
        return 0 if argv else 2

    mode = argv[0]
    port = int(argv[1]) if len(argv) > 1 else DEFAULT_PORT

    if mode == "start":
        start_server(port, host=host)
    elif mode == "stop":
        stop_server(port)
    elif mode == "serve":
        serve(port, host=host)
    elif mode == "reset":
        state_dir(port).mkdir(parents=True, exist_ok=True)
        state_path(port, "events.jsonl").write_bytes(b"")
        print("[mock] cleared events for port %d" % port)
    elif mode == "dump-events":
        events = read_events(port)
        for event in events:
            print(json.dumps(event, ensure_ascii=False, sort_keys=True))
        print("[mock] %d event(s) for port %d" % (len(events), port))
    elif mode == "selftest":
        return selftest(port, host=host, keep_running=keep_running)
    elif mode == "summary":
        return summarize(port)
    else:
        raise SystemExit("unknown mode %r (use --help)" % mode)
    return 0


if __name__ == "__main__":
    sys.exit(main())