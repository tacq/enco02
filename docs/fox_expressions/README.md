# Fox (2号驾驶员) — Expression Gallery

All faces the `fox` character can show on the 240×294 LCD. Screenshots are rendered by the
local simulator (`tools/face_preview/`), which is a line-by-line port of `firmware/main/display.cpp`,
so they match what the device draws. Each frame is captured 1.2 s after the face is triggered,
once the ear pose has settled.

| | | | |
|:---:|:---:|:---:|:---:|
| <img src="neutral.png" width="180"><br>**neutral** | <img src="happy.png" width="180"><br>**happy** | <img src="sad.png" width="180"><br>**sad** | <img src="wink.png" width="180"><br>**wink** |
| <img src="pout.png" width="180"><br>**pout** | <img src="surprised.png" width="180"><br>**surprised** | <img src="angry.png" width="180"><br>**angry** | <img src="shy.png" width="180"><br>**shy** |
| <img src="thinking.png" width="180"><br>**thinking** | <img src="sleepy.png" width="180"><br>**sleepy** | | |

## What triggers each face

The AI backend sends an emotion string; `display.cpp` maps it to a face and holds it for 2.5 s
after she stops talking. Each face also sets an ear pose (degrees, + = droop/outward).

| Face | Emotions from the AI that select it | Ear pose (L / R) | Also used for |
|---|---|---|---|
| neutral | `neutral`, `relaxed`, `confident` (face left alone) | 0 / 0 | default |
| happy | `happy`, `laughing`, `funny`, `delicious` | −3 / −3 (perked) | idle & listening ambient |
| sad | `sad`, `crying` | +14 / +14 (drooped) | idle ambient |
| wink | `winking`, `silly`, `cool` | 0 / 0 | idle & listening ambient |
| pout | `kissy`, `loving` | +5 / +5 | idle ambient |
| surprised | `surprised`, `shocked` | −7 / −7 (alert) | idle & listening ambient |
| angry | `angry` | +11 / +11 (pinned back) | — |
| shy | `embarrassed` | +8 / +8 | idle & listening ambient |
| thinking | `thinking`, `confused` | +7 / −2 (asymmetric) | idle & listening ambient |
| sleepy | `sleepy` (eyes-shut sprite, not an expression slot) | relaxed droop | sleep mode |

## Regenerating these screenshots

```bash
python3 tools/face_preview/export.py          # refresh data/ from firmware assets
D=docs/fox_expressions
for a in none expr:happy expr:sad expr:wink expr:pout expr:surprised \
         expr:angry expr:shy expr:thinking emo:sleepy; do
  node tools/face_preview/snapshot.js fox $D $a 1200
done
# then rename fox_<action>_1200.png -> <name>.png
```

To browse them animated (blink, mouth, ear springs, hair flow), open
`tools/face_preview/index.html` in a browser and pick **fox**.
