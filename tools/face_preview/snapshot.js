// Headless check of face_engine.js: renders frames to PNG without a browser.
//   node tools/face_preview/snapshot.js [character] [out_dir] [action] [ms,ms,...]
// action: shake | flick | expr:<name> | emo:<emotion> | none. Frames are taken at the given ms after the action.
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const here = __dirname;
globalThis.window = globalThis;
for (const f of ['data/manifest.js']) require(path.join(here, f));
for (const id of window.ENCO_MANIFEST.characters) require(path.join(here, 'data', id + '.js'));
const { Face, ToRGBA, W } = require(path.join(here, 'face_engine.js'));

const cid = process.argv[2] || 'fox';
const out = process.argv[3] || '.';
const action = process.argv[4] || 'shake';
const times = (process.argv[5] || '0,100,250,500,1000,1300').split(',').map(Number);

function png(file, rgba, w, h) {
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w * 4 + 1)] = 0;
    Buffer.from(rgba.buffer, y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1);
  }
  const crcTable = Array.from({ length: 256 }, (_, n) => {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    return c >>> 0;
  });
  const crc = (buf) => { let c = 0xffffffff; for (const b of buf) c = crcTable[(c ^ b) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const c = Buffer.alloc(4); c.writeUInt32BE(crc(td));
    return Buffer.concat([len, td, c]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr),
                                        chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
}

const face = new Face(window.ENCO_MANIFEST, window.ENCO_CHARS);
face.SetCharacter(cid);
face.ambient_mode_ = 0;
face.idleTwitch = false;
face.Advance(500);
if (action === 'shake') face.TwitchEars();
else if (action === 'flick') face.StartEarScript('kEarFlick', 1.0, false);
else if (action.startsWith('expr:')) face.ShowExpression(action.slice(5), 4000, true);
else if (action.startsWith('emo:')) face.UpdateRobotFaceEmotion(action.slice(4));
let t = 0;
fs.mkdirSync(out, { recursive: true });
for (const ms of times) {
  face.Advance(ms - t);
  t = ms;
  const fb = face.Render();
  const rgba = new Uint8Array(fb.length * 4);
  ToRGBA(fb, rgba);
  const file = path.join(out, `${cid}_${action.replace(':', '_')}_${ms}.png`);
  png(file, rgba, W, face.CH);
  console.log(file, JSON.stringify(face.EarState().map((e) => [e.base.toFixed(1), e.tip.toFixed(1)])));
}
