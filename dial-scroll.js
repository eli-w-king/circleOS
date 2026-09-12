const canvas = document.querySelector("#dial");
const context = canvas.getContext("2d");
const selectedLetter = document.querySelector("#selected-letter");
const message = document.querySelector("#message");
const controls = document.querySelector(".controls");
const deleteButton = document.querySelector('[data-action="delete"]');
const beamParallax = document.querySelector("#beam-parallax");

const LETTERS = [..."ABCDEFGHIJKLMNOPQRSTUVWXYZ"];
const SIZE = 466;
const CENTER = SIZE / 2;
const STEP = (Math.PI * 2) / LETTERS.length;
const TOP = -Math.PI / 2;
const RING_RADIUS = 188;
const TOUCH_RADIUS = 112;
const MESSAGE_WIDTH = 220;
const MESSAGE_FONT_SIZE = 32;
const GHOST_GAP = 3;

let rotation = 0;
let selectedIndex = 0;
let typedText = "";
let messageScroll = 0;
let followMessageEnd = true;
let dragging = false;
let centerPressed = false;
let centerScrolled = false;
let centerPointerX = 0;
let centerStartScroll = 0;
let lastPointerAngle = 0;
let animationFrame;
let wheelTimer;
let beamResetTimer;

function modulo(value, divisor) {
  return ((value % divisor) + divisor) % divisor;
}

function normalizeAngle(angle) {
  return Math.atan2(Math.sin(angle), Math.cos(angle));
}

function selectionForRotation(value) {
  return modulo(Math.round(-value / STEP), LETTERS.length);
}

function canvasPoint(event) {
  const bounds = canvas.getBoundingClientRect();
  return {
    x: ((event.clientX - bounds.left) / bounds.width) * SIZE,
    y: ((event.clientY - bounds.top) / bounds.height) * SIZE,
  };
}

function pointAngle(point) {
  return Math.atan2(point.y - CENTER, point.x - CENTER);
}

function pointRadius(point) {
  return Math.hypot(point.x - CENTER, point.y - CENTER);
}

function drawText(text, x, y, size, color, weight = 600) {
  context.fillStyle = color;
  context.font = `${weight} ${size}px Inter, system-ui, sans-serif`;
  context.textAlign = "center";
  context.textBaseline = "middle";
  context.fillText(text, x, y);
}

function renderBackground() {
  context.fillStyle = "#000";
  context.fillRect(0, 0, SIZE, SIZE);

  context.beginPath();
  context.arc(CENTER, CENTER, 226, 0, Math.PI * 2);
  context.strokeStyle = "rgba(255, 255, 255, 0.12)";
  context.lineWidth = 1;
  context.stroke();
}

function renderLetters() {
  for (let index = 0; index < LETTERS.length; index += 1) {
    const angle = TOP + index * STEP + rotation;
    const x = CENTER + Math.cos(angle) * RING_RADIUS;
    const y = CENTER + Math.sin(angle) * RING_RADIUS;
    const selected = index === selectedIndex;

    context.shadowColor = selected ? "rgba(255, 255, 255, 0.75)" : "transparent";
    context.shadowBlur = selected ? 12 : 0;
    drawText(
      LETTERS[index],
      x,
      y,
      selected ? 23 : 16,
      selected ? "#fff" : "rgba(255, 255, 255, 0.46)",
      selected ? 700 : 500,
    );
    context.shadowBlur = 0;
  }
}

function messageWidth() {
  context.font = `500 ${MESSAGE_FONT_SIZE}px Inter, system-ui, sans-serif`;
  return context.measureText(typedText).width;
}

function ghostWidth() {
  context.font = `500 ${MESSAGE_FONT_SIZE}px Inter, system-ui, sans-serif`;
  return context.measureText(LETTERS[selectedIndex]).width;
}

function previewWidth() {
  return messageWidth() + (typedText ? GHOST_GAP : 0) + ghostWidth();
}

function maximumMessageScroll() {
  return Math.max(0, previewWidth() - MESSAGE_WIDTH);
}

function scrollMessageToEnd() {
  followMessageEnd = true;
  messageScroll = maximumMessageScroll();
}

function renderCenter() {
  const left = CENTER - MESSAGE_WIDTH / 2;
  const committedWidth = messageWidth();
  const contentWidth = previewWidth();
  const overflowing = contentWidth > MESSAGE_WIDTH;
  const start = overflowing ? left - messageScroll : CENTER - contentWidth / 2;

  context.save();
  context.beginPath();
  context.rect(left, CENTER - 22, MESSAGE_WIDTH, 44);
  context.clip();
  context.font = `500 ${MESSAGE_FONT_SIZE}px Inter, system-ui, sans-serif`;
  context.textAlign = "left";
  context.textBaseline = "middle";
  if (typedText) {
    context.fillStyle = "#f5f5f5";
    context.fillText(typedText, start, CENTER);
  }
  context.fillStyle = "#363636";
  context.fillText(
    LETTERS[selectedIndex],
    start + committedWidth + (typedText ? GHOST_GAP : 0),
    CENTER,
  );
  context.restore();

  if (overflowing && messageScroll > 1) {
    const fade = context.createLinearGradient(left, 0, left + 38, 0);
    fade.addColorStop(0, "#000");
    fade.addColorStop(0.72, "#000");
    fade.addColorStop(1, "rgba(0, 0, 0, 0)");
    context.fillStyle = fade;
    context.fillRect(left, CENTER - 23, 42, 46);
    drawText("...", left + 13, CENTER - 1, 18, "#f5f5f5", 500);
  }
}

