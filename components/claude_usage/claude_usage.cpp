#include "claude_usage.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace esphome::claude_usage {

using tessera::Font;
using tessera::Tone;
namespace ui = tessera::ui;

// The three limits of a Claude plan, in the order the card shows them: the 5-hour session, the week, and the week of
// one model (Opus or Sonnet) on plans that have one.
enum { SESSION = 0, WEEK = 1, MODEL = 2, LIMITS = 3 };

// From BUSY_PCT the bar is amber (nearly used up), from ALERT_PCT the percentage is red as well.
static constexpr int BUSY_PCT = 75;
static constexpr int ALERT_PCT = 90;
// Numbers older than this are drawn as old: the computer that sends them is off, asleep or failing.
static constexpr uint32_t MAX_AGE = 15 * 60;

// Shows or hides a part, touching it only when that changes: LVGL redraws a part whose flag is set again.
static void set_hidden(lv_obj_t *o, bool hidden) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hidden) return;
  if (hidden)
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// What sensor.claude_code_usage says (README.md): one percentage and one moment of reset per limit.
struct Usage {
  float pct[LIMITS] = {-1, -1, -1};
  uint32_t resets_at[LIMITS] = {0, 0, 0};
  uint32_t updated_at = 0;
  std::string model_name;
  const char *blocked = nullptr;  // why the sensor itself gives nothing to show, a key of translations
  bool stale = false;
  bool working = false;  // the session rose since the numbers before: Claude Code is in use

  void read(JsonObjectConst data) {
    JsonObjectConst a = data["attributes"];
    const float before = pct[SESSION];
    const uint32_t sent = updated_at;
    static const char *const PCT[LIMITS] = {"session_pct", "week_pct", "model_pct"};
    static const char *const AT[LIMITS] = {"session_resets_at", "week_resets_at", "model_resets_at"};
    for (int i = 0; i < LIMITS; ++i) {
      pct[i] = a[PCT[i]] | -1.0f;
      resets_at[i] = a[AT[i]] | 0u;
    }
    updated_at = a["updated_at"] | 0u;
    model_name = std::string(a["model_name"] | "");
    stale = a["stale"] | false;
    // Judged once per sending of the computer, not again on a repeat of the same numbers.
    if (updated_at != sent) working = before >= 0 && pct[SESSION] > before;
    // Before a person has chosen the sensor the tile gets {}; a sensor without a value says unavailable or unknown.
    const std::string state = std::string(data["state"] | ""), wait = std::string(data["wait"] | "");
    blocked = data["state"].isNull() || wait == "wrong_entity"                  ? "choose_sensor"
              : !wait.empty() || state == "unavailable" || state == "unknown" ? "waiting"
                                                                                : nullptr;
  }

  // Why there is nothing to show (a key of translations, part screen), or nullptr.
  const char *message(uint32_t now) const { return blocked ? blocked : !now || pct[SESSION] < 0 ? "waiting" : nullptr; }

  // The numbers are old: the computer said so, or it has sent none for a while.
  bool old(uint32_t now) const { return stale || (updated_at && now > updated_at + MAX_AGE); }

  // The percentage now: once the moment of reset has passed the limit starts again from nothing, even before the
  // computer sends the new numbers.
  int percent(int limit, uint32_t now) const {
    if (pct[limit] < 0) return -1;
    if (resets_at[limit] && now >= resets_at[limit]) return 0;
    return static_cast<int>(std::lround(pct[limit]));
  }
};

// "in 42 min", "in 3 h 10 min", "in 2 d 5 h" until a moment; "" when it has passed or is not known.
static std::string countdown(const tessera::Plugin *plugin, uint32_t at, uint32_t now) {
  if (!at || !now || at <= now) return "";
  const uint32_t s = at - now;
  if (s < 3600) return tessera::format(plugin->text("in_minutes"), static_cast<long>((s + 59) / 60));
  // The larger unit, and the smaller one only when it is not 0: "in 3 h", "in 3 h 10 min".
  const bool days = s >= 86400;
  const long big = days ? s / 86400 : s / 3600, small = days ? s % 86400 / 3600 : s % 3600 / 60;
  if (!small) return tessera::format(plugin->text(days ? "in_days_whole" : "in_hours_whole"), big);
  const std::string t = tessera::fill(plugin->text(days ? "in_days" : "in_hours"), days ? "d" : "h", std::to_string(big));
  return tessera::fill(t.c_str(), days ? "h" : "m", std::to_string(small));
}

