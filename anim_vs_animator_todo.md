# Animator vs Animator precision TODO

This checklist tracks frame-accurate validation against `anim_vs_animator.txt` and is intended for future refinement passes.

## Global checks

- [ ] Verify every act starts from the final physical pose and position of the prior act.
- [ ] Verify cursor path continuity at every act boundary.
- [ ] Verify all impact bursts use 6 rays and only last 2 frames.
- [ ] Verify bounce timing matches the 4-frame drop-and-return rule.
- [ ] Verify running uses the specified 2-pose, 2-frame cadence.
- [ ] Verify all screen flashes are exactly 1 frame.
- [ ] Verify selection boxes, handles, and delete-zone geometry match the script.

## Act 1 — Creation

- [ ] Blank opening lasts through the initial beat before cursor entry.
- [ ] Cursor drift from `(4,4)` to `(64,18)` matches the script timing.
- [ ] Head circle traces progressively rather than popping in.
- [ ] Torso, arms, and legs are drawn in the scripted order and timing.
- [ ] Selection, drag right, drag back, and rotation beats align with the storyboard.
- [ ] Final stillness before Act 2 is fully motionless.

## Act 2 — Awakening

- [ ] Head look-around completes in the initial 6-frame beat.
- [ ] Reach, contact, jerk escape, and stumble line up to the script timings.
- [ ] Second grab reads as a two-hand grip on the cursor.
- [ ] Upward yank, drop, and bounce match the prescribed motion.
- [ ] Tug-of-war oscillation timing and leg slides match the script.
- [ ] Final fling and skid match the leftward landing position.

## Act 3 — Escalation

- [ ] Marquee draws progressively from the cursor path around the figure.
- [ ] Figure dodge out of the marquee reads clearly before the drag completes.
- [ ] Handle swing reads as a 180-degree gymnast arc under the top-center handle.
- [ ] Fill splash is brief and then shakes off cleanly.
- [ ] Cursor retreat, chase, swoop, selection, fling, bounce, and recovery match the storyboard.

## Act 4 — Weapons and drawn objects

- [ ] Rectangle cage is drawn edge by edge, not as an instant outline.
- [ ] Right-wall punch opens a readable exit gap.
- [ ] Circle is traced progressively before being moved around the figure.
- [ ] Horizontal floor line is drawn as a live stroke.
- [ ] Jump, landing on the line, staff pickup, swing, dodge, erasure, and stumble are verified beat by beat.

## Act 5 — Tools as battlefield

- [ ] Initial wall and platform are drawn at the scripted coordinates.
- [ ] Punch-through and kick-through gaps appear at the intended heights.
- [ ] Eraser leg damage, hop, regeneration, and anger beat match the script.
- [ ] Push into the right border and border-gap redraw are visually clear.
- [ ] Left-edge run, toolbar climb, top-border balance, incoming cursor swoop, kick deflection, and drop are verified.

## Act 6 — Control panel chaos

- [ ] Timeline, stop, play, and playhead are positioned and timed correctly.
- [ ] Stop freezes and play resumes with exact flash timing.
- [ ] Stop/play stutter sequence alternates on the intended cadence.
- [ ] Left scrub, snap-back, right scrub, and fast-forward beats match the script.
- [ ] Canvas shake and cursor approach into Act 7 are smooth and continuous.

## Act 7 — Resolution

- [ ] Marquee punch/redraw exchange occurs three times with escalating speed.
- [ ] Delete zone appears at the scripted size and location.
- [ ] Drag-to-delete stretch increases progressively and remains connected.
- [ ] Border grip pose reads clearly before the final pull.
- [ ] Figure erasure proceeds left leg, left arm, torso, right side, then head.
- [ ] Cursor drift to center and disappearance are verified frame by frame.
