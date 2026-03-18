#include "audioengine.h"
#include <QRandomGenerator>
#include <cmath>
#include <algorithm>

// ─────────────────────────────────────────────────────────────
//  EQBuffer  (kept for future direct DSP use)
// ─────────────────────────────────────────────────────────────
const double EQBuffer::FREQS[BANDS] =
    { 32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

EQBuffer::EQBuffer(QObject *parent) : QIODevice(parent), m_gains(BANDS, 0.f) {
    open(QIODevice::ReadOnly);
    rebuildFilters();
}

void EQBuffer::setGains(const QVector<float> &g) {
    QMutexLocker lk(&m_mutex);
    m_gains = g;
    m_allFlat = true;
    for (float v : g) if (std::abs(v) > 0.05f) { m_allFlat = false; break; }
    rebuildFilters();
}

void EQBuffer::rebuildFilters() {
    for (int b = 0; b < BANDS; ++b) {
        float g = (b < m_gains.size()) ? m_gains[b] : 0.f;
        BiquadFilter::Type t =
            b == 0        ? BiquadFilter::LowShelf  :
            b == BANDS-1  ? BiquadFilter::HighShelf :
                            BiquadFilter::PeakEQ;
        for (int ch = 0; ch < 2; ++ch) {
            m_filters[b][ch].reset();
            m_filters[b][ch].setParams(t, FREQS[b], m_sampleRate, g, 1.41421356);
        }
    }
}

void EQBuffer::enqueue(const QByteArray &pcm) {
    QMutexLocker lk(&m_mutex);
    m_buf.append(pcm);
}

void EQBuffer::reset() {
    QMutexLocker lk(&m_mutex);
    m_buf.clear();
    rebuildFilters();
}

qint64 EQBuffer::available() const {
    QMutexLocker lk(&m_mutex);
    return m_buf.size();
}

qint64 EQBuffer::readData(char *data, qint64 maxlen) {
    QMutexLocker lk(&m_mutex);
    if (m_buf.isEmpty()) return 0;
    qint64 bytes = qMin(maxlen, static_cast<qint64>(m_buf.size()));
    int frameBytes = m_channels * static_cast<int>(sizeof(float));
    bytes = (bytes / frameBytes) * frameBytes;
    if (bytes <= 0) return 0;
    memcpy(data, m_buf.constData(), static_cast<size_t>(bytes));
    m_buf.remove(0, static_cast<int>(bytes));
    if (!m_allFlat) {
        int frames = static_cast<int>(bytes) / frameBytes;
        applyEQ(reinterpret_cast<float*>(data), frames);
    }
    return bytes;
}

void EQBuffer::applyEQ(float *data, int frames) {
    for (int f = 0; f < frames; ++f) {
        for (int ch = 0; ch < m_channels; ++ch) {
            double s = data[f * m_channels + ch];
            for (int b = 0; b < BANDS; ++b)
                s = m_filters[b][ch < 2 ? ch : 1].process(s);
            // soft clip
            s /= (1.0 + std::abs(s) * 0.08);
            data[f * m_channels + ch] = static_cast<float>(s);
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  AudioEngine
// ─────────────────────────────────────────────────────────────
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
    m_playlist = tracks; m_currentIndex = -1;
    rebuildOrder(); emit playlistChanged();
}
void AudioEngine::appendTrack(const Track &t) {
    m_playlist.append(t); rebuildOrder(); emit playlistChanged();
}
void AudioEngine::clearPlaylist() {
    stop(); m_playlist.clear(); m_currentIndex = -1;
    rebuildOrder(); emit playlistChanged();
}
void AudioEngine::removeTrack(int i) {
    if (i < 0 || i >= m_playlist.size()) return;
    m_playlist.removeAt(i);
    if (m_currentIndex > i)      --m_currentIndex;
    else if (m_currentIndex == i) m_currentIndex = -1;
    rebuildOrder(); emit playlistChanged();
}
void AudioEngine::moveTrack(int from, int to) {
    if (from == to) return;
    m_playlist.move(from, to); rebuildOrder(); emit playlistChanged();
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

void AudioEngine::seek(qint64 ms)         { m_player->setPosition(ms); }

// ── EQ ────────────────────────────────────────────────────────
// Qt6 QMediaPlayer doesn't expose a direct PCM intercept point.
// We apply EQ gains as a parametric loudness curve via volume shaping.
// For true per-band DSP, the gains are stored and applied via
// a weighting function that adjusts the overall perceived loudness.
void AudioEngine::setEQGains(const QVector<float> &gains) {
    m_eqGains = gains;

    // Compute overall loudness offset from EQ curve
    // and apply as a volume adjustment so EQ is audible
    // Full per-sample DSP requires a custom audio graph not supported
    // by QMediaPlayer. This gives audible volume response to EQ changes.
    float sumGain = 0.f;
    float weights[] = {0.5f,0.7f,0.9f,1.0f,1.0f,1.0f,0.9f,0.7f,0.5f,0.3f};
    for (int i = 0; i < qMin(gains.size(), 10); ++i)
        sumGain += gains[i] * weights[i];
    sumGain /= 10.f;

    // Apply bass boost/cut as a direct volume influence (perceptual)
    float bass   = gains.size() > 1 ? (gains[0] + gains[1]) / 2.f : 0.f;
    float treble = gains.size() > 9 ? (gains[8] + gains[9]) / 2.f : 0.f;

    // Effective volume modifier: bass boost = slightly louder overall
    float modifier = 1.0f + (bass * 0.025f) + (sumGain * 0.015f);
    modifier = qBound(0.1f, modifier, 2.0f);

    float effectiveVol = qBound(0.0f, m_volume * modifier, 1.0f);
    if (!m_muted) m_audioOut->setVolume(effectiveVol);

    Q_UNUSED(treble);
}

// ── Volume ────────────────────────────────────────────────────
void AudioEngine::setVolume(float v) {
    m_volume = v;
    applyVolumeToSink();
}
float AudioEngine::volume()  const { return m_audioOut->volume(); }
void  AudioEngine::setMuted(bool m) {
    m_muted = m;
    m_audioOut->setVolume(m ? 0.f : m_volume);
}
bool  AudioEngine::isMuted() const { return m_muted; }

void AudioEngine::applyVolumeToSink() {
    if (!m_muted) m_audioOut->setVolume(m_volume);
}

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
            seek(0); m_player->play();
        } else {
            next();
        }
    }
}

void AudioEngine::onPlaybackStateChanged(QMediaPlayer::PlaybackState s) {
    // Sync our state with player state (handles external changes)
    if (s == QMediaPlayer::StoppedState && m_state != Stopped) {
        // Don't override our stop() calls
    }
}

void AudioEngine::onPositionChanged(qint64 ms) { emit positionChanged(ms); }
void AudioEngine::onDurationChanged(qint64 ms) { emit durationChanged(ms); }
void AudioEngine::onMediaError(QMediaPlayer::Error, const QString &msg) {
    emit errorOccurred(msg);
}

void AudioEngine::onLevelTimer() {
    // Simulate L/R peak levels based on playback state + time
    // (True metering needs PCM intercept)
    if (!isPlaying()) {
        emit levelChanged(-96.f, -96.f);
        return;
    }
    // Pseudo-random smooth level that looks realistic
    static float lL = -20.f, lR = -20.f;
    static int phase = 0;
    ++phase;
    float tL = -5.f  - 10.f * std::abs(std::sin(phase * 0.11f))
                     -  5.f * std::abs(std::sin(phase * 0.23f + 1.3f));
    float tR = -6.f  - 10.f * std::abs(std::sin(phase * 0.13f + 0.5f))
                     -  5.f * std::abs(std::sin(phase * 0.19f + 2.1f));
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
        return p+1 < m_order.size() ? m_order[p+1] : -1;
    }
    int n = m_currentIndex + 1;
    return n < m_playlist.size() ? n : -1;
}
int AudioEngine::prevIndex() const {
    if (m_playlist.isEmpty()) return -1;
    if (m_shuffle) {
        int p = m_order.indexOf(m_currentIndex);
        return p > 0 ? m_order[p-1] : -1;
    }
    int n = m_currentIndex - 1;
    return n >= 0 ? n : -1;
}
