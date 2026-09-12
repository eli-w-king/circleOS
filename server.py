from __future__ import annotations

import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

import httpx
from dotenv import load_dotenv


HOST = "127.0.0.1"
PORT = 3000
ORIGIN = f"http://localhost:{PORT}"
LIVE_SESSIONS_URL = "https://api.openai.com/v1/live/sessions"
ROOT = Path(__file__).resolve().parent
STATIC_FILES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/app.js": ("app.js", "text/javascript; charset=utf-8"),
    "/styles.css": ("styles.css", "text/css; charset=utf-8"),
    "/dial": ("dial.html", "text/html; charset=utf-8"),
    "/dial.html": ("dial.html", "text/html; charset=utf-8"),
    "/dial-smooth.css": ("dial-smooth.css", "text/css; charset=utf-8"),
    "/dial-scroll.js": ("dial-scroll.js", "text/javascript; charset=utf-8"),
}

load_dotenv(ROOT / ".env")


class SessionHandler(BaseHTTPRequestHandler):
    server_version = "circleOSVoice/0.1"

    def reply(
        self,
        status: int,
        body: bytes,
        content_type: str = "application/json; charset=utf-8",
    ) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def reply_json(self, status: int, payload: dict[str, Any]) -> None:
        self.reply(status, json.dumps(payload).encode("utf-8"))

    def do_GET(self) -> None:
        static_file = STATIC_FILES.get(self.path)
        if static_file is None:
            self.reply_json(404, {"error": "Not found"})
            return

        filename, content_type = static_file
        try:
            body = (ROOT / filename).read_bytes()
        except OSError as error:
            print(f"Unable to read {filename}: {error}")
            self.reply_json(500, {"error": "Application file unavailable"})
            return
        self.reply(200, body, content_type)

    def do_POST(self) -> None:
        if self.path != "/api/session":
            self.reply_json(404, {"error": "Not found"})
            return
        if self.headers.get("Origin") != ORIGIN:
            self.reply_json(403, {"error": "Unexpected request origin"})
            return
        if self.headers.get_content_type() != "application/json":
            self.reply_json(415, {"error": "Expected application/json"})
            return

        try:
            content_length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.reply_json(400, {"error": "Invalid Content-Length"})
            return
        if not 0 < content_length <= 65_536:
            self.reply_json(400, {"error": "An SDP offer is required"})
            return

        try:
            payload = json.loads(self.rfile.read(content_length))
            sdp = payload.get("sdp") if isinstance(payload, dict) else None
            if not isinstance(sdp, str) or not sdp.strip():
                raise ValueError("Empty SDP offer")
        except (json.JSONDecodeError, UnicodeDecodeError, ValueError):
            self.reply_json(400, {"error": "An SDP offer is required"})
            return

        api_key = os.environ.get("OPENAI_API_KEY")
        if not api_key:
            self.reply_json(
                503,
                {"error": "Set OPENAI_API_KEY in the server environment"},
            )
            return

        session = {
            "model": "gpt-live-1",
            "instructions": (
                "You are a warm, concise voice assistant. Respond naturally and "
                "avoid long monologues. Let the user interrupt you."
            ),
            "audio": {"output": {"voice": "marin"}},
            "delegation": {
                "type": "responses",
                "responses": {
                    "model": "gpt-5.6-luna",
                    "instructions": (
                        "Answer conversational requests accurately and concisely. "
                        "Return plain facts suitable for a spoken response."
                    ),
                },
            },
        }

        try:
            result = httpx.post(
                LIVE_SESSIONS_URL,
                headers={
                    "Authorization": f"Bearer {api_key}",
                    "Content-Type": "application/json",
                },
                json={
                    "session": session,
                    "transport": {"type": "webrtc", "sdp": sdp},
                },
                timeout=30,
            )
        except httpx.HTTPError as error:
            print(f"Live session request failed: {error}")
            self.reply_json(502, {"error": "Unable to reach OpenAI"})
            return

        if result.status_code != 201:
            print(f"Live session creation failed with HTTP {result.status_code}")
            self.reply_json(
                result.status_code,
                {
                    "error": (
                        "OpenAI rejected the Live session request "
                        f"(HTTP {result.status_code})"
                    )
                },
            )
            return

        try:
            response_body = result.json()
            response_sdp = response_body["transport"]["sdp"]
            session_id = response_body["session"]["id"]
            if not isinstance(response_sdp, str) or not isinstance(session_id, str):
                raise ValueError("Invalid Live session response")
        except (json.JSONDecodeError, KeyError, TypeError, ValueError):
            print("OpenAI returned an invalid Live session response")
            self.reply_json(502, {"error": "Invalid response from OpenAI"})
            return

        self.reply_json(201, response_body)

    def log_message(self, format: str, *args: object) -> None:
        print(f"{self.address_string()} - {format % args}")


if __name__ == "__main__":
    print(f"Open {ORIGIN}")
    print("Press Control-C to stop.")
    try:
        ThreadingHTTPServer((HOST, PORT), SessionHandler).serve_forever()
    except KeyboardInterrupt:
        print("\nServer stopped.")
