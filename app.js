const startButton = document.querySelector("#start");
const stopButton = document.querySelector("#stop");
const clearButton = document.querySelector("#clear");
const status = document.querySelector("#status");
const audio = document.querySelector("#audio");
const messages = document.querySelector("#messages");
const orb = document.querySelector("#orb");

let peer;
let events;
let microphone;
let closeTimeout;
let ready = false;
let finalized = false;
const transcriptNodes = new Map();

function setStatus(message, state) {
  status.textContent = message;
  orb.dataset.state = state;
}

function cleanup() {
  window.clearTimeout(closeTimeout);
  microphone?.getTracks().forEach((track) => track.stop());
  events?.close();
  peer?.close();
  audio.srcObject = null;
  peer = undefined;
  events = undefined;
  microphone = undefined;
  ready = false;
  startButton.disabled = false;
  stopButton.disabled = true;
}

function transcriptKey(event, role) {
  return `${role}:${event.item_id ?? event.delegation_id ?? "current"}`;
}

function appendTranscript(event, role) {
  const delta = typeof event.delta === "string" ? event.delta : "";
  if (!delta) return;

  const empty = messages.querySelector(".empty");
  empty?.remove();

  const key = transcriptKey(event, role);
  let row = transcriptNodes.get(key);
  if (!row) {
    row = document.createElement("p");
    row.className = `message ${role}`;
    const label = document.createElement("strong");
    label.textContent = role === "user" ? "You" : "Assistant";
    const text = document.createElement("span");
    row.append(label, text);
    messages.append(row);
    transcriptNodes.set(key, row);
  }
  row.querySelector("span").textContent += delta;
  messages.scrollTop = messages.scrollHeight;
}

function handleEvent(event) {
  switch (event.type) {
    case "session.started":
      ready = true;
      stopButton.disabled = false;
      setStatus("Listening", "listening");
      break;
    case "session.input_transcript.delta":
      appendTranscript(event, "user");
      setStatus("Listening", "listening");
      break;
    case "session.output_transcript.delta":
      appendTranscript(event, "assistant");
      setStatus("Speaking", "speaking");
      break;
    case "session.closed":
      finalized = true;
      console.info("Final session usage", event.usage);
      setStatus("Conversation ended", "idle");
      cleanup();
      break;
    case "error":
      console.error("GPT-Live error", event);
      setStatus(event.error?.message ?? "The voice session reported an error", "error");
      break;
    default:
      console.debug("GPT-Live event", event);
  }
}

async function waitForIceGathering(connection) {
  if (connection.iceGatheringState === "complete") return;
  await new Promise((resolve, reject) => {
    const timeout = window.setTimeout(() => {
      connection.removeEventListener("icegatheringstatechange", onState);
      reject(new Error("Timed out while preparing the voice connection"));
    }, 10_000);

    function onState() {
      if (connection.iceGatheringState !== "complete") return;
      window.clearTimeout(timeout);
      connection.removeEventListener("icegatheringstatechange", onState);
      resolve();
    }

    connection.addEventListener("icegatheringstatechange", onState);
    onState();
  });
}

startButton.addEventListener("click", async () => {
  startButton.disabled = true;
  finalized = false;
  setStatus("Requesting microphone access", "connecting");

  try {
    const connection = new RTCPeerConnection();
    peer = connection;
    connection.addEventListener("track", (event) => {
      audio.srcObject = new MediaStream([event.track]);
      audio.play().catch((error) => {
        console.warn("Autoplay was blocked", error);
        setStatus("Select this page, then try speaking again", "error");
      });
    });
    connection.addEventListener("connectionstatechange", () => {
      if (connection.connectionState === "failed") {
        setStatus("Voice connection failed", "error");
        cleanup();
      }
    });

    microphone = await navigator.mediaDevices.getUserMedia({
      audio: {
        echoCancellation: true,
        noiseSuppression: true,
        autoGainControl: true,
      },
    });
    for (const track of microphone.getAudioTracks()) {
      connection.addTrack(track, microphone);
    }

    events = connection.createDataChannel("oai-events");
    events.addEventListener("message", ({ data }) => {
      try {
        handleEvent(JSON.parse(data));
      } catch (error) {
        console.error("Invalid GPT-Live event", error);
        setStatus("Received an invalid session event", "error");
      }
    });
    events.addEventListener("close", (event) => {
      if (event.target !== events || finalized) return;
      setStatus("Disconnected before usage was finalized", "error");
      cleanup();
    });

    const offer = await connection.createOffer();
    await connection.setLocalDescription(offer);
    await waitForIceGathering(connection);

    const sdp = connection.localDescription?.sdp;
    if (!sdp) throw new Error("The browser did not create an audio offer");
    setStatus("Connecting to GPT-Live", "connecting");
    const response = await fetch("/api/session", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ sdp }),
    });
    const result = await response.json();
    if (!response.ok) {
      throw new Error(result.error ?? "Unable to create the voice session");
    }
    await connection.setRemoteDescription({
      type: "answer",
      sdp: result.transport.sdp,
    });
  } catch (error) {
    console.error(error);
    setStatus(error instanceof Error ? error.message : String(error), "error");
    cleanup();
  }
});

stopButton.addEventListener("click", () => {
  if (!ready || !events || events.readyState !== "open") return;
  stopButton.disabled = true;
  setStatus("Finishing conversation", "connecting");
  events.send(JSON.stringify({ type: "session.close" }));
  closeTimeout = window.setTimeout(() => {
    setStatus("Session did not confirm its final usage", "error");
    cleanup();
  }, 15_000);
});

clearButton.addEventListener("click", () => {
  transcriptNodes.clear();
  messages.replaceChildren();
  const empty = document.createElement("p");
  empty.className = "empty";
  empty.textContent = "Conversation text will appear here.";
  messages.append(empty);
});
