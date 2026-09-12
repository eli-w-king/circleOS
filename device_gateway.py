from __future__ import annotations

import argparse
import asyncio
import base64
import binascii
import json
import math
import os
import threading
import time
from array import array
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

from dotenv import load_dotenv
from websockets.asyncio.client import ClientConnection, connect
from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed

ROOT = Path(__file__).resolve().parent
SAMPLE_RATE = 16000
AUDIO_FRAME_BYTES = 640
LIVE_SESSIONS_URL = "wss://api.openai.com/v1/live/sessions"

load_dotenv(ROOT / ".env")


@dataclass
class AudioStats:
    frames: int = 0
    samples: int = 0
    started_at: float = 0.0
    last_report_at: float = 0.0


@dataclass
class DeviceStatus:
    connected: bool = False
    device_id: str = ""
    remote_address: str = ""
    rms_dbfs: float = -96.0
    peak: float = 0.0
    sample_rate: float = 0.0
    frames: int = 0
    samples: int = 0
    live_state: str = "ready"
    input_transcript: str = ""
    output_transcript: str = ""
    output_samples: int = 0
    live_error: str = ""
    updated_at: float = 0.0
    connection_key: int = 0


@dataclass
class LiveControl:
    requested: bool = False


device_status = DeviceStatus()
status_lock = threading.Lock()
live_control = LiveControl()
control_lock = threading.Lock()


def update_status(**values: Any) -> None:
    with status_lock:
        for name, value in values.items():
            setattr(device_status, name, value)


def status_payload() -> dict[str, Any]:
    with status_lock, control_lock:
        payload = {
            "connected": device_status.connected,
            "device_id": device_status.device_id,
            "remote_address": device_status.remote_address,
            "rms_dbfs": round(device_status.rms_dbfs, 1),
            "peak": round(device_status.peak, 3),
            "sample_rate": round(device_status.sample_rate),
            "frames": device_status.frames,
            "audio_seconds": round(device_status.samples / SAMPLE_RATE, 1),
            "live_requested": live_control.requested,
            "live_state": device_status.live_state,
            "input_transcript": device_status.input_transcript,
            "output_transcript": device_status.output_transcript,
            "output_audio_seconds": round(
                device_status.output_samples / SAMPLE_RATE,
                1,
            ),
            "live_error": device_status.live_error,
            "updated_at": device_status.updated_at,
        }
    return payload


def set_live_requested(requested: bool) -> None:
    with control_lock:
        live_control.requested = requested


def is_live_requested() -> bool:
    with control_lock:
        return live_control.requested


