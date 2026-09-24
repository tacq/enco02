#pragma once

#ifndef _TIMER_COMMAND_H_
#define _TIMER_COMMAND_H_

#include <cstdint>
#include <string>

// Maps a spoken utterance to a countdown-timer command.
//
// Same reasoning as volume_command.h: the cloud model decides for itself whether to emit an MCP
// tool call, and for a bare "五分钟后叫我" it often just answers conversationally. Matching the
// user's own transcript is what makes the feature reliable. The MCP tools remain the primary path;
// this is the safety net.
//
// Kept free of any Arduino or ESP-IDF dependency so the grammar can be exercised on a host.

enum class TimerCommandKind : uint8_t {
  kNone,
  kStart,   // seconds is set
  kCancel,
  kQuery,
};

struct TimerCommand {
  TimerCommandKind kind = TimerCommandKind::kNone;
  uint32_t seconds = 0;
};

// Longest timer we will accept. Anything beyond this is far more likely to be a misparse ("two
// thousand" out of a stray digit run) than a real request.
inline constexpr uint32_t kTimerMaxSeconds = 24u * 60u * 60u;

namespace timer_command_detail {

// Advances `i` past one UTF-8 sequence and returns it. Chinese characters are three bytes, so the
// grammar below cannot work a byte at a time.
inline bool NextChar(const std::string& s, size_t& i, std::string& out) {
  if (i >= s.size()) {
    return false;
  }
  const auto lead = static_cast<unsigned char>(s[i]);
  size_t len = 1;
  if ((lead & 0xE0) == 0xC0) {
    len = 2;
  } else if ((lead & 0xF0) == 0xE0) {
    len = 3;
  } else if ((lead & 0xF8) == 0xF0) {
    len = 4;
  }
  if (i + len > s.size()) {
    len = 1;  // Truncated sequence: consume one byte so we always make progress.
  }
  out.assign(s, i, len);
  i += len;
  return true;
}

// -1 for "not a digit". 两 and 俩 are included because spoken Chinese says 两分钟, never 二分钟.
inline int CnDigit(const std::string& c) {
  if (c == "零" || c == "〇") return 0;
  if (c == "一" || c == "壹" || c == "幺") return 1;
  if (c == "二" || c == "两" || c == "俩" || c == "贰") return 2;
  if (c == "三" || c == "叁") return 3;
  if (c == "四" || c == "肆") return 4;
  if (c == "五" || c == "伍") return 5;
  if (c == "六" || c == "陆") return 6;
  if (c == "七" || c == "柒") return 7;
  if (c == "八" || c == "捌") return 8;
  if (c == "九" || c == "玖") return 9;
  return -1;
}

// Total number of seconds named in `query`, or 0 if it names no duration.
//
// Handles the orderings that actually turn up in speech: "5分钟", "五分钟", "三十分钟", "半小时",
// "一个半小时", "一分半", "1分30秒", "90 seconds". Written as a small accumulator rather than a
// table of phrases because Chinese composes numbers ("二十五") instead of listing them.
inline uint32_t ParseDurationSeconds(const std::string& query) {
  // Case-folded copy so the English units below can be matched without duplicating every spelling.
  std::string lower;
  lower.reserve(query.size());
  for (const char ch : query) {
    lower.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
  }

  uint64_t total = 0;
  uint32_t section = 0;     // completed tens/hundreds part of the number being read
  uint32_t digits = 0;      // the ones place, or a run of Arabic digits
  bool saw_number = false;
  bool pending_half = false;
  // Whether that 半 is still sitting directly in front of a unit, as in 半小时 / 一个半小时. It stops
  // being adjacent as soon as any unrelated character goes by, which is what keeps the 时 at the end
  // of 倒计时 from claiming the half in "一分半的倒计时" and charging half an hour for it.
  bool half_adjacent = false;
  uint32_t last_unit_secs = 0;  // so a trailing 半 in "一分半" knows what it is half *of*
  // A number that was closed by a measure word. "一个半小时" needs the 一 to survive the 个, but
  // "一个5分钟的定时器" must not let it merge into the 5 and become fifteen minutes - so it is held
  // aside and only used if no fresh number turns up first.
  uint32_t carry = 0;
  bool carry_valid = false;

  const auto take_unit = [&](const uint32_t unit_secs) {
    uint32_t value = section + digits;
    bool have_value = saw_number;
    if (!have_value && carry_valid) {
      value = carry;  // "一个小时"
      have_value = true;
    }
    if (!have_value && !(pending_half && half_adjacent)) {
      // A unit character with no number in front of it. This is what keeps the 时 in 定时器 from
      // being read as an hour.
      return;
    }
    total += static_cast<uint64_t>(value) * unit_secs;
    if (pending_half) {
      total += unit_secs / 2;  // 半小时 -> 1800, 一个半小时 -> 3600 + 1800
    }
    last_unit_secs = unit_secs;
    section = 0;
    digits = 0;
    saw_number = false;
    pending_half = false;
    half_adjacent = false;
    carry_valid = false;
  };

  const auto note_number = [&]() {
    saw_number = true;
    carry_valid = false;  // A fresh number supersedes anything held across a measure word.
  };

  std::string c;
  size_t i = 0;
  while (NextChar(lower, i, c)) {
    if (c.size() == 1 && c[0] >= '0' && c[0] <= '9') {
      digits = digits * 10 + static_cast<uint32_t>(c[0] - '0');
      if (digits > kTimerMaxSeconds) {
        digits = kTimerMaxSeconds;  // Saturate rather than wrap on a pathological digit run.
      }
      note_number();
      continue;
    }

    const int d = CnDigit(c);
    if (d >= 0) {
      digits = digits * 10 + static_cast<uint32_t>(d);
      note_number();
      continue;
    }

    if (c == "十" || c == "拾") {
      // 十分钟 is ten minutes, 三十分钟 is thirty: a bare 十 carries an implicit leading one.
      section += (digits == 0 ? 1 : digits) * 10;
      digits = 0;
      note_number();
      continue;
    }
    if (c == "百") {
      section += (digits == 0 ? 1 : digits) * 100;
      digits = 0;
      note_number();
      continue;
    }
    if (c == "个") {
      // Measure word: closes the current number without discarding it. See `carry` above.
      carry = section + digits;
      carry_valid = saw_number;
      section = 0;
      digits = 0;
      saw_number = false;
      continue;
    }
    if (c == "半") {
      if (!saw_number && carry_valid) {
        digits = carry;  // "一个半小时"
        saw_number = true;
        carry_valid = false;
      }
      pending_half = true;
      half_adjacent = true;
      continue;
    }

    if (c == "时") {
      take_unit(3600);
      continue;
    }
    if (c == "秒") {
      take_unit(1);
      continue;
    }
    if (c == "分") {
      // 分 is the one unit with a common false friend: 十分 is also the adverb "very", as in
      // 十分感谢. Require corroboration - either the 钟 that makes it a clock unit, or another
      // number / end of sentence, as in "1分30秒" and "计时一分半".
      const size_t save = i;
      std::string next;
      const bool have_next = NextChar(lower, i, next);
      i = save;
      const bool is_minute =
          !have_next || next == "钟" || next == "半" || next == "秒" || next == "后" || next == "的" ||
          next == "，" || next == "。" || next == "," || next == "." || next == "!" || next == "！" ||
          CnDigit(next) >= 0 || (next.size() == 1 && next[0] >= '0' && next[0] <= '9');
      if (is_minute) {
        take_unit(60);
      } else {
        section = 0;
        digits = 0;
        saw_number = false;
        carry_valid = false;
      }
      continue;
    }

    // English units. Matched on the leading letter plus a lookahead so "m"/"s" on their own ("5m",
    // "30s") work without "minutes" being read as four separate units.
    if (c.size() == 1 && (c[0] == 'h' || c[0] == 'm' || c[0] == 's') && (saw_number || pending_half)) {
      const char unit = c[0];
      // Skip the rest of the word so its remaining letters are not re-examined.
      while (i < lower.size() && lower[i] >= 'a' && lower[i] <= 'z') {
        ++i;
      }
      take_unit(unit == 'h' ? 3600 : (unit == 'm' ? 60 : 1));
      continue;
    }

    // Any other character ends the current number without consuming it, so a stray figure cannot
    // leak across into a later unit. 小 and 钟 are excepted because they are *inside* the unit
    // words 小时 and 分钟 - letting 小 clear the accumulator would break "一个半小时".
    if (c != " " && c != "又" && c != "小" && c != "钟") {
      section = 0;
      digits = 0;
      saw_number = false;
      // pending_half survives - a trailing 半 is resolved against last_unit_secs below - but it is
      // no longer next to a unit, so it may not conjure one of its own out of a later 时 or 分.
      half_adjacent = false;
    }
  }

  // "一分半" / "两小时半": the 半 arrived after its unit, so apply it to that unit now.
  if (pending_half && last_unit_secs > 0) {
    total += last_unit_secs / 2;
  }

  if (total == 0) {
    return 0;  // No unit anywhere: "定时一下" names no duration.
  }
  return total > kTimerMaxSeconds ? kTimerMaxSeconds : static_cast<uint32_t>(total);
}

}  // namespace timer_command_detail