// "14:30", or with the day when it is not today: "Thu 15 Oct 09:00".
static std::string moment_text(uint32_t at) {
  const std::string time = tessera::clock_text(at);
  return tessera::days_from_today(at) != 0 ? tessera::date_text(at) + " " + time : time;
}

// "Resets 14:30 · in 2 h 10 min".
static std::string resets_text(const tessera::Plugin *plugin, uint32_t at, uint32_t now) {
  if (!at || !now || at <= now) return "";
  return tessera::fill(plugin->text("resets"), "time", moment_text(at)) + " · " + countdown(plugin, at, now);
}

static std::string name_of(const tessera::Plugin *plugin, const Usage &u, int limit) {
  if (limit == SESSION) return plugin->text("session");
  std::string week = plugin->text("week");
  if (limit == MODEL && !u.model_name.empty()) week += " · " + u.model_name;
  return week;
}

// The caption of a limit in capitals, as Claude's own pages set their small labels ("WEEK · OPUS"). Only ASCII letters
// change: a byte of a longer UTF-8 character (the dot, an accent) stays as it is.
static std::string caps(std::string s) {
  for (char &ch : s)
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
  return s;
}

// One limit, in four lines after DESIGN.md: a small caption with its name, its percentage in the largest figures, a
// thin bar in Claude's warm orange, and a quiet line about when it resets. As large as its room lets it be. The tile and
// the card both draw with it.
class Meter {
 public:
  void make(lv_obj_t *parent) {
    name_ = ui::label(parent, Font::BODY, theme::MUTED);
    pct_ = ui::label(parent, Font::DIGITS, theme::INK);
    track_ = ui::block(parent, theme::TRACK);
    fill_ = ui::block(parent, ACCENT);
    when_ = ui::label(parent, Font::BODY, theme::SUBTLE);
    for (lv_obj_t *l : {name_, pct_, when_}) lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
  }

  // The height this needs with `font` for the percentage, with or without the caption over it.
  static int height(Font font, int bar, bool caption = true) {
    const int body = ui::line_height(Font::BODY);
    return (caption ? body + gap() : 0) + ui::line_height(font) + bar + 2 * gap() + body;
  }

  // Lay it out in x, y, w, h: the percentage takes the largest font that fits, the caption goes when even the smallest
  // does not fit with it, and the whole sits in the middle of the room.
  void place(int x, int y, int w, int h, int bar) {
    w = std::max(1, w);
    bool caption = true;
    for (bool c : {true, false}) {
      caption = c;
      for (Font f : {Font::DIGITS, Font::VALUE, Font::HEADLINE, Font::TITLE}) {
        font_ = f;
        if (height(f, bar, c) <= h && ui::text_width("100%", f) <= w) break;
      }
      if (height(font_, bar, c) <= h) break;
    }
    caption_ = caption;
    const int body = ui::line_height(Font::BODY), line = ui::line_height(font_);
    int at = y + std::max(0, (h - height(font_, bar, caption)) / 2);
    lv_obj_set_size(name_, w, body);
    lv_obj_set_pos(name_, x, at);
    if (caption) at += body + gap();
    figure_y_ = at;
    ui::set_font(pct_, font_);
    lv_obj_set_size(pct_, w, line);
    lv_obj_set_pos(pct_, x, at);
    at += line + gap();
    x_ = x;
    w_ = w;
    bar_ = bar;
    bar_y_ = at;
    lv_obj_set_size(track_, w, bar);
    lv_obj_set_pos(track_, x, at);
    lv_obj_set_size(when_, w, body);
    lv_obj_set_pos(when_, x, at + bar + gap());
    painted_pct_ = -1;  // the bar is measured again on the next paint
  }

