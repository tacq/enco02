#pragma once

#ifndef _VOLUME_COMMAND_H_
#define _VOLUME_COMMAND_H_

#include <cstdint>
#include <string>

// Maps a spoken utterance to a relative volume command.
//
// This exists because the cloud model decides for itself whether to emit an MCP tool call, and for
// a bare "大声点" it frequently just answers conversationally instead. Matching the user's own
// transcript is what makes these commands reliable.
//
// Kept in its own header, free of any Arduino or ESP-IDF dependency, so the word lists can be
// exercised by a host-side test instead of only on the device.
enum class VolumeCommand : uint8_t {
  kNone,
  kUp,
  kDown,
};

inline VolumeCommand ClassifyVolumeCommand(const std::string& query) {
  if (query.empty()) {
    return VolumeCommand::kNone;
  }

  const auto has = [&](const char* kw) {
    return query.find(kw) != std::string::npos;
  };

  // A flat list of whole phrases was the first attempt and it does not survive contact with real
  // speech: Chinese puts the verb and the noun in either order, so "调小声音", "音量调小" and
  // "降低音量" are the same request and none of them contain each other. This instead looks for a
  // direction word *near* an audio noun, which covers the orderings without enumerating them.
  //
  // Order matters a great deal here, and the rule is: the more specific a phrase is about what the
  // user actually wants, the earlier it has to be tested. "太小声" contains "小声", but the two
  // mean opposite things.

  // Questions first. Substring matching cannot tell "声音大一点" from "你的声音大吗", and acting on
  // the question would be both wrong and confusing, so hand those back to the model to answer.
  if (has("吗") || has("呢") || has("什么") || has("多少") || has("?") || has("？")) {
    return VolumeCommand::kNone;
  }

  // Complaints that need no audio noun to be unambiguous. They have to precede the plain idioms
  // below because they read as their own opposite: "太小声" is a request for *more* volume.
  if (has("太小声") || has("听不见") || has("听不清") || has("听不到")) {
    return VolumeCommand::kUp;
  }
  if (has("太吵") || has("吵死") || has("太响") || has("太大声")) {
    return VolumeCommand::kDown;
  }

  // Plain idioms, still with no audio noun needed. Quieter is tested first: the user is often
  // correcting ("别大声，小声一点") and the correction is the part they mean.
  if (has("小声") || has("轻一点") || has("轻点") || has("安静点") || has("静一点")) {
    return VolumeCommand::kDown;
  }
  if (has("大声")) {
    return VolumeCommand::kUp;
  }

  // Everything below needs the utterance to be about the audio at all.
  if (!has("音量") && !has("声音") && !has("嗓门")) {
    return VolumeCommand::kNone;
  }

  // Degree-of-complaint beats plain direction, because it inverts it: "声音太大了" names 大 but is
  // asking for less, and "声音有点小" names 小 but is asking for more.
  if (has("太大") || has("很大") || has("好大") || has("有点大") || has("有些大")) {
    return VolumeCommand::kDown;
  }
  if (has("太小") || has("很小") || has("好小") || has("有点小") || has("有些小") || has("太轻")) {
    return VolumeCommand::kUp;
  }

  // Plain direction words, in any position relative to the noun.
  if (has("小") || has("低") || has("减") || has("降")) {
    return VolumeCommand::kDown;
  }
  // 大 is safe to match bare - next to 音量/声音 it essentially always means loudness. 高 is not,
  // because "声音很高" is about pitch, so only the explicitly-adjusting compounds are accepted.
  if (has("大") || has("调高") || has("提高") || has("升高") || has("增") || has("响")) {
    return VolumeCommand::kUp;
  }

  return VolumeCommand::kNone;
}

#endif
