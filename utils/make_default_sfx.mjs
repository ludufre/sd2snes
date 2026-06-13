// make_default_sfx.mjs — generate the 4 default menu navigation effects as MSU-1 PCM
// files in misc/ (sfx_cursor.pcm, sfx_confirm.pcm, sfx_back.pcm, sfx_error.pcm), so the
// release ships working menu sounds out of the box. The firmware reads these from
// /sd2snes/ at runtime (src/msu1.c menu_sfx_play); a missing file is just silent.
//
// The synthesis here is a BYTE-FOR-BYTE copy of the web Sound Creator's defaults
// (../sd2snes-landing/sounds/index.html: synth / defaultSfx / encodeMsuPcm), so
// "download without changing anything" in the editor reproduces these exact files.
// Keep the two in sync.
//
//   node utils/make_default_sfx.mjs            # writes misc/sfx_*.pcm
//
import { writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const SR = 44100; // MSU-1 PCM is fixed 44.1 kHz, 16-bit stereo
const ORDER = ["cursor", "confirm", "back", "error"];

function synth(segments, dur, opt) {
  opt = opt || {};
  const decay = opt.decay || 0.05, attack = opt.attack || 0.004, amp = opt.amp || 0.5;
  const harmonics = opt.harmonics || [1], tremolo = opt.tremolo || 0;
  const hnorm = harmonics.reduce((a, h) => a + 1 / h, 0);
  const n = Math.round(dur * SR), out = new Float32Array(n);
  let phase = 0;
  for (let i = 0; i < n; i++) {
    const t = i / SR;
    let f = segments[0].f;
    for (const seg of segments) if (t >= seg.t) f = seg.f;
    phase += (2 * Math.PI * f) / SR;
    let s = 0;
    for (const h of harmonics) s += Math.sin(phase * h) / h;
    s /= hnorm;
    let e = Math.min(1, t / attack) * Math.exp(-Math.max(0, t - attack) / decay);
    if (tremolo) e *= 0.7 + 0.3 * Math.sin(2 * Math.PI * tremolo * t);
    out[i] = s * e * amp;
  }
  return out;
}
function defaultSfx(key) {
  if (key === "cursor") return synth([{ t: 0, f: 1318 }], 0.05, { decay: 0.018, amp: 0.5 });
  if (key === "confirm") return synth([{ t: 0, f: 880 }, { t: 0.05, f: 1318 }], 0.16, { decay: 0.07, amp: 0.5 });
  if (key === "back") return synth([{ t: 0, f: 988 }, { t: 0.05, f: 659 }], 0.16, { decay: 0.07, amp: 0.5 });
  return synth([{ t: 0, f: 220 }], 0.24, { decay: 0.12, amp: 0.45, harmonics: [1, 3, 5], tremolo: 22 }); // error
}
// "MSU1" + u32 loop point (samples, LE) + interleaved 16-bit stereo @ 44.1 kHz.
function encodeMsuPcm(mono) {
  const PAD = 2048; // ~46 ms of trailing silence for the firmware's one-shot loop region
  const n = mono.length, total = n + PAD;
  const buf = new ArrayBuffer(8 + total * 4);
  const dv = new DataView(buf);
  dv.setUint8(0, 0x4d); dv.setUint8(1, 0x53); dv.setUint8(2, 0x55); dv.setUint8(3, 0x31); // "MSU1"
  dv.setUint32(4, n, true); // loop point = first sample of the trailing silence
  let o = 8;
  for (let i = 0; i < total; i++) {
    let s = i < n ? mono[i] : 0;
    if (s > 1) s = 1; else if (s < -1) s = -1;
    const v = s < 0 ? Math.round(s * 32768) : Math.round(s * 32767);
    dv.setInt16(o, v, true);
    dv.setInt16(o + 2, v, true);
    o += 4;
  }
  return Buffer.from(buf);
}

const miscDir = join(dirname(fileURLToPath(import.meta.url)), "..", "misc");
for (const key of ORDER) {
  const data = encodeMsuPcm(defaultSfx(key));
  const path = join(miscDir, `sfx_${key}.pcm`);
  writeFileSync(path, data);
  console.log(`wrote ${path} (${data.length} bytes)`);
}
