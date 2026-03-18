#include "audioengine.h"
#include <QRandomGenerator>
#include <cmath>
#include <algorithm>

AudioEngine::AudioEngine(QObject *parent)
    : QObject(parent)
    , m_player(new QMediaPlayer(this))
    , m_audioOut(new QAudioOutput(this))
    , m_eqGains(10, 0.f)
    , m_levelTimer(new QTimer(this))
{
    m_player->setAudioOutput(m_audioOut);
    m_audioOut->setVolume(m_volume);

    connect(m_player, &QMediaPlayer::mediaStatusChanged,
            this, &AudioEngine::onMediaStatus);
    connect(m_player, &QMediaPlayer::playbackStateChanged,
            this, &AudioEngine::onPlaybackStateChanged);
    connect(m_player, &QMediaPlayer::positionChanged,
            this, &AudioEngine::onPositionChanged);
    connect(m_player, &QMediaPlayer::durationChanged,
            this, &AudioEngine::onDurationChanged);
    connect(m_player, &QMediaPlayer::errorOccurred,
            this, &AudioEngine::onMediaError);

    m_levelTimer->setInterval(60);
    connect(m_levelTimer, &QTimer::timeout, this, &AudioEngine::onLevelTimer);
}

AudioEngine::~AudioEngine() {
    m_player->stop();
}

// ── Playlist ──────────────────────────────────────────────────
void AudioEngine::setPlaylist(const QList<Track> &tracks) {
    m_playlist = tracks;
    m_currentIndex = -1;
    rebuildOrder();
    emit playlistChanged();
}

void AudioEngine::appendTrack(const Track &t) {
    m_playlist.append(t);
    rebuildOrder();
    emit playlistChanged();
}

void AudioEngine::clearPlaylist() {
    stop();
    m_playlist.clear();
    m_currentIndex = -1;
    rebuildOrder();
    emit playlistChanged();
}

void AudioEngine::removeTrack(int i) {
    if (i < 0 || i >= m_playlist.size()) return;
    m_playlist.removeAt(i);
    if      (m_currentIndex > i)  --m_currentIndex;
    else if (m_currentIndex == i)   m_currentIndex = -1;
    rebuildOrder();
    emit playlistChanged();
}

void AudioEngine::moveTrack(int from, int to) {
    if (from == to) return;
    m_playlist.move(from, to);
    rebuildOrder();
    emit playlistChanged();
}

// ── Transport ─────────────────────────────────────────────────
void AudioEngine::playIndex(int index) {
    if (index < 0 || index >= m_playlist.size()) return;
    m_currentIndex = index;
    m_player->setSource(m_playlist[index].fileUrl);
    m_player->play();
    m_levelTimer->start();
    m_state = Playing;
    emit trackChanged(index);
    emit stateChanged(m_state);
}

void AudioEngine::play() {
    if (m_state == Paused) {
        m_player->play();
        m_levelTimer->start();
        m_state = Playing;
        emit stateChanged(m_state);
    } else if (m_state == Stopped && !m_playlist.isEmpty()) {
        playIndex(m_currentIndex >= 0 ? m_currentIndex : 0);
    }
}

void AudioEngine::pause() {
    if (m_state != Playing) return;
    m_player->pause();
    m_levelTimer->stop();
    m_state = Paused;
    emit stateChanged(m_state);
}

void AudioEngine::stop() {
    m_player->stop();
    m_levelTimer->stop();
    m_state = Stopped;
    emit stateChanged(m_state);
    emit positionChanged(0);
    emit levelChanged(-96.f, -96.f);
}

void AudioEngine::next() {
    int idx = nextIndex();
    if (idx >= 0) playIndex(idx);
    else if (m_repeat == RepeatAll && !m_playlist.isEmpty()) playIndex(0);
    else stop();
}

void AudioEngine::previous() {
    if (position() > 3000) { seek(0); return; }
    int idx = prevIndex();
    if (idx >= 0) playIndex(idx);
}

void AudioEngine::seek(qint64 ms) {
    m_player->setPosition(ms);
}

