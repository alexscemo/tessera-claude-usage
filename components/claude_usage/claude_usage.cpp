#include "claude_usage.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
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

// A number as Home Assistant's attribute has it: a JSON number, or a text of one. -1 when there is none.
static float number(JsonVariantConst v) {
  if (v.isNull()) return -1;
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    char *end = nullptr;
    const float f = strtof(s, &end);
    return end == s ? -1 : f;
  }
  return v.as<float>();
}

// What sensor.claude_code_usage says (README.md): one percentage and one moment of reset per limit.
struct Usage {
  float pct[LIMITS] = {-1, -1, -1};
  uint32_t resets_at[LIMITS] = {0, 0, 0};
  uint32_t updated_at = 0;
  std::string model_name, wait;
  bool has_entity = false, no_value = false, stale = false;

  void read(JsonObjectConst data) {
    JsonObjectConst a = data["attributes"];
    static const char *const PCT[LIMITS] = {"session_pct", "week_pct", "model_pct"};
    static const char *const AT[LIMITS] = {"session_resets_at", "week_resets_at", "model_resets_at"};
    for (int i = 0; i < LIMITS; ++i) {
      pct[i] = number(a[PCT[i]]);
      resets_at[i] = a[AT[i]] | 0u;
    }
    updated_at = a["updated_at"] | 0u;
    model_name = std::string(a["model_name"] | "");
    stale = a["stale"] | false;
    // Before a person has chosen the sensor the tile gets {}; a sensor without a value says unavailable or unknown.
    has_entity = !data["state"].isNull();
    const std::string state = std::string(data["state"] | "");
    no_value = state == "unavailable" || state == "unknown";
    wait = std::string(data["wait"] | "");
  }

  // Why there is nothing to show (a key of translations, part screen), or nullptr.
  const char *message(uint32_t now) const {
    if (!has_entity || wait == "wrong_entity") return "choose_sensor";
    if (!wait.empty() || no_value || !now || pct[SESSION] < 0) return "waiting";
    return nullptr;
  }

  bool has(int limit) const { return pct[limit] >= 0; }

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

// "Resets 14:30 · in 2 h 10 min", or with the day when it is not today: "Resets Thu 15 Oct 09:00 · in 2 d 1 h".
static std::string resets_text(const tessera::Plugin *plugin, uint32_t at, uint32_t now) {
  if (!at || !now || at <= now) return "";
  std::string when = tessera::clock_text(at);
  if (tessera::days_from_today(at) != 0) when = tessera::date_text(at) + " " + when;
  return tessera::fill(plugin->text("resets"), "time", when) + " · " + countdown(plugin, at, now);
}

static std::string name_of(const tessera::Plugin *plugin, const Usage &u, int limit) {
  if (limit == SESSION) return plugin->text("session");
  std::string week = plugin->text("week");
  if (limit == MODEL && !u.model_name.empty()) week += " · " + u.model_name;
  return week;
}

// One limit, in three lines: its percentage large with its name beside it, a bar of how much is used, and a line about
// when it resets. As large as its room lets it be. The tile and the card both draw with it.
class Meter {
 public:
  void make(lv_obj_t *parent, Font name_font) {
    name_large_ = name_font_ = name_font;
    pct_ = ui::label(parent, Font::VALUE, theme::INK);
    name_ = ui::label(parent, name_font, theme::MUTED);
    track_ = ui::block(parent, theme::TRACK);
    fill_ = ui::block(parent, theme::ACCENT);
    when_ = ui::label(parent, Font::BODY, theme::MUTED);
    for (lv_obj_t *l : {name_, pct_, when_}) lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(name_, LV_TEXT_ALIGN_RIGHT, 0);
  }

  // The height this needs with `font` for the percentage.
  static int height(Font font, int bar) { return ui::line_height(font) + bar + 2 * gap() + ui::line_height(Font::BODY); }

  // Lay it out in x, y, w, h: the percentage takes the largest font that fits the room and leaves some of the width
  // to the name; the rest sits under it, the whole in the middle of the room.
  void place(int x, int y, int w, int h, int bar) {
    x_ = x;
    w_ = std::max(1, w);
    bar_ = bar;
    const int fixed = bar + 2 * gap() + ui::line_height(Font::BODY);
    pct_font_ = Font::TITLE;
    for (Font f : {Font::VALUE, Font::HEADLINE, Font::TITLE}) {
      pct_font_ = f;
      if (fixed + ui::line_height(f) <= h && ui::text_width("100%", f) <= w_ * 6 / 10) break;
    }
    line_ = ui::line_height(pct_font_);
    y_ = y + std::max(0, (h - fixed - line_) / 2);
    ui::set_font(pct_, pct_font_);
    // The name never larger than the percentage beside it.
    name_font_ = pct_font_ == Font::TITLE ? Font::BODY : name_large_;
    ui::set_font(name_, name_font_);
    bar_y_ = y_ + line_ + gap();
    lv_obj_set_size(track_, w_, bar_);
    lv_obj_set_pos(track_, x_, bar_y_);
    lv_obj_set_size(when_, w_, ui::line_height(Font::BODY));
    lv_obj_set_pos(when_, x_, bar_y_ + bar_ + gap());
  }

