#pragma once
#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-br">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Garra Robótica</title>
<style>
  body { font-family: Arial, sans-serif; background:#111; color:#eee; margin:0; padding:20px; }
  h1 { text-align:center; font-size:22px; }
  h2 { text-align:center; font-size:18px; margin-top:30px; }
  .card { background:#1e1e1e; border-radius:12px; padding:16px; margin:14px auto; max-width:480px; }
  .row { display:flex; justify-content:space-between; margin-bottom:8px; }
  .val { color:#4fc3f7; font-weight:bold; }
  .range { display:flex; justify-content:space-between; font-size:12px; color:#888; }
  input[type=range] { width:100%; height:32px; }
  button { width:100%; max-width:480px; display:block; margin:14px auto; padding:12px;
           border:0; border-radius:10px; background:#4fc3f7; color:#000; font-size:16px; }
  button.sec { background:#333; color:#eee; }
  button.ok { background:#66bb6a; }
  button.stop { background:#ef5350; }
  .mini { display:inline-block; width:auto; margin:0 0 0 6px; padding:6px 10px; font-size:13px; border-radius:8px; }
  .step.ativo { outline:2px solid #66bb6a; }
  .sh { display:flex; justify-content:space-between; align-items:center; margin-bottom:8px; }
  .chips { display:flex; flex-wrap:wrap; gap:6px; font-size:12px; color:#bbb; margin-bottom:10px; }
  .chips span { background:#2a2a2a; border-radius:6px; padding:3px 7px; }
  .tempos { display:flex; gap:12px; }
  .tempos label { flex:1; font-size:13px; color:#aaa; }
  .tempos input[type=number] { width:100%; box-sizing:border-box; margin-top:4px; padding:8px;
           background:#111; color:#eee; border:1px solid #444; border-radius:8px; font-size:15px; }
  .ctl { display:flex; gap:10px; align-items:center; }
  .ctl label { font-size:14px; white-space:nowrap; }
  .vazio { text-align:center; color:#777; font-size:14px; }
</style>
</head>
<body>
<h1>Controle da Garra Robótica</h1>
<div id="servos"></div>
<button class="sec" onclick="centralizar()">Posição inicial</button>

<h2>Sequência de movimentos</h2>

<div class="card">
  <div class="tempos">
    <label>Tempo de movimento (s)
      <input type="number" id="tMove" min="0.1" max="120" step="0.1" value="1.0"></label>
    <label>Pausa depois (s)
      <input type="number" id="tHold" min="0" max="600" step="0.1" value="0.5"></label>
  </div>
  <button onclick="gravarPasso()">+ Gravar passo com a posição atual</button>
</div>

<div id="passos"></div>

<div class="card">
  <div class="ctl">
    <button class="ok" style="margin:0" onclick="executar()">▶ Executar</button>
    <button class="stop" style="margin:0" onclick="parar()">■ Parar</button>
  </div>
  <div style="margin-top:12px">
    <label><input type="checkbox" id="repetir"> Repetir em loop</label>
  </div>
</div>
<button class="sec" onclick="limparTudo()">Apagar todos os passos</button>

<script>
const MAX_PASSOS = 30;
let cfg = [];
let seq = [];            // [{a:[..], move:s, hold:s}]
let ultimoEnvio = [];
let timer = null;

// ---------- Sliders ----------
function enviar(i, valor) {
  fetch(`/servo?id=${i}&angle=${valor}`).catch(() => {});
}

function mover(i, valor, forcar) {
  document.getElementById("v" + i).textContent = valor + "°";
  const agora = Date.now();
  if (!forcar && agora - ultimoEnvio[i] < 50) return;
  ultimoEnvio[i] = agora;
  enviar(i, valor);
}

function criarSliders() {
  const box = document.getElementById("servos");
  cfg.forEach((s, i) => {
    ultimoEnvio[i] = 0;
    box.insertAdjacentHTML("beforeend", `
      <div class="card">
        <div class="row"><span>${s.name}</span><span class="val" id="v${i}">${s.home}°</span></div>
        <input type="range" id="s${i}" min="${s.min}" max="${s.max}" value="${s.home}"
               oninput="mover(${i}, this.value, false)"
               onchange="mover(${i}, this.value, true)">
        <div class="range"><span>${s.min}°</span><span>${s.max}°</span></div>
      </div>`);
  });
}

function mostrarPose(pose) {
  pose.forEach((ang, i) => {
    document.getElementById("s" + i).value = ang;
    document.getElementById("v" + i).textContent = ang + "°";
  });
}

function irParaPose(pose) {
  mostrarPose(pose);
  pose.forEach((ang, i) => enviar(i, ang));
}

function poseAtual() {
  return cfg.map((_, i) => Number(document.getElementById("s" + i).value));
}

function centralizar() {
  irParaPose(cfg.map(s => s.home));
}

// ---------- Sequência ----------
function nomeCurto(i) {
  return cfg[i].name.replace(/\(.*\)/, "").trim();
}

function enviarSequencia() {
  const corpo = seq.map(s =>
    [...s.a, Math.round(s.move * 1000), Math.round(s.hold * 1000)].join(",")
  ).join("\n");
  fetch("/seq", { method: "POST", headers: { "Content-Type": "text/plain" }, body: corpo })
    .catch(() => {});
}

function desenharPassos() {
  const box = document.getElementById("passos");
  if (seq.length === 0) {
    box.innerHTML = '<div class="vazio">Nenhum passo gravado ainda.</div>';
    return;
  }
  box.innerHTML = seq.map((s, n) => `
    <div class="card step" id="st${n}">
      <div class="sh">
        <b>Passo ${n + 1}</b>
        <span>
          <button class="mini sec" onclick="irPasso(${n})">Ir</button>
          <button class="mini sec" onclick="atualizarPasso(${n})">Atualizar</button>
          <button class="mini stop" onclick="apagarPasso(${n})">X</button>
        </span>
      </div>
      <div class="chips">${s.a.map((ang, i) => `<span>${nomeCurto(i)} ${ang}°</span>`).join("")}</div>
      <div class="tempos">
        <label>Mover (s)
          <input type="number" min="0.1" max="120" step="0.1" value="${s.move}"
                 onchange="editarTempo(${n}, 'move', this.value)"></label>
        <label>Pausa (s)
          <input type="number" min="0" max="600" step="0.1" value="${s.hold}"
                 onchange="editarTempo(${n}, 'hold', this.value)"></label>
      </div>
    </div>`).join("");
}

function gravarPasso() {
  if (seq.length >= MAX_PASSOS) { alert("Limite de " + MAX_PASSOS + " passos."); return; }
  seq.push({
    a: poseAtual(),
    move: Math.max(0.1, parseFloat(document.getElementById("tMove").value) || 1),
    hold: Math.max(0, parseFloat(document.getElementById("tHold").value) || 0)
  });
  desenharPassos();
  enviarSequencia();
}

function irPasso(n) { irParaPose(seq[n].a); }

function atualizarPasso(n) {
  seq[n].a = poseAtual();
  desenharPassos();
  enviarSequencia();
}

function apagarPasso(n) {
  seq.splice(n, 1);
  desenharPassos();
  enviarSequencia();
}

function editarTempo(n, campo, valor) {
  let v = parseFloat(valor);
  if (isNaN(v)) v = campo === "move" ? 1 : 0;
  seq[n][campo] = campo === "move" ? Math.max(0.1, v) : Math.max(0, v);
  desenharPassos();
  enviarSequencia();
}

function limparTudo() {
  if (!confirm("Apagar todos os passos?")) return;
  seq = [];
  desenharPassos();
  enviarSequencia();
}

// ---------- Execução ----------
function executar() {
  if (seq.length === 0) { alert("Grave pelo menos um passo."); return; }
  const loop = document.getElementById("repetir").checked ? 1 : 0;
  fetch(`/play?loop=${loop}`).then(() => acompanhar()).catch(() => {});
}

function parar() {
  fetch("/stop").catch(() => {});
}

function destacar(n) {
  document.querySelectorAll(".step").forEach((el, i) =>
    el.classList.toggle("ativo", i === n));
}

function acompanhar() {
  if (timer) return;
  timer = setInterval(async () => {
    try {
      const st = await (await fetch("/status")).json();
      mostrarPose(st.pos);
      if (st.playing) {
        destacar(st.step);
      } else {
        destacar(-1);
        clearInterval(timer);
        timer = null;
      }
    } catch (e) {}
  }, 300);
}

// ---------- Início ----------
async function iniciar() {
  cfg = await (await fetch("/config")).json();
  criarSliders();

  const estado = await (await fetch("/state")).json();   // posição estimada atual
  mostrarPose(estado);

  const txt = await (await fetch("/seq")).text();        // sequência salva no ESP
  seq = txt.trim() === "" ? [] : txt.trim().split("\n").map(l => {
    const p = l.split(",").map(Number);
    return { a: p.slice(0, cfg.length), move: p[cfg.length] / 1000, hold: p[cfg.length + 1] / 1000 };
  });
  desenharPassos();

  const st = await (await fetch("/status")).json();
  if (st.playing) acompanhar();
}
iniciar().catch(() => {});
</script>
</body>
</html>
)rawliteral";