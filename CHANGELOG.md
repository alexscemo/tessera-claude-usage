# Changelog

## 0.2.0
- A new look after Claude's own: each limit has a small caption in capitals, its percentage in the largest figures, a
  thin bar in a warm orange (amber from 75%, red from 90%) and a quieter line about when it resets.
- Claude's spark beside the session, animated while Claude Code is in use, as in Claude's desktop app.
- Needs a screen with plugin API 0.9 (Tessera 0.4.88 or later).

## 0.1.1
- Numbers more than 15 minutes old are shown as old ("Not updated"), for example when the computer is off or asleep.
- A plan without a weekly limit no longer shows a made-up 0% week: the week is left out.
- The screen redraws the tile only where something changed, not every second.

## 0.1.0
- First version: a tile and a card with your Claude Code plan limits (the session, the week and the week of one model),
  read from a sensor of Home Assistant that `tools/push_usage.py` writes from your computer.
