// A line-by-line JavaScript port of the robot-face animation in firmware/main/display.cpp, for
// previewing on a Mac what the ESP32 panel will show. It uses the same data (exported from the
// generated face_assets_*.c by export.py), the same RGB565 pixels, the same fixed-point warps and
// the same timers (80 ms face tick, 50 ms motion tick), so what moves here moves the same way there.
//
// Function names match their firmware counterparts; keep them in step when display.cpp changes.
// No DOM in here: index.html drives it, and it also runs under node for headless checks.
(function (root) {
  'use strict';

  const W = 240, H = 320;                       // ENCO_FACE_W / ENCO_FACE_H, and the panel
  const kFaceTickMs = 80;
  const kMotionTickMs = 50;
  const kBlinkFrameShut = 2, kBlinkFrameLast = 5;
  const kBlinkMinTicks = 30, kBlinkMaxTicks = 75;
  const kHairPoseCount = 14, kHairMinGapTicks = 20, kHairGapSpreadTicks = 40;
  const kAmbientIdleMinTicks = 125, kAmbientIdleSpreadTicks = 190;
  const kAmbientListenMinTicks = 30, kAmbientListenSpreadTicks = 35;
  const kAmbientIdleFaces = ['happy', 'wink', 'pout', 'shy', 'surprised', 'thinking', 'sad'];
  const kAmbientListenFaces = ['thinking', 'happy', 'surprised', 'shy', 'wink'];
  const kPetalSway = [0, 1, 2, 3, 3, 3, 2, 1, 0, -1, -2, -3, -3, -3, -2, -1];
  const kPetalCount = 6;
  const kMaxEars = 2;
  const kDeg = 0.01745329, kTwoPi = 6.2831853;
  const kMouthCycle = [1, 2, 1, 0, 2, 1, 2, 0];
  const kHairPatterns = [
    [[-1, 0], [-2, -1], [-2, -1], [-2, -2], [-2, -2], [-1, -2], [0, -2], [0, -1], [1, 0], [1, 1], [0, 1], [0, 0], [0, 0], [0, 0]],
    [[1, 0], [2, 1], [2, 1], [2, 2], [2, 2], [1, 2], [0, 2], [0, 1], [-1, 0], [-1, -1], [0, -1], [0, 0], [0, 0], [0, 0]],
    [[-1, 0], [-2, 0], [-2, 0], [-1, 0], [0, 0], [1, 0], [2, 0], [2, 0], [1, 0], [1, 0], [0, 0], [0, 0], [0, 0], [0, 0]],
    [[-1, 0], [-2, -1], [-1, -2], [0, -1], [1, 0], [2, 1], [1, 2], [0, 1], [-1, 0], [-1, -1], [0, -1], [0, 0], [0, 0], [0, 0]],
  ];

  const rnd = (n) => Math.floor(Math.random() * n);   // esp_random() % n
  const rbit = () => Math.random() < 0.5;

  function b64bytes(s) {
    const bin = atob(s);
    const u8 = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
    return u8;
  }

  // lv_image_dsc_t -> {w, h, px: Uint16Array RGB565, alpha: Uint8Array | null}, decoded once.
  function img(dsc) {
    if (!dsc) return null;
    if (!dsc._dec) {
      const u8 = b64bytes(dsc.data);
      const n = dsc.w * dsc.h;
      const px = new Uint16Array(n);
      for (let i = 0; i < n; i++) px[i] = u8[2 * i] | (u8[2 * i + 1] << 8);
      dsc._dec = { w: dsc.w, h: dsc.h, px, alpha: dsc.cf === 'rgb565a8' ? u8.subarray(2 * n, 3 * n) : null };
    }
    return dsc._dec;
  }

  function SinTurns(t) {
    t -= Math.floor(t + 0.5);
    const y = 8 * t - 16 * t * Math.abs(t);
    return y + 0.225 * (y * Math.abs(y) - y);
  }

  function Lerp565(a, b, f) {
    if (f === 0) return a;
    const ea = ((a | (a << 16)) & 0x07E0F81F) >>> 0;
    const eb = ((b | (b << 16)) & 0x07E0F81F) >>> 0;
    const r = Math.floor((ea * (32 - f) + eb * f) / 32) & 0x07E0F81F;
    return (r | (r >>> 16)) & 0xFFFF;
  }

  // One side of one row, in place (see display.cpp WarpSpan). row holds x = 0 .. W-1.
  function WarpSpan(row, off, p0, p1, p2, b0, b1, feather, k, lo, hi) {
    if (lo > hi || p1 <= p0 || p2 <= p1) return;
    const body = b0 <= b1 && feather > 0;
    const step1 = Math.trunc(k * 65536 / (p1 - p0));
    const step2 = Math.trunc(k * 65536 / (p2 - p1));
    const xmax = W - 1;
    const px = (x) => {
      let d = x <= p1 ? (x - p0) * step1 : (p2 - x) * step2;
      if (body) {
        const dist = x < b0 ? b0 - x : (x > b1 ? x - b1 : 0);
        if (dist === 0) return;
        if (dist < feather) d = Math.trunc(d * dist / feather);
      }
      const s = (x * 65536 + d) | 0;
      const i = s >> 16;
      const f = (s >> 11) & 31;
      const i0 = Math.min(xmax, Math.max(0, i));
      const i1 = Math.min(xmax, Math.max(0, i + 1));
      row[off + x] = Lerp565(row[off + i0], row[off + i1], f);
    };
    if (k > 0) { for (let x = lo; x <= hi; x++) px(x); } else { for (let x = hi; x >= lo; x--) px(x); }
  }

  function EarSpring(o, xk, vk, target, hz, zeta, h) {
    const w = kTwoPi * hz;
    o[vk] += (w * w * (target - o[xk]) - 2 * zeta * w * o[vk]) * h;
    o[xk] += o[vk] * h;
  }

  function newEar() {
    return { base: 0, base_v: 0, tip: 0, tip_v: 0, deco: [0, 0, 0, 0], deco_v: [0, 0, 0, 0],
             anchor_vx: [0, 0, 0, 0], anchor_x: [0, 0, 0, 0], pose: 0,
             active: false, sb: 0, st: 0, fx: [0, 0, 0, 0], fy: [0, 0, 0, 0],
             dcos: [1, 1, 1, 1], dsin: [0, 0, 0, 0] };
  }

  class Face {
    constructor(manifest, chars) {
      this.M = manifest;
      this.chars = chars;
      const c = manifest.ear_consts || {};
      this.kEarMaxOut = c.kEarMaxOut ?? 0.30;
      this.kEarTipMaxBend = c.kEarTipMaxBend ?? 0.12;
      this.kEarRootHz = c.kEarRootHz ?? 7.0;
      this.kEarRootZeta = c.kEarRootZeta ?? 0.35;
      this.kEarTipHz = c.kEarTipHz ?? 5.0;
      this.kEarTipZeta = c.kEarTipZeta ?? 0.3;
      this.kEarPendulumPx = c.kEarPendulumPx ?? 60.0;

      this.now = 1;                 // lv_tick_get()
      this.nextFaceAt = kFaceTickMs;
      this.nextMotionAt = kMotionTickMs;
      this.statusH = 26;            // status bar height; the face container is the rest
      this.caption = true;          // the caption pill (never warped)
      this.speaking = false;        // audio_playback_signal::IsPlaying()
      this.current_emotion_ = 'neutral';
      this.ambient_enabled_ = true;
      this.ambient_mode_ = 1;       // 0 off, 1 idle, 2 listening
      this.face_tick_ = 0;
      this.last_ambient_expr_ = -1;
      this.next_ambient_tick_ = 0;
      this.showEarMap = false;
      this.showHairBands = false;
      this.warps = true;
      this.chr_ = chars[manifest.characters[0]];
      this.next_blink_tick_ = kBlinkMinTicks;
      this.next_hair_tick_ = 14;
      this.ApplyCharacter();
      this.next_ambient_tick_ = this.AmbientGapTicks();
    }

    get CH() { return H - this.statusH; }   // face container height

    // --- characters -------------------------------------------------------------------------
    SetCharacter(id) {
      const c = this.chars[id];
      if (!c || c === this.chr_) return false;
      this.chr_ = c;
      this.ApplyCharacter();
      return true;
    }

    ApplyCharacter() {
      this.expr_index_ = -1;
      this.expr_pinned_ = false;
      this.expr_ambient_ = false;
      this.expr_quiet_ticks_ = 0;
      this.blink_frame_ = this.current_emotion_ === 'sleepy' ? kBlinkFrameShut : 0;
      this.next_blink_tick_ = this.face_tick_ + kBlinkMinTicks;
      this.hair_step_ = 0;
      this.hair_pattern_ = 0;
      this.hair_level_bangs_ = 0;
      this.hair_level_locks_ = 0;
      this.next_hair_tick_ = this.face_tick_ + kHairMinGapTicks;
      this.mouth_open_ = false;
      this.mouthSprite = null;
      this.flow_gust_ = 0; this.flow_wind_ = 0; this.flow_billow_ = 0; this.flow_flutter_ = 0;
      this.ResetEars();
      this.petals_ = [];
      if (this.chr_.petals && this.chr_.petal_frames && this.chr_.petal_sizes) {
        for (let i = 0; i < kPetalCount; i++) { this.petals_.push({}); this.SpawnPetal(i, true); }
      }
      this.ApplyMouthFrame();
    }

    FindExpression(name) {
      return this.chr_.exprs.findIndex((e) => e.name === name);
    }

    AmbientGapTicks() {
      return this.ambient_mode_ === 2 ? kAmbientListenMinTicks + rnd(kAmbientListenSpreadTicks)
                                      : kAmbientIdleMinTicks + rnd(kAmbientIdleSpreadTicks);
    }

    // --- expressions / emotion ------------------------------------------------------------------
    ShowExpression(name, hold_ms, pinned) {
      const clear = !name || name === 'neutral' || name === 'none';
      let index = -1;
      if (!clear) {
        index = this.FindExpression(name);
        if (index < 0) return false;
      }
      if (!pinned && this.expr_pinned_ && this.expr_index_ >= 0) return true;
      this.expr_index_ = index;
      this.expr_pinned_ = pinned && index >= 0;
      this.expr_ambient_ = false;
      this.expr_quiet_ticks_ = Math.min(0xFFFF, Math.floor(hold_ms / kFaceTickMs));
      if (this.current_emotion_ !== 'sleepy') this.blink_frame_ = 0;
      if (index < 0) this.next_blink_tick_ = this.face_tick_ + kBlinkMinTicks;
      this.ApplyMouthFrame();
      return true;
    }

    UpdateRobotFaceEmotion(emotion) {
      this.current_emotion_ = emotion;
      if (emotion === 'sleepy') this.blink_frame_ = kBlinkFrameShut;
      else if (this.blink_frame_ === kBlinkFrameShut) this.blink_frame_ = 0;
    }

    EyesSprite() {   // ApplyBlinkFrame()
      if (this.blink_frame_ !== 0) {
        return this.blink_frame_ === kBlinkFrameShut ? this.chr_.eyes_shut : this.chr_.eyes_half;
      }
      return this.expr_index_ >= 0 ? this.chr_.exprs[this.expr_index_].eyes : null;
    }

    ApplyMouthFrame() {
      const rest = this.expr_index_ >= 0 ? this.chr_.exprs[this.expr_index_].mouth : null;
      if (!this.mouth_open_) { this.mouthSprite = rest; return; }
      const frame = kMouthCycle[Math.floor(this.face_tick_ / 2) % kMouthCycle.length];
      this.mouthSprite = frame === 0 ? rest : (frame === 2 ? this.chr_.mouth_wide : this.chr_.mouth_small);
    }

    // --- timers ----------------------------------------------------------------------------------
    // Advances the virtual clock by `ms`, firing both LVGL timers in time order.
    Advance(ms) {
      const end = this.now + ms;
      for (;;) {
        const t = Math.min(this.nextFaceAt, this.nextMotionAt);
        if (t > end) break;
        this.now = t;
        if (t === this.nextFaceAt) { this.OnFaceTimer(); this.nextFaceAt += kFaceTickMs; }
        if (t === this.nextMotionAt) { this.OnMotionTimer(); this.nextMotionAt += kMotionTickMs; }
      }
      this.now = end;
    }

    OnFaceTimer() {
      this.face_tick_++;
      // Blink
      if (this.current_emotion_ !== 'sleepy' && this.expr_index_ < 0) {
        if (this.blink_frame_ !== 0) {
          const next = this.blink_frame_ + 1;
          this.blink_frame_ = next > kBlinkFrameLast ? 0 : next;
          if (this.blink_frame_ === 0) {
            this.next_blink_tick_ = this.face_tick_ + kBlinkMinTicks + rnd(kBlinkMaxTicks - kBlinkMinTicks);
          }
        } else if (this.face_tick_ >= this.next_blink_tick_) {
          this.blink_frame_ = 1;
        }
      }
      // Random hair breeze (sprite characters)
      if (this.chr_.bangs) {
        if (this.hair_step_ !== 0) {
          this.hair_step_++;
          if (this.hair_step_ > kHairPoseCount) {
            this.hair_step_ = 0;
            this.next_hair_tick_ = this.face_tick_ + kHairMinGapTicks + rnd(kHairGapSpreadTicks);
          }
          this.ApplyHairFrame();
        } else if (this.face_tick_ >= this.next_hair_tick_) {
          this.hair_pattern_ = rnd(4);
          this.hair_step_ = 1;
          this.ApplyHairFrame();
        }
      }
      // Mouth
      this.mouth_open_ = this.speaking;
      this.ApplyMouthFrame();
      // Expression hold
      if (this.expr_index_ >= 0 && !this.mouth_open_) {
        if (this.expr_quiet_ticks_ > 0) {
          this.expr_quiet_ticks_--;
        } else {
          this.expr_index_ = -1;
          this.expr_pinned_ = false;
          this.expr_ambient_ = false;
          this.next_blink_tick_ = this.face_tick_ + kBlinkMinTicks;
          this.next_ambient_tick_ = this.face_tick_ + this.AmbientGapTicks();
          this.ApplyMouthFrame();
        }
      }
      // Ambient expressions
      if (this.ambient_enabled_ && this.ambient_mode_ !== 0 && this.expr_index_ < 0 && !this.mouth_open_ &&
          this.blink_frame_ === 0 && this.current_emotion_ !== 'sleepy' &&
          this.face_tick_ >= this.next_ambient_tick_) {
        const listening = this.ambient_mode_ === 2;
        const pool = listening ? kAmbientListenFaces : kAmbientIdleFaces;
        let pick = this.FindExpression(pool[rnd(pool.length)]);
        if (pick === this.last_ambient_expr_) pick = this.FindExpression(pool[rnd(pool.length)]);
        if (pick >= 0) {
          this.expr_index_ = pick;
          this.expr_pinned_ = false;
          this.expr_ambient_ = true;
          this.last_ambient_expr_ = pick;
          this.expr_quiet_ticks_ = listening ? 20 + rnd(10) : 25 + rnd(20);
          this.ApplyMouthFrame();
        } else {
          this.next_ambient_tick_ = this.face_tick_ + this.AmbientGapTicks();
        }
      }
    }

    ApplyHairFrame() {
      const idx = this.hair_step_ === 0 ? kHairPoseCount : this.hair_step_ - 1;
      const pose = idx >= kHairPoseCount ? [0, 0] : kHairPatterns[this.hair_pattern_ & 3][idx];
      this.hair_level_bangs_ = pose[0];
      this.hair_level_locks_ = pose[1];
    }

    OnMotionTimer() {
      this.UpdateHairFlow();
      this.UpdateEars();
      this.UpdatePetals();
    }

    UpdateHairFlow() {
      if (!this.chr_.hair_flow) return;
      const ms = this.now;
      const turn = (p) => (ms % p) / p;
      this.flow_gust_ = 0.65 + 0.35 * SinTurns(turn(6700)) * SinTurns(turn(2300) + 0.11);
      this.flow_wind_ = 0.55 * SinTurns(turn(11000));
      this.flow_billow_ = turn(2600);
      this.flow_flutter_ = turn(950);
    }

    // --- petals ----------------------------------------------------------------------------------
    SpawnPetal(index, anywhere) {
      const c = this.chr_;
      const p = this.petals_[index];
      p.size = rnd(3) === 0 ? 1 : 0;
      if (p.size >= c.petal_sizes) p.size = c.petal_sizes - 1;
      const dsc = c.petals[p.size * c.petal_frames];
      const w = dsc.w, h = dsc.h;
      const left = (index & 1) === 0;
      const x0 = left ? -Math.trunc(w / 2) : c.petal_lane_r + 6;
      let x1 = left ? c.petal_lane_l - w - 6 : W - Math.trunc(w / 2);
      if (x1 < x0) x1 = x0;
      const x = x0 + rnd(x1 - x0 + 1);
      const y = anywhere ? rnd(H) - h : -h - rnd(90);
      p.x16 = x * 16;
      p.y16 = y * 16;
      p.vy16 = p.size ? 12 + rnd(6) : 8 + rnd(5);
      const drift = rnd(4) - 1;
      p.vx16 = left ? -drift : drift;
      p.frame = rnd(c.petal_frames);
      p.spin = 3 + rnd(4);
      p.age = rnd(256);
      p.draw_x = x + kPetalSway[(p.age >> 3) & 15];
      p.draw_y = y;
    }

    UpdatePetals() {
      const c = this.chr_;
      if (!c.petals) return;
      const bottom = this.CH;
      for (let i = 0; i < this.petals_.length; i++) {
        const p = this.petals_[i];
        p.x16 += p.vx16;
        p.y16 += p.vy16;
        p.age = (p.age + 1) & 255;
        if (p.age % p.spin === 0) p.frame = (p.frame + 1) % c.petal_frames;
        const x = (p.x16 >> 4) + kPetalSway[(p.age >> 3) & 15];
        const y = p.y16 >> 4;
        if (y >= bottom || x < -24 || x > W + 8) { this.SpawnPetal(i, false); continue; }
        p.draw_x = x;
        p.draw_y = y;
      }
    }

    // --- ears ------------------------------------------------------------------------------------
    ResetEars() {
      this.ears_ = [newEar(), newEar()];
      this.ear_script_ = null;
      this.ear_last_ms_ = 0;
      this.ear_last_expr_ = -1;
      this.ear_next_idle_ms_ = this.now + 6000 + rnd(6000);
    }

    StartEarScript(name, gain, mirror) {
      this.ear_script_ = this.M.ear_scripts[name];
      this.ear_script_name_ = name;
      this.ear_script_gain_ = gain;
      this.ear_script_mirror_ = mirror ? 1 : 0;
      this.ear_script_t0_ = this.now;
    }

    TwitchEars() {
      if (!this.chr_.ears.length) return false;
      this.StartEarScript('kEarShake', 1.0, false);
      this.ear_next_idle_ms_ = this.now + 8000 + rnd(6000);
      return true;
    }

    UpdateEars() {
      const c = this.chr_;
      const n = Math.min(c.ears.length, kMaxEars);
      if (n === 0) return;
      const now = this.now;
      let dt_ms = now - this.ear_last_ms_;
      if (this.ear_last_ms_ === 0) {
        for (let e = 0; e < n; e++) {
          for (let i = 0; i < c.ears[e].deco.length && i < 4; i++) {
            this.ears_[e].anchor_x[i] = c.ears[e].deco[i].x;
            this.ears_[e].anchor_vx[i] = 0;
          }
        }
        dt_ms = kMotionTickMs;
      }
      this.ear_last_ms_ = now;
      if (dt_ms > 200) dt_ms = kMotionTickMs;

      const pose = [0, 0];
      let tau_ms = 200;
      const expr = this.expr_index_ >= 0 ? c.exprs[this.expr_index_].name : null;
      if (expr !== null) {
        const p = this.M.ear_poses[expr];
        if (p) { pose[0] = p[0] * kDeg; pose[1] = p[1] * kDeg; tau_ms = p[2]; }
      } else if (this.current_emotion_ === 'sleepy') {
        pose[0] = pose[1] = 10 * kDeg;
        tau_ms = 600;
      } else if (this.ambient_mode_ === 2) {
        pose[0] = pose[1] = -3 * kDeg;
        tau_ms = 150;
      }
      if (this.expr_index_ !== this.ear_last_expr_) {
        this.ear_last_expr_ = this.expr_index_;
        if (expr !== null && this.ear_script_ === null) {
          const mirror = rbit();
          if (expr === 'happy') this.StartEarScript('kEarPerk', 0.8, false);
          else if (expr === 'surprised') this.StartEarScript('kEarPerk', 1.2, false);
          else if (expr === 'wink') this.StartEarScript('kEarFlick', 1.0, mirror);
          else if (expr === 'pout') this.StartEarScript('kEarDoubleFlick', 0.9, mirror);
          else if (expr === 'shy') this.StartEarScript('kEarFlick', 0.6, mirror);
        }
      }

      if (this.ear_script_ === null && now - this.ear_next_idle_ms_ >= 0 && this.idleTwitch !== false) {
        const r = rnd(100);
        const mirror = rbit();
        const gain = 0.7 + rnd(31) * 0.01;
        if (r < 45) this.StartEarScript('kEarFlick', gain, mirror);
        else if (r < 70) this.StartEarScript('kEarDoubleFlick', gain, mirror);
        else if (r < 85) this.StartEarScript('kEarBoth', gain, mirror);
        else this.StartEarScript('kEarAlternate', gain, mirror);
        this.ear_next_idle_ms_ = now + 5000 + rnd(9000);
      }

      const steps = Math.max(1, Math.floor((dt_ms + 4) / 5));
      const h = dt_ms * 0.001 / steps;
      const pose_k = Math.min(1, h * 1000 / tau_ms);
      let script_end = 0;
      for (let k = 0; k < steps; k++) {
        const kick = [0, 0];
        if (this.ear_script_ !== null) {
          const t = now - dt_ms + Math.floor((k + 1) * dt_ms / steps) - this.ear_script_t0_;
          for (const st of this.ear_script_) {
            const [at_ms, hold_ms, ears, deg] = st;
            script_end = Math.max(script_end, at_ms + hold_ms);
            if (t >= at_ms && t < at_ms + hold_ms) {
              const m = this.ear_script_mirror_ ? (((ears & 1) << 1) | ((ears >> 1) & 1)) : ears;
              for (let e = 0; e < n; e++) if (m & (1 << e)) kick[e] += deg * kDeg * this.ear_script_gain_;
            }
          }
        }
        for (let e = 0; e < n; e++) {
          const E = this.ears_[e];
          const r = c.ears[e];
          E.pose += (pose[e] - E.pose) * pose_k;
          const target = Math.min(this.kEarMaxOut, Math.max(-this.kEarMaxOut, E.pose + kick[e]));
          EarSpring(E, 'base', 'base_v', target, this.kEarRootHz, this.kEarRootZeta, h);
          E.base = Math.min(this.kEarMaxOut, Math.max(-this.kEarMaxOut, E.base));
          EarSpring(E, 'tip', 'tip_v', E.base, this.kEarTipHz, this.kEarTipZeta, h);
          E.tip = Math.min(E.base + this.kEarTipMaxBend, Math.max(E.base - this.kEarTipMaxBend, E.tip));

          const sb = r.out_sign * E.base;
          const stt = r.out_sign * E.tip;
          const lx = r.tip_x - r.pivot_x, ly = r.tip_y - r.pivot_y;
          const inv_l2 = 1 / (lx * lx + ly * ly);
          for (let i = 0; i < r.deco.length && i < 4; i++) {
            const d = r.deco[i];
            const vx = d.x - r.pivot_x, vy = d.y - r.pivot_y;
            const s = Math.min(1, Math.max(0, (vx * lx + vy * ly) * inv_l2));
            const th = sb + (stt - sb) * s;
            const x = r.pivot_x + Math.cos(th) * vx - Math.sin(th) * vy;
            const vel = (x - E.anchor_x[i]) / h;
            const acc = (vel - E.anchor_vx[i]) / h;
            E.anchor_x[i] = x;
            E.anchor_vx[i] = vel;
            const hang = d.hang / 255;
            const w = kTwoPi * d.hz10 * 0.1;
            const z = d.zeta100 * 0.01;
            E.deco_v[i] += (w * w * ((1 - hang) * sb - E.deco[i]) - 2 * z * w * E.deco_v[i] +
                            acc / this.kEarPendulumPx) * h;
            E.deco[i] = Math.min(0.4, Math.max(-0.4, E.deco[i] + E.deco_v[i] * h));
          }
        }
      }
      if (this.ear_script_ !== null && now - this.ear_script_t0_ > script_end + 20) this.ear_script_ = null;

      // Warp constants for WarpEarsChunk().
      for (let e = 0; e < n; e++) {
        const E = this.ears_[e];
        const r = c.ears[e];
        E.sb = r.out_sign * E.base;
        E.st = r.out_sign * E.tip;
        let biggest = Math.max(Math.abs(E.sb), Math.abs(E.st));
        const lx = r.tip_x - r.pivot_x, ly = r.tip_y - r.pivot_y;
        const inv_l2 = 1 / (lx * lx + ly * ly);
        for (let i = 0; i < r.deco.length && i < 4; i++) {
          const d = r.deco[i];
          const vx = d.x - r.pivot_x, vy = d.y - r.pivot_y;
          const s = Math.min(1, Math.max(0, (vx * lx + vy * ly) * inv_l2));
          const th = E.sb + (E.st - E.sb) * s;
          E.fx[i] = r.pivot_x + Math.cos(th) * vx - Math.sin(th) * vy;
          E.fy[i] = r.pivot_y + Math.sin(th) * vx + Math.cos(th) * vy;
          E.dcos[i] = Math.cos(E.deco[i]);
          E.dsin[i] = Math.sin(E.deco[i]);
          biggest = Math.max(biggest, Math.abs(E.deco[i]));
        }
        E.active = biggest > 0.002;
      }
    }

    // --- rendering -------------------------------------------------------------------------------
    // Composes the face container (240 x CH) exactly as LVGL would, then runs the pre-flush warps.
    Render() {
      const c = this.chr_;
      const CH = this.CH;
      const fb = new Uint16Array(W * CH);
      const base = img(c.base).px;
      fb.set(base.subarray(0, W * CH));
      const blit = (dsc, x0, y0) => {
        const s = img(dsc);
        if (!s) return;
        for (let y = 0; y < s.h; y++) {
          const yy = y0 + y;
          if (yy < 0 || yy >= CH) continue;
          for (let x = 0; x < s.w; x++) {
            const xx = x0 + x;
            if (xx < 0 || xx >= W) continue;
            const a = s.alpha ? s.alpha[y * s.w + x] : 255;
            if (a === 255) fb[yy * W + xx] = s.px[y * s.w + x];
            else if (a > 0) fb[yy * W + xx] = Mix565(s.px[y * s.w + x], fb[yy * W + xx], a);
          }
        }
      };
      if (c.bangs) {
        blit(c.bangs[this.hair_level_bangs_ + 2], c.bangs_x, c.bangs_y);
        blit(c.locks_l[this.hair_level_locks_ + 2], c.locks_l_x, c.locks_l_y);
        blit(c.locks_r[this.hair_level_locks_ + 2], c.locks_r_x, c.locks_r_y);
      }
      blit(this.EyesSprite(), c.eyes_x, c.eyes_y);
      blit(this.mouthSprite, c.mouth_x, c.mouth_y);
      for (const p of this.petals_) blit(c.petals[p.size * c.petal_frames + p.frame], p.draw_x, p.draw_y);
      if (this.warps) {
        this.WarpEars(fb, CH, base);
        this.WarpHair(fb, CH);
      }
      if (this.showEarMap) this.TintEarMap(fb, CH);
      if (this.showHairBands) this.TintHairBands(fb, CH);
      return fb;
    }

    PillRect() {   // subtitle_box_: 232 x 32, bottom-mid, 4 px up
      const CH = this.CH;
      return { x1: 4, y1: CH - 4 - 32, x2: 4 + 231, y2: CH - 4 - 1 };
    }

    WarpHair(fb, CH) {
      const f = this.chr_.hair_flow;
      if (!f) return;
      const first = Math.max(0, f.y0);
      const last = Math.min(CH - 1, f.y0 + f.rows - 1);
      const keep = [];
      if (this.caption) {
        const r = this.PillRect();
        keep.push({ x1: r.x1 - 4, y1: r.y1 - 4, x2: r.x2 + 4, y2: r.y2 + 4 });
      }
      const unit = 1 / f.amp_unit;
      for (let y = first; y <= last; y++) {
        const r = y - f.y0;
        const amp = f.amp[r];
        if (amp === 0) continue;
        const a = amp * unit;
        const py = f.y0 + r;
        const billow = this.flow_billow_ - py * (1 / 140);
        const flutter = this.flow_flutter_ - py * (1 / 48);
        const out_l = a * (0.75 * this.flow_gust_ * (0.5 + 0.5 * SinTurns(billow)) +
                           0.22 * SinTurns(flutter) - 0.5 * this.flow_wind_);
        const out_r = a * (0.75 * this.flow_gust_ * (0.5 + 0.5 * SinTurns(billow + 0.3)) +
                           0.22 * SinTurns(flutter + 0.38) + 0.5 * this.flow_wind_);
        for (let side = 0; side < 2; side++) {
          const o = r * 10 + side * 5;
          const p0 = f.spans[o], p1 = f.spans[o + 1], p2 = f.spans[o + 2];
          const b0 = f.spans[o + 3], b1 = f.spans[o + 4];
          const k = side === 0 ? out_l : -out_r;
          if (Math.abs(k) < 0.03) continue;
          let lo = Math.max(p0 + 1, 0);
          let hi = Math.min(p2 - 1, W - 1);
          for (let i = 0; i < keep.length && lo <= hi; i++) {
            const e = keep[i];
            if (y < e.y1 || y > e.y2 || e.x2 < lo || e.x1 > hi) continue;
            if (e.x1 <= lo && e.x2 >= hi) lo = hi + 1;
            else if (e.x1 <= lo) lo = e.x2 + 1;
            else hi = e.x1 - 1;
          }
          WarpSpan(fb, y * W, p0, p1, p2, b0, b1, f.feather, k, lo, hi);
        }
      }
    }

    WarpEars(fb, CH, base) {
      const c = this.chr_;
      const n = Math.min(c.ears.length, kMaxEars);
      for (let e = 0; e < n; e++) {
        const E = this.ears_[e];
        if (!E.active) continue;
        const r = c.ears[e];
        if (!r._w) { r._w = b64bytes(r.weight); r._d = b64bytes(r.deco_map); }
        const x_lo = Math.max(0, r.box_x), x_hi = Math.min(W - 1, r.box_x + r.box_w - 1);
        const y_lo = Math.max(0, r.box_y), y_hi = Math.min(CH - 1, r.box_y + r.box_h - 1);
        const cx = r.pivot_x, cy = r.pivot_y;
        const lx = r.tip_x - r.pivot_x, ly = r.tip_y - r.pivot_y;
        const inv_l2 = 1 / (lx * lx + ly * ly);
        const bend = E.st - E.sb;
        for (let py = y_lo; py <= y_hi; py++) {
          const bi = (py - r.box_y) * r.box_w - r.box_x;
          const vy = py - cy;
          for (let pxx = x_lo; pxx <= x_hi; pxx++) {
            const wv = r._w[bi + pxx];
            const dv = r._d[bi + pxx];
            if (wv === 0 && (dv & 63) === 0) continue;
            if (fb[py * W + pxx] !== base[py * W + pxx]) continue;   // something drawn over her
            const vx = pxx - cx;
            const s = Math.min(1, Math.max(0, (vx * lx + vy * ly) * inv_l2));
            const th = E.sb + bend * s;
            const th2 = th * th;
            const co = 1 - 0.5 * th2;
            const sn = th * (1 - th2 * (1 / 6));
            const wd = (dv & 63) * (1 / 63);
            const k = (1 - wd) * wv * (1 / 255);
            let sx = pxx + k * (cx + co * vx + sn * vy - pxx);
            let sy = py + k * (cy - sn * vx + co * vy - py);
            if (wd > 0) {
              const j = dv >> 6;
              const d = r.deco[j];
              const qx = pxx - E.fx[j], qy = py - E.fy[j];
              sx += wd * (d.x + E.dcos[j] * qx + E.dsin[j] * qy - pxx);
              sy += wd * (d.y - E.dsin[j] * qx + E.dcos[j] * qy - py);
            }
            sx = Math.min(W - 1, Math.max(0, sx));
            sy = Math.min(H - 1, Math.max(0, sy));
            const qx32 = Math.trunc(sx * 32), qy32 = Math.trunc(sy * 32);
            const ix = qx32 >> 5, iy = qy32 >> 5;
            const ix1 = Math.min(ix + 1, W - 1), iy1 = Math.min(iy + 1, H - 1);
            const fx = qx32 & 31, fy = qy32 & 31;
            const s0 = iy * W, s1 = iy1 * W;
            fb[py * W + pxx] = Lerp565(Lerp565(base[s0 + ix], base[s0 + ix1], fx),
                                       Lerp565(base[s1 + ix], base[s1 + ix1], fx), fy);
          }
        }
      }
    }

    // Debug views: red = follows the ear, green = an ornament's own swing, blue = hair wind bands.
    TintEarMap(fb, CH) {
      for (const r of this.chr_.ears) {
        if (!r._w) { r._w = b64bytes(r.weight); r._d = b64bytes(r.deco_map); }
        for (let j = 0; j < r.box_h; j++) {
          const y = r.box_y + j;
          if (y >= CH) break;
          for (let i = 0; i < r.box_w; i++) {
            const x = r.box_x + i;
            const wv = r._w[j * r.box_w + i], dv = r._d[j * r.box_w + i] & 63;
            if (!wv && !dv) continue;
            fb[y * W + x] = Mix565(RGB(255, 0, 0), fb[y * W + x], Math.round(wv * 0.55)) ;
            if (dv) fb[y * W + x] = Mix565(RGB(0, 255, 0), fb[y * W + x], Math.round(dv * 2.2));
          }
        }
      }
    }

    TintHairBands(fb, CH) {
      const f = this.chr_.hair_flow;
      if (!f) return;
      const maxAmp = Math.max(...f.amp) || 1;
      for (let r = 0; r < f.rows; r++) {
        const y = f.y0 + r;
        if (y >= CH || f.amp[r] === 0) continue;
        const a = Math.round(140 * f.amp[r] / maxAmp);
        for (let side = 0; side < 2; side++) {
          const o = r * 10 + side * 5;
          for (let x = Math.max(0, f.spans[o] + 1); x < Math.min(W, f.spans[o + 2]); x++) {
            if (x >= f.spans[o + 3] && x <= f.spans[o + 4]) continue;
            fb[y * W + x] = Mix565(RGB(40, 120, 255), fb[y * W + x], a);
          }
        }
      }
    }

    EarState() {
      return this.ears_.slice(0, this.chr_.ears.length).map((E) => ({
        base: E.base / kDeg, tip: E.tip / kDeg, deco: E.deco.slice(0, 2).map((d) => d / kDeg) }));
    }
  }

  function RGB(r, g, b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

  // LVGL's RGB565 blend of fg over bg at opacity a (0..255).
  function Mix565(fg, bg, a) {
    const fr = fg >> 11, fg6 = (fg >> 5) & 63, fb = fg & 31;
    const br = bg >> 11, bg6 = (bg >> 5) & 63, bb = bg & 31;
    const r = Math.round((fr * a + br * (255 - a)) / 255);
    const g = Math.round((fg6 * a + bg6 * (255 - a)) / 255);
    const b = Math.round((fb * a + bb * (255 - a)) / 255);
    return (r << 11) | (g << 5) | b;
  }

  // What the panel shows for an RGB565 frame, as RGBA bytes (from565 in face_lib.py).
  function ToRGBA(fb, out) {
    for (let i = 0; i < fb.length; i++) {
      const v = fb[i];
      const r5 = v >> 11, g6 = (v >> 5) & 63, b5 = v & 31;
      out[4 * i] = (r5 << 3) | (r5 >> 2);
      out[4 * i + 1] = (g6 << 2) | (g6 >> 4);
      out[4 * i + 2] = (b5 << 3) | (b5 >> 2);
      out[4 * i + 3] = 255;
    }
    return out;
  }

  root.EncoPreview = { Face, ToRGBA, W, H, kFaceTickMs, kMotionTickMs };
  if (typeof module !== 'undefined') module.exports = root.EncoPreview;
})(typeof window !== 'undefined' ? window : globalThis);