function render() {
  selectedIndex = selectionForRotation(rotation);
  if (followMessageEnd) messageScroll = maximumMessageScroll();
  context.clearRect(0, 0, SIZE, SIZE);
  renderBackground();
  renderLetters();
  renderCenter();
  selectedLetter.textContent = LETTERS[selectedIndex];
}

function addBeamParallax(rotationDelta) {
  followMessageEnd = true;
  const degrees = Math.max(-3.5, Math.min(3.5, -rotationDelta * 18));
  beamParallax.style.transform = `rotate(${degrees}deg)`;
  window.clearTimeout(beamResetTimer);
  beamResetTimer = window.setTimeout(() => {
    beamParallax.style.transform = "rotate(0deg)";
  }, 80);
}

function animateRotation(target) {
  window.cancelAnimationFrame(animationFrame);
  const start = rotation;
  const distance = target - start;
  const startTime = performance.now();
  const duration = 170;

  function frame(now) {
    const progress = Math.min((now - startTime) / duration, 1);
    const eased = 1 - Math.pow(1 - progress, 3);
    rotation = start + distance * eased;
    render();
    if (progress < 1) {
      animationFrame = window.requestAnimationFrame(frame);
    }
  }

  animationFrame = window.requestAnimationFrame(frame);
}

function snapToLetter() {
  animateRotation(Math.round(rotation / STEP) * STEP);
}

function rotateBy(steps) {
  addBeamParallax(-steps * STEP);
  animateRotation(rotation - steps * STEP);
}

function selectCurrentLetter() {
  typedText += LETTERS[selectedIndex];
  scrollMessageToEnd();
  message.textContent = typedText;
  render();
  canvas.animate(
    [
      { filter: "brightness(1)" },
      { filter: "brightness(1.35)" },
      { filter: "brightness(1)" },
    ],
    { duration: 180, easing: "ease-out" },
  );
}

function deleteLetter() {
  typedText = typedText.slice(0, -1);
  scrollMessageToEnd();
  message.textContent = typedText || "—";
  render();
}

canvas.addEventListener("pointerdown", (event) => {
  const point = canvasPoint(event);
  centerPressed = pointRadius(point) <= TOUCH_RADIUS;
  centerScrolled = false;
  centerPointerX = point.x;
  centerStartScroll = messageScroll;
  dragging = !centerPressed;
  lastPointerAngle = pointAngle(point);
  canvas.setPointerCapture(event.pointerId);
});

canvas.addEventListener("pointermove", (event) => {
  const point = canvasPoint(event);
  if (centerPressed) {
    const delta = point.x - centerPointerX;
    if (Math.abs(delta) > 5) centerScrolled = true;
    if (centerScrolled) {
      followMessageEnd = false;
      messageScroll = Math.max(
        0,
        Math.min(maximumMessageScroll(), centerStartScroll - delta),
      );
      render();
    }
    return;
  }
  if (!dragging) return;
  const angle = pointAngle(point);
  const delta = normalizeAngle(angle - lastPointerAngle);
  rotation += delta;
  addBeamParallax(delta);
  lastPointerAngle = angle;
  render();
});

canvas.addEventListener("pointerup", (event) => {
  const point = canvasPoint(event);
  if (
    centerPressed &&
    !centerScrolled &&
    pointRadius(point) <= TOUCH_RADIUS
  ) {
    selectCurrentLetter();
  } else if (dragging) {
    snapToLetter();
  }
  centerPressed = false;
  centerScrolled = false;
  dragging = false;
  canvas.releasePointerCapture(event.pointerId);
});

canvas.addEventListener("pointercancel", () => {
  centerPressed = false;
  centerScrolled = false;
  dragging = false;
  snapToLetter();
});

canvas.addEventListener(
  "wheel",
  (event) => {
    event.preventDefault();
    const delta = -Math.sign(event.deltaY || event.deltaX) * STEP;
    rotation += delta;
    addBeamParallax(delta);
    render();
    window.clearTimeout(wheelTimer);
    wheelTimer = window.setTimeout(snapToLetter, 90);
  },
  { passive: false },
);

controls.addEventListener("click", (event) => {
  const action = event.target.closest("button")?.dataset.action;
  if (action === "previous") rotateBy(-1);
  if (action === "next") rotateBy(1);
  if (action === "select") selectCurrentLetter();
});

deleteButton.addEventListener("click", deleteLetter);

window.addEventListener("keydown", (event) => {
  if (event.key === "ArrowLeft") rotateBy(-1);
  if (event.key === "ArrowRight") rotateBy(1);
  if (event.key === "Enter" || event.key === " ") selectCurrentLetter();
  if (event.key === "Backspace") deleteLetter();
});

render();
