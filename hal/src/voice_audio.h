// SPDX-License-Identifier: Apache-2.0
// In-HAL voice-call audio path (MSM8916 q6voice): opens the CS-Voice PCMs while a call is up, routes the codec and
// follows Android's mic mute and call volume. Replaces the module's j5x-voice.sh daemon.
#pragma once
namespace j5ril {
void voiceAudioStart();             // spawn the worker thread (idempotent)
void voiceAudioCall(bool active);   // from the call state: dialing/alerting/active
void voiceAudioMute(bool muted);    // IRadioVoice.setMute
}
