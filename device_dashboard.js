const elements = {
  connection: document.querySelector("#connection"),
  connectionText: document.querySelector("#connection-text"),
  orb: document.querySelector("#orb"),
  meterFill: document.querySelector("#meter-fill"),
  levelLabel: document.querySelector("#level-label"),
  sampleRate: document.querySelector("#sample-rate"),
  audioSeconds: document.querySelector("#audio-seconds"),
  deviceId: document.querySelector("#device-id"),
  startLive: document.querySelector("#start-live"),
  stopLive: document.querySelector("#stop-live"),
  liveError: document.querySelector("#live-error"),
  inputTranscript: document.querySelector("#input-transcript"),
  outputTranscript: document.querySelector("#output-transcript"),
};

function render(status) {
  const connected = status.connected === true;
  elements.connection.classList.toggle("connected", connected);
  elements.connectionText.textContent = connected ? "Live" : "Waiting for device";

  const dbfs = Number.isFinite(status.rms_dbfs) ? status.rms_dbfs : -96;
  const normalized = Math.max(0, Math.min(1, (dbfs + 72) / 60));
  elements.orb.style.setProperty("--level", (0.72 + normalized * 0.38).toFixed(3));
  elements.orb.style.setProperty("--brightness", (0.8 + normalized * 0.5).toFixed(3));
  elements.meterFill.style.setProperty("--meter", `${(normalized * 100).toFixed(1)}%`);
  elements.levelLabel.textContent = `${dbfs.toFixed(1)} dBFS`;

  elements.sampleRate.textContent =
    `${Number(status.sample_rate || 0).toLocaleString()} samples/s`;
  elements.audioSeconds.textContent =
    `${Number(status.audio_seconds || 0).toFixed(1)} seconds`;
  elements.deviceId.textContent = status.device_id || "--";

  const liveActive = ["connecting", "active"].includes(status.live_state);
  elements.startLive.disabled = !connected || liveActive || status.live_requested;
  elements.stopLive.disabled = !liveActive && !status.live_requested;
  if (status.live_state === "connecting") {
    elements.startLive.textContent = "Connecting to GPT-Live...";
  } else if (status.live_state === "active") {
    elements.startLive.textContent = "Voice session active";
  } else {
    elements.startLive.textContent = "Start voice session";
  }
  elements.liveError.textContent = status.live_error || "";
  elements.inputTranscript.textContent =
    status.input_transcript || "Waiting for speech...";
  elements.outputTranscript.textContent =
    status.output_transcript || "Waiting for a response...";
}

async function setLiveState(action) {
  elements.startLive.disabled = true;
  elements.stopLive.disabled = true;
  elements.liveError.textContent = "";
  try {
    const response = await fetch(`/api/live/${action}`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
    });
    const result = await response.json();
    if (!response.ok) {
      throw new Error(result.error || `Unable to ${action} the voice session`);
    }
    render(result);
  } catch (error) {
    elements.liveError.textContent =
      error instanceof Error ? error.message : String(error);
  }
}

async function refresh() {
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    if (!response.ok) {
      throw new Error(`Status request failed: ${response.status}`);
    }
    render(await response.json());
  } catch (error) {
    elements.connection.classList.remove("connected");
    elements.connectionText.textContent = "Gateway unavailable";
    console.error(error);
  }
}

elements.startLive.addEventListener("click", () => setLiveState("start"));
elements.stopLive.addEventListener("click", () => setLiveState("stop"));

refresh();
setInterval(refresh, 150);