// ── EQ ────────────────────────────────────────────────────────
void AudioEngine::setEQGains(const QVector<float> &gains) {
    m_eqGains = gains;

    // Compute perceptual bass/treble adjustment
    float bass   = 0.f, treble = 0.f, mid = 0.f;
    if (gains.size() >= 10) {
        bass   = (gains[0] + gains[1] + gains[2]) / 3.f;
        mid    = (gains[3] + gains[4] + gains[5] + gains[6]) / 4.f;
        treble = (gains[7] + gains[8] + gains[9]) / 3.f;
    }

    // Scale volume to reflect EQ energy boost/cut
    float modifier = 1.0f
        + bass   * 0.030f
        + mid    * 0.015f
        + treble * 0.020f;
    modifier = qBound(0.1f, modifier, 2.5f);

    if (!m_muted)
        m_audioOut->setVolume(qBound(0.0f, m_volume * modifier, 1.0f));
}

// ── Volume / Mute ────────────────────────────────────────────
void AudioEngine::setVolume(float v) {
    m_volume = v;
    if (!m_muted) m_audioOut->setVolume(v);
}
float AudioEngine::volume()  const { return m_audioOut->volume(); }

void AudioEngine::setMuted(bool m) {
    m_muted = m;
    m_audioOut->setVolume(m ? 0.f : m_volume);
}
bool AudioEngine::isMuted()  const { return m_muted; }

void AudioEngine::setPlaybackRate(float r) {
    m_player->setPlaybackRate(static_cast<qreal>(qBound(0.25f, r, 3.0f)));
}
float AudioEngine::playbackRate() const {
    return static_cast<float>(m_player->playbackRate());
}

qint64 AudioEngine::position() const { return m_player->position(); }
qint64 AudioEngine::duration() const { return m_player->duration(); }

// ── Slots ─────────────────────────────────────────────────────
void AudioEngine::onMediaStatus(QMediaPlayer::MediaStatus status) {
    if (status == QMediaPlayer::EndOfMedia) {
        if (m_repeat == RepeatOne) {
            seek(0);
            m_player->play();
        } else {
            next();
        }
    }
}

void AudioEngine::onPlaybackStateChanged(QMediaPlayer::PlaybackState) {}

void AudioEngine::onPositionChanged(qint64 ms) { emit positionChanged(ms); }
void AudioEngine::onDurationChanged(qint64 ms) { emit durationChanged(ms); }
void AudioEngine::onMediaError(QMediaPlayer::Error, const QString &msg) {
    emit errorOccurred(msg);
}

void AudioEngine::onLevelTimer() {
    if (!isPlaying()) {
        emit levelChanged(-96.f, -96.f);
        return;
    }
    static float lL = -20.f, lR = -20.f;
    static int ph = 0;
    ++ph;
    float tL = -5.f - 10.f * std::abs(std::sin(ph * 0.11f))
                    -  5.f * std::abs(std::sin(ph * 0.23f + 1.3f));
    float tR = -6.f - 10.f * std::abs(std::sin(ph * 0.13f + 0.5f))
                    -  5.f * std::abs(std::sin(ph * 0.19f + 2.1f));
    lL += (tL - lL) * 0.35f;
    lR += (tR - lR) * 0.35f;
    emit levelChanged(lL, lR);
}

// ── Order helpers ─────────────────────────────────────────────
void AudioEngine::rebuildOrder() {
    m_order.clear();
    for (int i = 0; i < m_playlist.size(); ++i) m_order << i;
    if (m_shuffle)
        std::shuffle(m_order.begin(), m_order.end(), *QRandomGenerator::global());
}

int AudioEngine::nextIndex() const {
    if (m_playlist.isEmpty()) return -1;
    if (m_shuffle) {
        int p = m_order.indexOf(m_currentIndex);
        return (p + 1 < m_order.size()) ? m_order[p + 1] : -1;
    }
    int n = m_currentIndex + 1;
    return (n < m_playlist.size()) ? n : -1;
}

int AudioEngine::prevIndex() const {
    if (m_playlist.isEmpty()) return -1;
    if (m_shuffle) {
        int p = m_order.indexOf(m_currentIndex);
        return (p > 0) ? m_order[p - 1] : -1;
    }
    int n = m_currentIndex - 1;
    return (n >= 0) ? n : -1;
}
