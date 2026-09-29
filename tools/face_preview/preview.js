// UI glue for index.html: runs the face engine on the browser's frame clock and draws the panel.
(function () {
  'use strict';
  const $ = (id) => document.getElementById(id);
  const M = window.ENCO_MANIFEST;
  const C = window.ENCO_CHARS || {};
  if (!M || !Object.keys(C).length) {
    $('missing').style.display = 'block';
    return;
  }
  const { Face, ToRGBA, W, H } = window.EncoPreview;
  const NAMES = { k3: '1号驾驶员 (k3)', fox: '2号驾驶员 (fox)' };

  const face = new Face(M, C);
  const canvas = $('screen');
  const ctx = canvas.getContext('2d');
  if (!ctx.roundRect) ctx.roundRect = function (x, y, w, h) { this.rect(x, y, w, h); };  // older Safari
  let paused = false;
  let speed = 1;
  let last = performance.now();
  let frames = 0, fpsT0 = last, fps = 0;

  // --- controls ---------------------------------------------------------------------------------
  for (const id of M.characters.filter((c) => C[c])) {
    const o = document.createElement('option');
    o.value = id;
    o.textContent = NAMES[id] || id;
    $('character').appendChild(o);
  }
  $('character').value = face.chr_.id;

  function buildExprButtons() {
    const box = $('exprs');
    box.textContent = '';
    for (const name of ['neutral', ...face.chr_.exprs.map((e) => e.name)]) {
      const b = document.createElement('button');
      b.textContent = name;
      b.addEventListener('click', () => face.ShowExpression(name, Number($('hold').value) || 0, true));
      box.appendChild(b);
    }
    const hasEars = face.chr_.ears.length > 0;
    for (const b of $('earBox').querySelectorAll('button')) b.disabled = !hasEars;
    $('earBox').style.opacity = hasEars ? 1 : 0.45;
  }
  buildExprButtons();

  $('character').addEventListener('change', (e) => { face.SetCharacter(e.target.value); buildExprButtons(); });
  $('reset').addEventListener('click', () => { face.ApplyCharacter(); });
  $('zoom').addEventListener('change', resize);
  $('speed').addEventListener('change', (e) => { speed = Number(e.target.value); });
  $('pause').addEventListener('click', () => {
    paused = !paused;
    $('pause').textContent = paused ? 'Resume' : 'Pause';
    $('pause').classList.toggle('on', paused);
  });
  $('step').addEventListener('click', () => { face.Advance(50); draw(); });

  for (const b of $('earBox').querySelectorAll('button[data-ear]')) {
    b.addEventListener('click', () => {
      const which = b.dataset.ear;
      if (which === 'shake') face.TwitchEars();
      else face.StartEarScript(which, Number($('gain').value) || 1, $('mirror').checked);
    });
  }
  $('idleTwitch').addEventListener('change', (e) => { face.idleTwitch = e.target.checked; });
  $('sleepy').addEventListener('change', (e) => face.UpdateRobotFaceEmotion(e.target.checked ? 'sleepy' : 'neutral'));
  $('speaking').addEventListener('change', (e) => { face.speaking = e.target.checked; });
  $('ambient').addEventListener('change', (e) => {
    face.ambient_mode_ = Number(e.target.value);
    face.next_ambient_tick_ = face.face_tick_ + face.AmbientGapTicks();
  });
  $('caption').addEventListener('change', (e) => { face.caption = e.target.checked; });
  $('statusH').addEventListener('change', (e) => {
    face.statusH = Math.max(0, Math.min(60, Number(e.target.value) || 0));
    resize();
  });
  $('warps').addEventListener('change', (e) => { face.warps = e.target.checked; });
  $('earMap').addEventListener('change', (e) => { face.showEarMap = e.target.checked; });
  $('hairBands').addEventListener('change', (e) => { face.showHairBands = e.target.checked; });

  function resize() {
    const z = Number($('zoom').value);
    canvas.style.width = W * z + 'px';
    canvas.style.height = H * z + 'px';
  }
  resize();

  // --- drawing ----------------------------------------------------------------------------------
  const image = ctx.createImageData(W, H);
  function draw() {
    const fb = face.Render();
    const CH = face.CH;
    const sh = face.statusH;
    const rgba = new Uint8Array(W * CH * 4);
    ToRGBA(fb, rgba);
    image.data.fill(0);
    image.data.set(rgba, W * sh * 4);
    ctx.putImageData(image, 0, 0);

    // Status bar (approximation of the firmware's top HUD bar).
    if (sh > 0) {
      ctx.fillStyle = '#040910';
      ctx.fillRect(0, 0, W, sh);
      ctx.fillStyle = '#00f0ff';
      ctx.font = '13px "PingFang SC", sans-serif';
      ctx.textBaseline = 'middle';
      const d = new Date();
      ctx.fillText(`${d.getHours()}:${String(d.getMinutes()).padStart(2, '0')}`, 8, sh / 2);
      ctx.textAlign = 'right';
      ctx.fillText(NAMES[face.chr_.id] ? NAMES[face.chr_.id].split(' ')[0] : face.chr_.id, W - 8, sh / 2);
      ctx.textAlign = 'left';
    }
    // Caption pill (drawn by LVGL on the device, never warped).
    if (face.caption) {
      const r = face.PillRect();
      const x = r.x1, y = r.y1 + sh, w = r.x2 - r.x1 + 1, h = r.y2 - r.y1 + 1;
      ctx.save();
      ctx.beginPath();
      ctx.roundRect(x, y, w, h, 12);
      ctx.fillStyle = 'rgba(11,18,32,0.9)';
      ctx.fill();
      ctx.strokeStyle = '#38bdf8';
      ctx.lineWidth = 1;
      ctx.stroke();
      ctx.beginPath();
      ctx.roundRect(x - 3, y - 3, w + 6, h + 6, 15);
      ctx.strokeStyle = 'rgba(56,189,248,0.3)';
      ctx.stroke();
      ctx.clip();
      ctx.fillStyle = '#f1f5f9';
      ctx.font = '15px "PingFang SC", sans-serif';
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText($('captionText').value, x + w / 2, y + h / 2 + 1);
      ctx.restore();
    }

    const ears = face.EarState();
    const expr = face.expr_index_ >= 0 ? face.chr_.exprs[face.expr_index_].name : 'neutral';
    const lines = [
      `t=${(face.now / 1000).toFixed(2)}s  ${fps.toFixed(0)} fps  speed ${speed}×${paused ? '  PAUSED' : ''}`,
      `expression: ${expr}${face.expr_ambient_ ? ' (ambient)' : ''}  blink ${face.blink_frame_}  mouth ${face.mouth_open_ ? 'open' : 'rest'}`,
    ];
    ears.forEach((e, i) => lines.push(
      `ear ${i}: root ${e.base.toFixed(1).padStart(5)}°  tip ${e.tip.toFixed(1).padStart(5)}°  ` +
      `ornaments ${e.deco.map((d) => d.toFixed(1)).join(', ')}°`));
    if (face.ear_script_) lines.push(`script: ${face.ear_script_name_}`);
    $('readout').textContent = lines.join('\n');
  }

  function loop(t) {
    const dt = Math.min(100, t - last);
    last = t;
    if (!paused) face.Advance(dt * speed);
    draw();
    frames++;
    if (t - fpsT0 > 1000) { fps = frames * 1000 / (t - fpsT0); frames = 0; fpsT0 = t; }
    requestAnimationFrame(loop);
  }
  requestAnimationFrame(loop);
})();
