//////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2007-2016 musikcube team
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    * Redistributions of source code must retain the above copyright notice,
//      this list of conditions and the following disclaimer.
//
//    * Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//    * Neither the name of the author nor the names of other contributors may
//      be used to endorse or promote products derived from this software
//      without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
//////////////////////////////////////////////////////////////////////////////

#include "pch.hpp"

#include <core/debug.h>
#include <core/audio/GaplessTransport.h>
#include <core/plugin/PluginFactory.h>
#include <algorithm>
#include <boost/thread.hpp>

using namespace musik::core::audio;
using namespace musik::core::sdk;

static std::string TAG = "Transport";

#define RESET_NEXT_PLAYER(instance) \
    if (instance->nextPlayer) { \
        instance->nextPlayer->Destroy(); \
        instance->nextPlayer = nullptr; \
    }

GaplessTransport::GaplessTransport()
: volume(1.0)
, state(PlaybackStopped)
, nextPlayer(nullptr)
, nextCanStart(false)
, muted(false) {
    this->output = Player::CreateDefaultOutput();
}

GaplessTransport::~GaplessTransport() {
    this->disconnect_all();

    PlayerList players;

    {
        LockT lock(this->stateMutex);
        std::swap(this->active, players);
    }

    for (auto it = players.begin(); it != players.end(); it++) {
        (*it)->Destroy();
    }
}

PlaybackState GaplessTransport::GetPlaybackState() {
    LockT lock(this->stateMutex);
    return this->state;
}

void GaplessTransport::PrepareNextTrack(const std::string& trackUrl) {
    bool startNext = false;
    {
        LockT lock(this->stateMutex);
        RESET_NEXT_PLAYER(this);
        this->nextPlayer = Player::Create(trackUrl, this->output, this);
        startNext = this->nextCanStart;
    }

    if (startNext) {
        this->StartWithPlayer(this->nextPlayer);
    }
}

void GaplessTransport::Start(const std::string& url) {
    musik::debug::info(TAG, "we were asked to start the track at " + url);

    Player* newPlayer = Player::Create(url, this->output, this);
    musik::debug::info(TAG, "Player created successfully");

    this->StartWithPlayer(newPlayer);
}

void GaplessTransport::StartWithPlayer(Player* newPlayer) {
    if (newPlayer) {
        bool playingNext = false;

        {
            LockT lock(this->stateMutex);

            playingNext = (newPlayer == nextPlayer);
            if (nextPlayer != nullptr && newPlayer != nextPlayer) {
                this->nextPlayer->Destroy();
            }

            this->nextPlayer = nullptr;
            this->active.push_back(newPlayer);
        }

        /* first argument suppresses the "Stop" event from getting triggered,
        the second param is used for gapless playback -- we won't stop the output
        and will allow pending buffers to finish if we're not automatically
        playing the next track. note we do this outside of critical section so
        outputs *can* stop buffers immediately, and not to worry about causing a
        deadlock. */
        this->StopInternal(true, !playingNext, newPlayer);
        this->SetNextCanStart(false);
        this->output->Resume();
        newPlayer->Play();
        musik::debug::info(TAG, "play()");

        this->RaiseStreamEvent(StreamScheduled, newPlayer);
    }
}

void GaplessTransport::Stop() {
    this->StopInternal(false, true);
}