  // The texts and the bar; pct < 0 hides it all. `old`: the numbers are not fresh, so they are drawn quieter.
  void paint(const std::string &name, int pct, const std::string &when, bool old) {
    if (pct < 0) {
      for (lv_obj_t *o : {name_, pct_, track_, fill_, when_}) set_hidden(o, true);
      painted_pct_ = -1;
      return;
    }
    for (lv_obj_t *o : {pct_, track_, when_}) set_hidden(o, false);
    set_hidden(name_, !caption_);
    ui::set_text(name_, caps(name));
    ui::set_text(pct_, std::to_string(pct) + "%");
    // Measured and placed only when the percentage changed, not on every tick.
    if (pct != painted_pct_) {
      painted_pct_ = pct;
      const int fw = std::min(w_, w_ * std::clamp(pct, 0, 100) / 100);
      set_hidden(fill_, fw < bar_);  // too short to draw as a rounded bar
      lv_obj_set_size(fill_, std::max(1, fw), bar_);
      lv_obj_set_pos(fill_, x_, bar_y_);
    }
    ui::set_tone(pct_, !old && pct >= ALERT_PCT ? Tone::ALERT : Tone::NORMAL, old ? theme::MUTED : theme::INK);
    ui::set_text(when_, when);
    // The bar warm orange, amber when the limit is nearly used up, red at the end, grey when the numbers are old.
    const Tone tone = old ? Tone::NORMAL : pct >= ALERT_PCT ? Tone::ALERT : pct >= BUSY_PCT ? Tone::BUSY : Tone::NORMAL;
    ui::set_fill(fill_, tone, old ? theme::MUTED : ACCENT);
  }

  void on_theme() {
    ui::set_color(name_, theme::MUTED);
    ui::set_color(when_, theme::SUBTLE);
    lv_obj_set_style_bg_color(track_, ui::color(theme::TRACK), 0);
  }

  static int gap() { return ui::px(ui::large() ? 5 : 3); }

  // Where the spark goes: at the right end of the percentage's line, about as high as its figures.
  void spark_box(int &x, int &y, int &size) const {
    const int line = ui::line_height(font_);
    size = line * 7 / 10;
    x = x_ + w_ - size;
    y = figure_y_ + (line - size) / 2;
  }

 private:
  // No theme role is Claude's coral (DESIGN.md); the solar orange is the nearest, the same in light and dark.
  static constexpr theme::Role ACCENT = theme::ENERGY_SOLAR;

  Font font_ = Font::DIGITS;
  bool caption_ = true;
  int x_ = 0, w_ = 1, bar_ = 1, bar_y_ = 0, figure_y_ = 0, painted_pct_ = -1;
  lv_obj_t *name_{}, *pct_{}, *track_{}, *fill_{}, *when_{};
};

// Claude's spark: twelve rays of uneven length from one centre, drawn as one line that goes out to each tip and back.
// It stands still, and while Claude Code works its rays breathe and it turns slowly, as the spark of Claude's desktop
// app does while it answers. LVGL moves it frame by frame, as the core moves its own alarm tile.
class Spark {
 public:
  ~Spark() {
    if (line_) lv_anim_delete(line_, nullptr);  // no frame may reach this after it is gone
  }