class DashboardHandler(BaseHTTPRequestHandler):
    server_version = "circleOSDeviceGateway/0.1"

    def reply(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path == "/api/status":
            body = json.dumps(status_payload()).encode("utf-8")
            self.reply(200, body, "application/json; charset=utf-8")
            return

        files = {
            "/": ("device_dashboard.html", "text/html; charset=utf-8"),
            "/device-dashboard.js": (
                "device_dashboard.js",
                "text/javascript; charset=utf-8",
            ),
        }
        requested = files.get(self.path)
        if requested is None:
            self.reply(404, b'{"error":"Not found"}', "application/json; charset=utf-8")
            return

        filename, content_type = requested
        try:
            body = (ROOT / filename).read_bytes()
        except OSError as error:
            print(f"Unable to read dashboard file {filename}: {error}")
            self.reply(
                500,
                b'{"error":"Dashboard file unavailable"}',
                "application/json; charset=utf-8",
            )
            return
        self.reply(200, body, content_type)

    def do_POST(self) -> None:
        expected_origins = {
            f"http://localhost:{self.server.server_port}",
            f"http://127.0.0.1:{self.server.server_port}",
        }
        if self.headers.get("Origin") not in expected_origins:
            self.reply(
                403,
                b'{"error":"Unexpected request origin"}',
                "application/json; charset=utf-8",
            )
            return

        if self.path == "/api/live/start":
            if not os.environ.get("OPENAI_API_KEY"):
                self.reply(
                    503,
                    b'{"error":"OPENAI_API_KEY is not configured"}',
                    "application/json; charset=utf-8",
                )
                return
            if not status_payload()["connected"]:
                self.reply(
                    409,
                    b'{"error":"The circleOS device is not connected"}',
                    "application/json; charset=utf-8",
                )
                return
            set_live_requested(True)
        elif self.path == "/api/live/stop":
            set_live_requested(False)
        else:
            self.reply(
                404,
                b'{"error":"Not found"}',
                "application/json; charset=utf-8",
            )
            return

        self.reply(
            202,
            json.dumps(status_payload()).encode("utf-8"),
            "application/json; charset=utf-8",
        )

    def log_message(self, format: str, *args: object) -> None:
        return


class LiveSession:
    def __init__(
        self,
        device: ServerConnection,
        device_send_lock: asyncio.Lock,
        api_key: str,
    ) -> None:
        self.device = device
        self.device_send_lock = device_send_lock
        self.api_key = api_key
        self.connection: ClientConnection | None = None
        self.receiver: asyncio.Task[None] | None = None
        self.started = asyncio.Event()
        self.closed = asyncio.Event()
        self.closing = False
        self.pending_output = bytearray()
        self.device_phase = "ready"

    async def send_device_json(self, payload: dict[str, Any]) -> None:
        async with self.device_send_lock:
            await self.device.send(json.dumps(payload))

    async def set_device_phase(self, phase: str) -> None:
        if phase == self.device_phase:
            return
        self.device_phase = phase
        await self.send_device_json({"type": "live", "state": phase})

    async def start(self) -> None:
        update_status(
            live_state="connecting",
            live_error="",
            input_transcript="",
            output_transcript="",
            output_samples=0,
        )
        await self.set_device_phase("connecting")
        self.connection = await connect(
            LIVE_SESSIONS_URL,
            additional_headers={"Authorization": f"Bearer {self.api_key}"},
            compression=None,
            max_size=4 * 1024 * 1024,
            ping_interval=20,
            ping_timeout=10,
        )
        self.receiver = asyncio.create_task(
            self.receive_events(),
            name="openai-live-receiver",
        )
        await self.connection.send(
            json.dumps(
                {
                    "type": "session.start",
                    "event_id": "circleos_session_start",
                    "session": {
                        "model": "gpt-live-1",
                        "instructions": (
                            "You are the concise, friendly voice of a small "
                            "circleOS device. Respond naturally in one or two "
                            "sentences when possible. Let the user interrupt."
                        ),
                        "audio": {
                            "format": {"type": "audio/pcm", "rate": SAMPLE_RATE},
                            "output": {"voice": "stone"},
                        },
                        "delegation": {
                            "type": "responses",
                            "responses": {
                                "model": "gpt-5.6-luna",
                                "instructions": (
                                    "Answer accurately and concisely for a "
                                    "spoken conversation."
                                ),
                            },
                        },
                    },
                }
            )
        )
        try:
            await asyncio.wait_for(self.started.wait(), timeout=20)
        except asyncio.TimeoutError as error:
            raise RuntimeError("GPT-Live did not start within 20 seconds") from error

    async def receive_events(self) -> None:
        assert self.connection is not None
        try:
            async for message in self.connection:
                if not isinstance(message, str):
                    raise RuntimeError("GPT-Live sent an unexpected binary message")
                try:
                    event = json.loads(message)
                except json.JSONDecodeError as error:
                    raise RuntimeError("GPT-Live sent invalid JSON") from error

                event_type = event.get("type")
                if event_type == "session.started":
                    session = event.get("session", {})
                    print(f"GPT-Live session ready: {session.get('id', 'unknown')}")
                    update_status(live_state="active")
                    self.started.set()
                    await self.set_device_phase("active")
                elif event_type in {
                    "session.input_audio.speech_stopped",
                    "session.input_transcript.done",
                    "session.response.created",
                    "session.response.started",
                    "session.delegation.started",
                }:
                    await self.set_device_phase("processing")
                elif event_type == "session.input_transcript.delta":
                    delta = event.get("delta")
                    if isinstance(delta, str):
                        with status_lock:
                            device_status.input_transcript = (
                                device_status.input_transcript + delta
                            )[-1000:]
                elif event_type == "session.output_transcript.delta":
                    delta = event.get("delta")
                    if isinstance(delta, str):
                        with status_lock:
                            device_status.output_transcript = (
                                device_status.output_transcript + delta
                            )[-1000:]
                elif event_type == "session.output_audio.delta":
                    await self.set_device_phase("active")
                    delta = event.get("delta")
                    if not isinstance(delta, str):
                        raise RuntimeError("GPT-Live audio event has no delta")
                    try:
                        decoded = base64.b64decode(delta, validate=True)
                    except (binascii.Error, ValueError) as error:
                        raise RuntimeError("GPT-Live sent invalid base64 audio") from error
                    if len(decoded) % 2:
                        raise RuntimeError("GPT-Live sent an incomplete PCM16 sample")
                    self.pending_output.extend(decoded)
                    while len(self.pending_output) >= AUDIO_FRAME_BYTES:
                        frame = bytes(self.pending_output[:AUDIO_FRAME_BYTES])
                        del self.pending_output[:AUDIO_FRAME_BYTES]
                        async with self.device_send_lock:
                            await self.device.send(frame)
                        with status_lock:
                            device_status.output_samples += AUDIO_FRAME_BYTES // 2
                elif event_type == "session.closed":
                    usage = event.get("usage")
                    print(f"GPT-Live session closed; usage: {usage}")
                    self.closed.set()
                    return
                elif event_type == "error":
                    error = event.get("error", {})
                    message_text = (
                        error.get("message")
                        if isinstance(error, dict)
                        else "Unknown GPT-Live error"
                    )
                    raise RuntimeError(f"GPT-Live error: {message_text}")
        except asyncio.CancelledError:
            raise
        except Exception as error:
            if not self.closing:
                print(f"GPT-Live receiver failed: {error}")
                update_status(live_state="error", live_error=str(error))
                set_live_requested(False)
                try:
                    await self.set_device_phase("failed")
                except ConnectionClosed:
                    pass
        finally:
            self.closed.set()

    async def send_audio(self, payload: bytes) -> None:
        if not self.started.is_set() or self.connection is None:
            return
        await self.connection.send(
            json.dumps(
                {
                    "type": "session.input_audio.append",
                    "audio": base64.b64encode(payload).decode("ascii"),
                }
            )
        )

    async def close(self) -> None:
        self.closing = True
        if self.connection is not None and self.started.is_set():
            try:
                await self.connection.send(json.dumps({"type": "session.close"}))
                await asyncio.wait_for(self.closed.wait(), timeout=15)
            except (asyncio.TimeoutError, ConnectionClosed):
                print("GPT-Live session closed without final usage confirmation")

        if self.receiver is not None and not self.receiver.done():
            self.receiver.cancel()
            await asyncio.gather(self.receiver, return_exceptions=True)
        if self.connection is not None:
            await self.connection.close()

        with status_lock:
            session_failed = device_status.live_state == "error"
            if not session_failed:
                device_status.live_state = "ready"
        if not session_failed:
            try:
                await self.set_device_phase("ready")
            except ConnectionClosed:
                pass


def audio_levels(payload: bytes) -> tuple[float, float, int]:
    if not payload or len(payload) % 2:
        raise ValueError("PCM16 payload must contain complete samples")

    samples = array("h")
    samples.frombytes(payload)
    sample_count = len(samples)
    square_sum = sum(sample * sample for sample in samples)
    peak = max(abs(sample) for sample in samples)
    rms = math.sqrt(square_sum / sample_count) / 32768.0
    peak_normalized = peak / 32768.0
    rms_dbfs = 20.0 * math.log10(max(rms, 1.0 / 32768.0))
    return rms_dbfs, peak_normalized, sample_count


async def handle_device(connection: ServerConnection) -> None:
    if connection.request.path != "/device":
        await connection.close(code=1008, reason="Unsupported path")
        return

    remote = connection.remote_address
    print(f"Device connection from {remote}")
    try:
        first_message = await asyncio.wait_for(connection.recv(), timeout=10)
    except asyncio.TimeoutError:
        await connection.close(code=1008, reason="Hello timeout")
        print(f"Closed {remote}: hello timeout")
        return

    if not isinstance(first_message, str):
        await connection.close(code=1003, reason="Expected JSON hello")
        print(f"Closed {remote}: first message was not text")
        return

    try:
        hello = json.loads(first_message)
    except json.JSONDecodeError:
        await connection.close(code=1007, reason="Invalid JSON hello")
        print(f"Closed {remote}: invalid hello JSON")
        return

    expected = {
        "type": "hello",
        "format": "pcm_s16le",
        "sample_rate": 16000,
        "channels": 1,
    }
    if not isinstance(hello, dict) or any(
        hello.get(key) != value for key, value in expected.items()
    ):
        await connection.close(code=1008, reason="Unsupported audio format")
        print(f"Closed {remote}: unsupported hello")
        return

    device_id = str(hello.get("device_id", "unknown"))
    connection_key = id(connection)
    device_send_lock = asyncio.Lock()
    live_session: LiveSession | None = None
    update_status(
        connected=True,
        device_id=device_id,
        remote_address=str(remote),
        rms_dbfs=-96.0,
        peak=0.0,
        sample_rate=0.0,
        frames=0,
        samples=0,
        live_state="ready",
        live_error="",
        input_transcript="",
        output_transcript="",
        output_samples=0,
        updated_at=time.time(),
        connection_key=connection_key,
    )
    async with device_send_lock:
        await connection.send(json.dumps({"type": "ready", "live_state": "ready"}))
        await connection.send(json.dumps({"type": "live", "state": "ready"}))
    print(f"Audio stream ready for {device_id}; tap the device orb to start")

    now = time.monotonic()
    stats = AudioStats(started_at=now, last_report_at=now)
    try:
        async for message in connection:
            if isinstance(message, str):
                try:
                    control_message = json.loads(message)
                except json.JSONDecodeError:
                    await connection.close(code=1007, reason="Invalid control JSON")
                    return
                if (
                    not isinstance(control_message, dict)
                    or control_message.get("type") != "toggle_live"
                ):
                    await connection.close(code=1008, reason="Unsupported control message")
                    return
                set_live_requested(not is_live_requested())
                continue
            if len(message) > 4096:
                await connection.close(code=1009, reason="Audio frame too large")
                return

            try:
                rms_dbfs, peak, sample_count = audio_levels(message)
            except ValueError as error:
                await connection.close(code=1007, reason=str(error))
                return

            stats.frames += 1
            stats.samples += sample_count
            now = time.monotonic()
            elapsed = now - stats.started_at
            sample_rate = stats.samples / elapsed if elapsed else 0.0
            update_status(
                rms_dbfs=rms_dbfs,
                peak=peak,
                sample_rate=sample_rate,
                frames=stats.frames,
                samples=stats.samples,
                updated_at=time.time(),
            )

            if is_live_requested() and live_session is None:
                api_key = os.environ.get("OPENAI_API_KEY")
                if not api_key:
                    set_live_requested(False)
                    update_status(
                        live_state="error",
                        live_error="OPENAI_API_KEY is not configured",
                    )
                else:
                    live_session = LiveSession(
                        connection,
                        device_send_lock,
                        api_key,
                    )
                    try:
                        await live_session.start()
                    except Exception as error:
                        print(f"Unable to start GPT-Live: {error}")
                        update_status(live_state="error", live_error=str(error))
                        set_live_requested(False)
                        await live_session.close()
                        live_session = None

            if live_session is not None:
                receiver_finished = (
                    live_session.receiver is not None
                    and live_session.receiver.done()
                )
                if not is_live_requested() or receiver_finished:
                    await live_session.close()
                    live_session = None
                else:
                    await live_session.send_audio(message)

            if now - stats.last_report_at >= 1.0:
                print(
                    f"{device_id}: {rms_dbfs:6.1f} dBFS, "
                    f"peak {peak:0.3f}, {sample_rate:0.0f} samples/s"
                )
                async with device_send_lock:
                    await connection.send(
                        json.dumps(
                            {
                                "type": "level",
                                "rms_dbfs": round(rms_dbfs, 1),
                                "peak": round(peak, 3),
                            }
                        )
                    )
                stats.last_report_at = now
    except ConnectionClosed as error:
        print(f"Device {device_id} disconnected: {error.code} {error.reason}")
    finally:
        set_live_requested(False)
        if live_session is not None:
            await live_session.close()
        with status_lock:
            if device_status.connection_key == connection_key:
                device_status.connected = False
                device_status.live_state = "ready"
                device_status.updated_at = time.time()


async def run(host: str, port: int, dashboard_host: str, dashboard_port: int) -> None:
    dashboard = ThreadingHTTPServer(
        (dashboard_host, dashboard_port),
        DashboardHandler,
    )
    dashboard_thread = threading.Thread(
        target=dashboard.serve_forever,
        name="device-dashboard",
        daemon=True,
    )
    dashboard_thread.start()

    try:
        async with serve(
            handle_device,
            host,
            port,
            compression=None,
            max_size=4096,
            ping_interval=20,
            ping_timeout=10,
        ):
            print(f"Device gateway listening on ws://{host}:{port}/device")
            print(f"Live microphone dashboard: http://localhost:{dashboard_port}")
            await asyncio.Future()
    finally:
        dashboard.shutdown()
        dashboard.server_close()
        dashboard_thread.join()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Receive circleOS microphone audio")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", default=8787, type=int)
    parser.add_argument("--dashboard-host", default="127.0.0.1")
    parser.add_argument("--dashboard-port", default=8788, type=int)
    arguments = parser.parse_args()
    try:
        asyncio.run(
            run(
                arguments.host,
                arguments.port,
                arguments.dashboard_host,
                arguments.dashboard_port,
            )
        )
    except KeyboardInterrupt:
        print("\nDevice gateway stopped.")