  // The texts and the bar; pct < 0 hides it all. `old`: the numbers are not fresh, so they are drawn quieter.
  void paint(const std::string &name, int pct, const std::string &when, bool old) {
    const bool shown = pct >= 0;
    show(shown);
    if (!shown) return;
    // The percentage as wide as it is, the name in what is left of the line, on the percentage's foot.
    const std::string value = std::to_string(pct) + "%";
    const int pw = std::min(w_, ui::text_width(value, pct_font_) + ui::px(2));
    ui::set_text(pct_, value);
    lv_obj_set_size(pct_, pw, line_);
    lv_obj_set_pos(pct_, x_, y_);
    ui::set_tone(pct_, !old && pct >= ALERT_PCT ? Tone::ALERT : Tone::NORMAL, old ? theme::MUTED : theme::INK);
    const int space = ui::px(6), nh = ui::line_height(name_font_);
    const int nw = w_ - pw - space;
    if (nw < ui::text_width(name, name_font_) / 2) {
      lv_obj_add_flag(name_, LV_OBJ_FLAG_HIDDEN);  // no room for the name: the percentage alone says it
    } else {
      ui::set_text(name_, name);
      lv_obj_set_size(name_, nw, nh);
      lv_obj_set_pos(name_, x_ + pw + space, y_ + std::max(0, line_ - nh - ui::px(2)));
    }
    ui::set_text(when_, when);
    const int fw = std::min(w_, w_ * std::clamp(pct, 0, 100) / 100);
    if (fw < bar_) {
      lv_obj_add_flag(fill_, LV_OBJ_FLAG_HIDDEN);  // too short to draw as a rounded bar
    } else {
      lv_obj_clear_flag(fill_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(fill_, fw, bar_);
      lv_obj_set_pos(fill_, x_, bar_y_);
    }
    // The bar in the accent, amber when the limit is nearly used up, grey when the numbers are old.
    const theme::Role role = old ? theme::MUTED : pct >= BUSY_PCT ? theme::MARK_AMBER : theme::ACCENT;
    if (role != fill_role_) {
      fill_role_ = role;
      paint_fill();
    }
  }

  void on_theme() {
    ui::set_color(name_, theme::MUTED);
    ui::set_color(when_, theme::MUTED);
    lv_obj_set_style_bg_color(track_, ui::color(theme::TRACK), 0);
    paint_fill();
  }

  static int gap() { return ui::px(ui::large() ? 5 : 3); }

 private:
  void show(bool on) {
    for (lv_obj_t *o : {name_, pct_, track_, fill_, when_}) {
      if (on)
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
      else
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
  }

  void paint_fill() { lv_obj_set_style_bg_color(fill_, ui::color(fill_role_), 0); }

  Font name_font_ = Font::BODY, name_large_ = Font::BODY, pct_font_ = Font::VALUE;
  theme::Role fill_role_ = theme::ACCENT;
  int x_ = 0, y_ = 0, w_ = 1, bar_ = 1, bar_y_ = 0, line_ = 1;
  lv_obj_t *name_{}, *pct_{}, *track_{}, *fill_{}, *when_{};
};

// The tile. 1x1: the session alone. Wider: the session and the week side by side. Taller: one above the other, under a
// header with the tile's name. The numbers change every few minutes, the countdowns every second.
class UsageTile : public tessera::Tile {
 public:
  explicit UsageTile(const tessera::Plugin *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    name_ = c.name;
    entity_ = c.entity;
    tile_ = c.tile;
    bar_ = ui::px(ui::large() ? 8 : 6);
    count_ = c.columns >= 2 || c.rows >= 2 ? 2 : 1;
    stacked_ = c.rows >= 2;
    const int head = ui::line_height(Font::TITLE) + Meter::gap();
    // The header only where the two limits under it still get large figures.
    has_head_ = stacked_ && height_ - head >= 2 * Meter::height(Font::HEADLINE, bar_) + Meter::gap();

    message_ = ui::label(c.parent, Font::BODY_LARGE, theme::MUTED);
    lv_obj_set_size(message_, width_, ui::line_height(Font::BODY_LARGE));
    lv_obj_set_pos(message_, 0, (height_ - ui::line_height(Font::BODY_LARGE)) / 2);
    lv_obj_set_style_text_align(message_, LV_TEXT_ALIGN_CENTER, 0);
    if (has_head_) {
      head_ = ui::label(c.parent, Font::TITLE, theme::INK);
      lv_obj_set_size(head_, width_, ui::line_height(Font::TITLE));
      lv_obj_set_pos(head_, 0, 0);
      lv_label_set_long_mode(head_, LV_LABEL_LONG_CLIP);
      ui::set_text(head_, name_.empty() ? plugin_->text("header") : name_);
    }

    const int top = has_head_ ? head : 0;
    const int col_gap = ui::px(ui::large() ? 16 : 10);
    for (int i = 0; i < count_; ++i) {
      meters_[i].make(c.parent, Font::BODY);
      if (count_ == 1)
        meters_[i].place(0, 0, width_, height_, bar_);
      else if (stacked_) {
        const int h = (height_ - top - Meter::gap()) / 2;
        meters_[i].place(0, top + i * (h + Meter::gap()), width_, h, bar_);
      } else {
        const int w = (width_ - col_gap) / 2;
        meters_[i].place(i * (w + col_gap), 0, w, height_, bar_);
      }
    }
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
      const std::string when =
          usage_.stale ? plugin_->text("old") : countdown(plugin_, usage_.resets_at[i], now);
      meters_[i].paint(name_of(plugin_, usage_, i), usage_.percent(i, now), when, usage_.stale);
    }
    if (head_) {
      if (why)
        lv_obj_add_flag(head_, LV_OBJ_FLAG_HIDDEN);
      else
        lv_obj_clear_flag(head_, LV_OBJ_FLAG_HIDDEN);
    }
  }

  void on_theme() override {
    ui::set_color(message_, theme::MUTED);
    if (head_) ui::set_color(head_, theme::INK);
    for (int i = 0; i < count_; ++i) meters_[i].on_theme();
  }

  void on_tap() override { tessera::open_card(plugin_->plugin_id(), "usage", entity_, tile_); }

 private:
  const tessera::Plugin *plugin_;
  int width_ = 0, height_ = 0, bar_ = 1, count_ = 1, tile_ = -1;
  bool stacked_ = false, has_head_ = false;
  std::string name_, entity_;
  lv_obj_t *message_{}, *head_{};
  Meter meters_[2];
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
    bar_ = ui::px(ui::large() ? 12 : 8);
    for (auto &m : meters_) m.make(c.parent, Font::BODY_LARGE);
    note_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_label_set_long_mode(note_, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(note_, width_, ui::line_height(Font::BODY));
    lv_obj_set_pos(note_, 0, height_ - ui::line_height(Font::BODY));
    shown_ = -1;
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
    const int h = count ? std::min((room - (count - 1) * gap) / count, Meter::height(Font::VALUE, bar_) + gap) : 0;
    for (int i = 0; i < count; ++i) meters_[i].place(0, i * (h + gap), width_, h, bar_);
  }

  void render() {
    const char *why = usage_.message(now_);
    const int count = why ? 0 : usage_.has(MODEL) ? 3 : 2;
    layout(count);
    for (int i = 0; i < LIMITS; ++i) {
      if (i >= count || !usage_.has(i)) {
        meters_[i].paint("", -1, "", false);
        continue;
      }
      meters_[i].paint(name_of(plugin_, usage_, i), usage_.percent(i, now_),
                       resets_text(plugin_, usage_.resets_at[i], now_), usage_.stale);
    }
    std::string note;
    if (why)
      note = plugin_->text(why);
    else if (usage_.updated_at) {
      std::string when = tessera::clock_text(usage_.updated_at);
      if (tessera::days_from_today(usage_.updated_at) != 0) when = tessera::date_text(usage_.updated_at) + " " + when;
      note = tessera::fill(plugin_->text("updated"), "time", when);
      if (usage_.stale) note = std::string(plugin_->text("old")) + " · " + note;
    } else if (usage_.stale)
      note = plugin_->text("old");
    ui::set_text(note_, note);
    ui::set_color(note_, theme::MUTED);
  }

  const tessera::Plugin *plugin_;
  int width_ = 0, height_ = 0, bar_ = 1, shown_ = -1;
  uint32_t now_ = 0;
  lv_obj_t *note_{};
  Meter meters_[LIMITS];
  Usage usage_;
};

void ClaudeUsage::setup() {
  // The ids are the tile's and the card's in the manifest.
  add_tile("usage", [this]() { return new UsageTile(this); });
  add_card("usage", [this]() { return new UsageCard(this); });
}

}  // namespace esphome::claude_usage