  void make(lv_obj_t *parent) {
    line_ = lv_line_create(parent);
    lv_obj_remove_flag(line_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_rounded(line_, true, 0);
    on_theme();
  }

  void place(int x, int y, int size) {
    size_ = std::max(6, size);
    width_ = std::max(2, size_ / 8);
    lv_obj_set_size(line_, size_, size_);
    lv_obj_set_pos(line_, x, y);
    lv_obj_set_style_line_width(line_, width_, 0);
    draw(phase_);
  }

  // Grey when the numbers are old, moving while Claude Code works, gone while there are no numbers to stand beside.
  void paint(bool working, bool old, bool shown) {
    set_hidden(line_, !shown);
    working = working && shown;
    const theme::Role role = old ? theme::MUTED : theme::ENERGY_SOLAR;
    if (role != role_) {
      role_ = role;
      on_theme();
    }
    if (working == working_) return;
    working_ = working;
    lv_anim_delete(line_, nullptr);
    if (!working) {
      draw(0);  // at rest, the spark as Claude draws it
      return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, line_);
    lv_anim_set_values(&a, 0, TURN);
    lv_anim_set_duration(&a, 12000);
    lv_anim_set_custom_exec_cb(&a, frame);
    lv_anim_set_user_data(&a, this);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
  }

  void on_theme() { lv_obj_set_style_line_color(line_, ui::color(role_), 0); }

 private:
  static constexpr int RAYS = 12, TURN = 3600;  // the phase in tenths of a degree: one turn a period

  static void frame(lv_anim_t *a, int32_t phase) { static_cast<Spark *>(lv_anim_get_user_data(a))->draw(phase); }

  void draw(int32_t phase) {
    // The rays' lengths, and each a little off its even angle, so it reads as drawn by hand as Claude's own.
    static constexpr float LENGTH[RAYS] = {1.0f, 0.74f, 0.92f, 0.68f, 0.97f, 0.8f, 0.94f, 0.7f, 1.0f, 0.78f, 0.9f, 0.72f};
    static constexpr float SKEW[RAYS] = {0.0f, 0.06f, -0.04f, 0.05f, -0.03f, 0.04f, 0.0f, -0.05f, 0.03f, -0.04f, 0.05f, -0.02f};
    constexpr float PI = 3.14159265f;
    phase_ = phase;
    const float centre = size_ / 2.0f, reach = centre - width_ / 2.0f, t = phase * 2 * PI / TURN;
    for (int i = 0; i < RAYS; ++i) {
      // Moving: one turn a period, and each ray breathes four times in it, each at its own moment.
      const float breath = working_ ? 0.8f + 0.2f * sinf(4 * t + i * 1.9f) : 1.0f;
      const float angle = i * 2 * PI / RAYS + SKEW[i] + (working_ ? t : 0), len = reach * LENGTH[i] * breath;
      points_[2 * i] = {(lv_value_precise_t) centre, (lv_value_precise_t) centre};
      points_[2 * i + 1] = {(lv_value_precise_t) (centre + len * cosf(angle)),
                            (lv_value_precise_t) (centre + len * sinf(angle))};
    }
    lv_line_set_points(line_, points_, 2 * RAYS);
  }

  lv_obj_t *line_{};
  lv_point_precise_t points_[2 * RAYS]{};
  theme::Role role_ = theme::ENERGY_SOLAR;
  bool working_ = false;
  int32_t phase_ = 0;
  int size_ = 6, width_ = 2;
};

// The tile. 1x1: the session alone. Wider: the session and the week side by side. Taller: one above the other, under a
// header with the tile's name. The numbers change every few minutes, the countdowns every second.
class UsageTile : public tessera::Tile {
 public:
  explicit UsageTile(const tessera::Plugin *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
    const int width = c.width, height = c.height, bar = ui::px(ui::large() ? 6 : 4);
    const std::string name = c.name;
    entity_ = c.entity;
    tile_ = c.tile;
    count_ = c.columns >= 2 || c.rows >= 2 ? 2 : 1;
    const bool stacked = c.rows >= 2;
    const int head = ui::line_height(Font::TITLE) + Meter::gap();

    message_ = ui::label(c.parent, Font::BODY_LARGE, theme::MUTED);
    lv_obj_set_size(message_, width, ui::line_height(Font::BODY_LARGE));
    lv_obj_set_pos(message_, 0, (height - ui::line_height(Font::BODY_LARGE)) / 2);
    lv_obj_set_style_text_align(message_, LV_TEXT_ALIGN_CENTER, 0);
    // The header only where the two limits under it still get large figures.
    if (stacked && height - head >= 2 * Meter::height(Font::HEADLINE, bar) + Meter::gap()) {
      head_ = ui::label(c.parent, Font::TITLE, theme::INK);
      lv_obj_set_size(head_, width, ui::line_height(Font::TITLE));
      lv_obj_set_pos(head_, 0, 0);
      lv_label_set_long_mode(head_, LV_LABEL_LONG_CLIP);
      ui::set_text(head_, name.empty() ? plugin_->text("header") : name);
    }

    const int top = head_ ? head : 0;
    const int col_gap = ui::px(ui::large() ? 16 : 10);
    for (int i = 0; i < count_; ++i) {
      meters_[i].make(c.parent);
      if (count_ == 1)
        meters_[i].place(0, 0, width, height, bar);
      else if (stacked) {
        const int h = (height - top - Meter::gap()) / 2;
        meters_[i].place(0, top + i * (h + Meter::gap()), width, h, bar);
      } else {
        const int w = (width - col_gap) / 2;
        meters_[i].place(i * (w + col_gap), 0, w, height, bar);
      }
    }
    int x, y, size;
    meters_[0].spark_box(x, y, size);
    spark_.make(c.parent);
    spark_.place(x, y, size);
  }

  void on_state(JsonObjectConst data) override { usage_.read(data); }

  void on_tick(uint32_t now) override {
    const char *why = usage_.message(now);
    ui::set_text(message_, why ? plugin_->text(why) : "");
    for (int i = 0; i < count_; ++i) {
      if (why) {
        meters_[i].paint("", -1, "", false);
        continue;
      }
      // When it resets, or that the numbers are old: the tile has room for one line under the bar.
      const bool old = usage_.old(now);
      const std::string when = old ? plugin_->text("old") : countdown(plugin_, usage_.resets_at[i], now);
      meters_[i].paint(name_of(plugin_, usage_, i), usage_.percent(i, now), when, old);
    }
    if (head_) set_hidden(head_, why != nullptr);
    spark_.paint(usage_.working && !usage_.old(now), usage_.old(now), !why);
  }