void GaplessTransport::StopInternal(
    bool suppressStopEvent,
    bool stopOutput,
    Player* exclude)
{
    musik::debug::info(TAG, "stop");

    /* if we stop the output, we kill all of the Players immediately.
    otherwise, we let them finish naturally; RemoveActive() will take
    care of disposing of them */
    if (stopOutput) {
        std::list<Player*> toDelete;

        {
            LockT lock(this->stateMutex);
            RESET_NEXT_PLAYER(this);

            /* destroy all but specified player */
            auto it = this->active.begin();
            while (it != this->active.end()) {
                if (*it != exclude) {
                    (*it)->Destroy();
                    it = this->active.erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        /* stopping the transport will stop any buffers that are currently in
        flight. this makes the sound end immediately. */
        this->output->Stop();
    }

    if (!suppressStopEvent) {
        /* if we know we're starting another track immediately, suppress
        the stop event. this functionality is not available to the public
        interface, it's an internal optimization */
        this->SetPlaybackState(PlaybackStopped);
    }
}

bool GaplessTransport::Pause() {
    musik::debug::info(TAG, "pause");

    size_t count = 0;

    this->output->Pause();

    {
        LockT lock(this->stateMutex);
        count = this->active.size();
    }

    if (count) {
        this->SetPlaybackState(PlaybackPaused);
        return true;
    }

    return false;
}

bool GaplessTransport::Resume() {
    musik::debug::info(TAG, "resume");

    this->output->Resume();

    size_t count = 0;

    {
        LockT lock(this->stateMutex);
        count = this->active.size();

        auto it = this->active.begin();
        while (it != this->active.end()) {
            (*it)->Play();
            ++it;
        }
    }

    if (count) {
        this->SetPlaybackState(PlaybackPlaying);
        return true;
    }

    return false;
}

double GaplessTransport::Position() {
    LockT lock(this->stateMutex);

    if (!this->active.empty()) {
        return this->active.front()->Position();
    }

    return 0;
}

void GaplessTransport::SetPosition(double seconds) {
    LockT lock(this->stateMutex);

    if (!this->active.empty()) {
        this->active.front()->SetPosition(seconds);
        this->TimeChanged(seconds);
    }
}

bool GaplessTransport::IsMuted() {
    return this->muted;
}

void GaplessTransport::SetMuted(bool muted) {
    if (this->muted != muted) {
        this->muted = muted;
        this->output->SetVolume(muted ? 0.0f : this->volume);
        this->VolumeChanged();
    }
}

double GaplessTransport::Volume() {
    return this->volume;
}

void GaplessTransport::SetVolume(double volume) {
    double oldVolume = this->volume;

    volume = std::max(0.0, std::min(1.0, volume));

    this->volume = volume;

    if (oldVolume != this->volume) {
        this->VolumeChanged();
    }

    std::string output = boost::str(
        boost::format("set volume %d%%") % round(volume * 100));

    musik::debug::info(TAG, output);

    this->output->SetVolume(this->volume);
}

void GaplessTransport::RemoveFromActive(Player* player) {
    bool found = false;

    {
        LockT lock(this->stateMutex);

        std::list<Player*>::iterator it =
            std::find(this->active.begin(), this->active.end(), player);

        if (it != this->active.end()) {
            this->active.erase(it);
            found = true;
        }
    }

    /* outside of the critical section, otherwise potential deadlock */
    if (found) {
        player->Destroy();
    }
}

void GaplessTransport::SetNextCanStart(bool nextCanStart) {
    LockT lock(this->stateMutex);
    this->nextCanStart = nextCanStart;
}

void GaplessTransport::OnPlaybackStarted(Player* player) {
    this->RaiseStreamEvent(StreamPlaying, player);
    this->SetPlaybackState(PlaybackPlaying);
}

void GaplessTransport::OnPlaybackAlmostEnded(Player* player) {
    this->SetNextCanStart(true);

    {
        LockT lock(this->stateMutex);

        /* if another component configured a next player while we were playing,
        go ahead and get it started now. */
        if (this->nextPlayer) {
            this->StartWithPlayer(this->nextPlayer);
        }
    }

    this->RaiseStreamEvent(StreamAlmostDone, player);
}

void GaplessTransport::OnPlaybackFinished(Player* player) {
    this->RaiseStreamEvent(StreamFinished, player);

    bool stopped = false;

    {
        LockT lock(this->stateMutex);

        bool startedNext = false;

        size_t count = this->active.size();
        Player* front = count ? this->active.front() : nullptr;
        bool playerIsFront = (player == front);

        /* only start the next player if the currently active player is the
        one that just finished. */
        if (this->nextPlayer && playerIsFront) {
            this->StartWithPlayer(this->nextPlayer);
            startedNext = true;
        }

        /* we're considered stopped if we were unable to automatically start
        the next track, and the number of players is zero... or the number
        of players is one, and it's the current player. remember, we free
        players asynchronously. */
        if (!startedNext) {
            stopped = !count || (count == 1 && playerIsFront);
        }
    }

    if (stopped) {
        this->Stop();
    }

    this->RemoveFromActive(player);
}

void GaplessTransport::OnPlaybackError(Player* player) {
    this->RaiseStreamEvent(StreamError, player);
    this->SetPlaybackState(PlaybackStopped);
    this->RemoveFromActive(player);
}

void GaplessTransport::SetPlaybackState(int state) {
    bool changed = false;

    {
        LockT lock(this->stateMutex);
        changed = (this->state != state);
        this->state = (PlaybackState) state;
    }

    if (changed) {
        this->PlaybackEvent(state);
    }
}

void GaplessTransport::RaiseStreamEvent(int type, Player* player) {
    this->StreamEvent(type, player->GetUrl());
}
