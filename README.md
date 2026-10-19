# Claude Code usage

A tile with your Claude Code plan limits, the same numbers as `/usage` in Claude Code:
- how much of the 5-hour session you have used, and when it resets;
- how much of the week you have used, and when it resets.

Tap the tile for a card with every limit, including the week of one model (Opus or Sonnet) on plans that have one. The
card also shows the day and time of each reset, and when the numbers last came in.

The tile reads **one sensor of Home Assistant**. A small script on the computer where you use Claude Code writes that
sensor every 5 minutes. Neither the screen nor the Tessera app talks to Anthropic.

## Set up

1. On the computer where you use Claude Code, with Python 3.9 or newer, copy `tools/push_usage.ini.example` to
   `tools/push_usage.ini`. Fill in Home Assistant's address and a long-lived access token. To make the token, in Home
   Assistant open your profile, then Security, then Long-lived access tokens, then Create token.
2. Run `python tools/push_usage.py --print`. It shows the numbers it would send, which should match `/usage` in Claude
   Code.
3. Run `python tools/push_usage.py` once. In Home Assistant, Developer tools > States now has
   `sensor.claude_code_usage`.
4. On Windows, run `python tools/push_usage.py --install-task` so Task Scheduler sends the numbers every 5 minutes. On
   macOS or Linux, run the script every 5 minutes with cron.
5. Add the plugin to a screen in Tessera. The screen builds once.
6. In Layout, place the **Claude Code usage** tile from the library's Plugins group (1x1 up to 2x2) and choose the
   sensor in the inspector.

## Good to know

- **Tile sizes:**
  - A 1x1 tile shows the session.
  - A wider tile shows the session and the week side by side.
  - A taller tile shows them one above the other, under the tile's name.
- **Colours:** the bar is a warm orange, amber from 75% and red from 90%; the percentage turns red from 90%.
- **Clawd:** Claude Code's mascot stands beside the session and blinks every few seconds. He walks on the spot while
  Claude Code is in use (the session rose since the numbers before), and goes grey when the numbers are old.
- **Resets:** once a reset time has passed, the tile counts that limit as 0% right away, without waiting for the next
  numbers.
- **When the numbers can't be read:** if Claude Code's login has expired, or Anthropic doesn't answer, the sensor keeps
  its last numbers and says they are old. The login renews the next time you use Claude Code. The script never renews it
  itself, because that would log Claude Code out. Numbers more than 15 minutes old are also shown as old, for example
  when the computer is off or asleep.
- **Where the numbers come from:** the address Claude Code itself uses for `/usage`. Anthropic doesn't document it, so a
  new version of Claude Code may change it. If that happens, the script logs the problem in `tools/push_usage.log`.
- **After Home Assistant restarts:** a sensor written this way is gone until the next send, at most 5 minutes later.
- **What it shares:** the script reads Claude Code's login from `~/.claude/.credentials.json`, uses it only to ask for
  the usage, and never sends it to Home Assistant.
- **Tested so far:** only on the plugin bench (`tools/preview.py`). It has not yet been on a real screen.