  void on_theme() override {
    ui::set_color(message_, theme::MUTED);
    if (head_) ui::set_color(head_, theme::INK);
    for (int i = 0; i < count_; ++i) meters_[i].on_theme();
    spark_.on_theme();
  }

  void on_tap() override { tessera::open_card(plugin_->plugin_id(), "usage", entity_, tile_); }

 private:
  const tessera::Plugin *plugin_;
  int count_ = 1, tile_ = -1;
  std::string entity_;
  lv_obj_t *message_{}, *head_{};
  Meter meters_[2];
  Spark spark_;
  Usage usage_;
};

// The card: every limit the plan has, one above the other, each with the day and time it resets and a countdown, and
// at the bottom when the computer last sent the numbers.
class UsageCard : public tessera::Card {
 public:
  explicit UsageCard(const tessera::Plugin *plugin) : plugin_(plugin) {}

  void open(const tessera::CardContext &c) override {
    width_ = c.width;
    height_ = c.height;
    bar_ = ui::px(ui::large() ? 8 : 6);
    for (auto &m : meters_) m.make(c.parent);
    note_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_label_set_long_mode(note_, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(note_, width_, ui::line_height(Font::BODY));
    lv_obj_set_pos(note_, 0, height_ - ui::line_height(Font::BODY));
    spark_.make(c.parent);
  }

  void on_state(JsonObjectConst data) override {
    usage_.read(data);
    render();
  }

  void on_tick(uint32_t now) override {
    now_ = now;
    render();
  }

  void on_theme() override {
    for (auto &m : meters_) m.on_theme();
    spark_.on_theme();
    ui::set_color(note_, theme::MUTED);
    render();
  }

 private:
  // Lay the limits out again only when how many there are changed.
  void layout(int count) {
    if (count == shown_) return;
    shown_ = count;
    const int gap = ui::px(ui::large() ? 14 : 8);
    const int room = height_ - ui::line_height(Font::BODY) - gap;
    // Each limit as large as its share of the room, but no larger than its largest figures need: on a large screen
    // they stay together at the top instead of spreading over the glass.
    const int h = count ? std::min((room - (count - 1) * gap) / count, Meter::height(Font::DIGITS, bar_) + gap) : 0;
    for (int i = 0; i < count; ++i) meters_[i].place(0, i * (h + gap), width_, h, bar_);
    int x, y, size;
    meters_[0].spark_box(x, y, size);
    spark_.place(x, y, size);
  }

  void render() {
    const char *why = usage_.message(now_);
    const int count = why ? 0 : usage_.pct[MODEL] >= 0 ? 3 : 2;
    const bool old = usage_.old(now_);
    layout(count);
    // A limit the sensor lacks has the percentage -1, which paint() hides.
    for (int i = 0; i < LIMITS; ++i) {
      if (i < count)
        meters_[i].paint(name_of(plugin_, usage_, i), usage_.percent(i, now_),
                         resets_text(plugin_, usage_.resets_at[i], now_), old);
      else
        meters_[i].paint("", -1, "", false);
    }
    // "Not updated · Updated Thu 15 Oct 09:00", or why there is nothing to show.
    std::string note;
    if (why)
      note = plugin_->text(why);
    else {
      if (usage_.updated_at) note = tessera::fill(plugin_->text("updated"), "time", moment_text(usage_.updated_at));
      if (old) note = note.empty() ? std::string(plugin_->text("old")) : plugin_->text("old") + std::string(" · ") + note;
    }
    ui::set_text(note_, note);
    spark_.paint(usage_.working && !old, old, !why);
  }

  const tessera::Plugin *plugin_;
  int width_ = 0, height_ = 0, bar_ = 1, shown_ = -1;
  uint32_t now_ = 0;
  lv_obj_t *note_{};
  Meter meters_[LIMITS];
  Spark spark_;
  Usage usage_;
};

void ClaudeUsage::setup() {
  // The ids are the tile's and the card's in the manifest.
  add_tile("usage", [this]() { return new UsageTile(this); });
  add_card("usage", [this]() { return new UsageCard(this); });
}

}  // namespace esphome::claude_usage