inline TimerCommand ClassifyTimerCommand(const std::string& query) {
  using namespace timer_command_detail;

  TimerCommand result;
  if (query.empty()) {
    return result;
  }

  std::string lower;
  lower.reserve(query.size());
  for (const char ch : query) {
    lower.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
  }
  const auto has = [&](const char* kw) {
    return lower.find(kw) != std::string::npos;
  };

  // Every branch below requires the utterance to be about timing at all. Without this, "等五分钟
  //我就回来" would start a timer the user never asked for - the same false-positive trap the
  // volume fallback has to avoid.
  const bool about_timer = has("定时") || has("倒计时") || has("计时") || has("闹钟") || has("闹铃") ||
                           has("提醒") || has("叫我") || has("喊我") || has("秒后") || has("分钟后") ||
                           has("分后") || has("小时后") || has("钟头后") || has("timer") || has("alarm") ||
                           has("countdown") || has("remind");
  if (!about_timer) {
    return result;
  }

  // Cancel first: "取消五分钟的定时器" names a duration too, and the cancellation is what is meant.
  if (has("取消") || has("关掉") || has("关闭") || has("停止") || has("停掉") || has("撤销") ||
      has("删除") || has("清除") || has("别计时") || has("不计时") || has("不用计时") ||
      has("算了") || has("cancel") || has("stop")) {
    result.kind = TimerCommandKind::kCancel;
    return result;
  }

  // Then questions, before the duration parse: "五分钟的定时器还有多久" contains a duration but is
  // asking, not setting.
  if (has("还有多久") || has("还剩") || has("剩多少") || has("剩下多少") || has("剩余") ||
      has("多长时间") || has("还有多长") || has("到了吗") || has("好了吗") || has("how long") ||
      has("how much time") || has("remaining")) {
    result.kind = TimerCommandKind::kQuery;
    return result;
  }

  const uint32_t seconds = ParseDurationSeconds(query);
  if (seconds > 0) {
    result.kind = TimerCommandKind::kStart;
    result.seconds = seconds;
  }
  return result;
}

#endif
