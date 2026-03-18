#pragma once
#include <QObject>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QAudioDecoder>
#include <QAudioSink>
#include <QAudioFormat>
#include <QIODevice>
#include <QTimer>
#include <QMutex>
#include <QByteArray>
#include <QVector>
#include <QList>
#include <QUrl>
#include "../dsp/biquadfilter.h"
#include "../library/track.h"

// ─────────────────────────────────────────────────────────────
//  EQBuffer  – circular PCM buffer with biquad EQ applied on read
// ─────────────────────────────────────────────────────────────
class EQBuffer : public QIODevice {
    Q_OBJECT
public:
    explicit EQBuffer(QObject *parent = nullptr);
    void setGains(const QVector<float> &gainsDB);
    void enqueue(const QByteArray &pcm);
    void reset();
    qint64 available() const;
    bool   atEnd() const override { return false; }

protected:
    qint64 readData (char *data, qint64 maxlen) override;
    qint64 writeData(const char*, qint64)        override { return 0; }

private:
    static const int    BANDS = 10;
    static const double FREQS[BANDS];

    mutable QMutex m_mutex;
    QByteArray     m_buf;
    QVector<float> m_gains;
    BiquadFilter   m_filters[BANDS][2];  // [band][L/R]
    int            m_sampleRate = 44100;
    int            m_channels   = 2;
    bool           m_allFlat    = true;

    void rebuildFilters();
    void applyEQ(float *data, int frames);
};

// ─────────────────────────────────────────────────────────────
//  AudioEngine  – QMediaPlayer for transport + optional EQ sink
// ─────────────────────────────────────────────────────────────
class AudioEngine : public QObject {
    Q_OBJECT
public:
    enum RepeatMode { RepeatNone=0, RepeatOne, RepeatAll };
    enum State      { Stopped=0, Playing, Paused };

    explicit AudioEngine(QObject *parent = nullptr);
    ~AudioEngine();

    // Playlist
    void setPlaylist(const QList<Track> &tracks);
    void appendTrack(const Track &t);
    void clearPlaylist();
    void removeTrack(int index);
    void moveTrack(int from, int to);
    const QList<Track>& playlist() const { return m_playlist; }

    // Transport
    void playIndex(int index);
    void play();
    void pause();
    void stop();
    void next();
    void previous();
    void seek(qint64 ms);

    // EQ – gains in dB per band
    void setEQGains(const QVector<float> &gains);

    // Properties
    void  setVolume(float v);
    float volume()       const;
    void  setMuted(bool m);
    bool  isMuted()      const;
    void  setShuffleMode(bool s);
    bool  shuffleMode()  const { return m_shuffle; }
    void  setRepeatMode(RepeatMode r);
    RepeatMode repeatMode() const { return m_repeat; }
    void  setPlaybackRate(float r);
    float playbackRate() const;

    State  state()        const { return m_state; }
    bool   isPlaying()    const { return m_state == Playing; }
    bool   isPaused()     const { return m_state == Paused;  }
    bool   isStopped()    const { return m_state == Stopped; }
    int    currentIndex() const { return m_currentIndex; }
    qint64 position()     const;
    qint64 duration()     const;

signals:
    void positionChanged(qint64 ms);
    void durationChanged(qint64 ms);
    void stateChanged(AudioEngine::State s);
    void trackChanged(int index);
    void playlistChanged();
    void errorOccurred(const QString &msg);
    void levelChanged(float leftDB, float rightDB);

private slots:
    void onMediaStatus(QMediaPlayer::MediaStatus status);
    void onPlaybackStateChanged(QMediaPlayer::PlaybackState s);
    void onPositionChanged(qint64 ms);
    void onDurationChanged(qint64 ms);
    void onMediaError(QMediaPlayer::Error e, const QString &msg);
    void onLevelTimer();

private:
    // Main playback via QMediaPlayer (reliable on all platforms)
    QMediaPlayer  *m_player;
    QAudioOutput  *m_audioOut;

    QList<Track>   m_playlist;
    int            m_currentIndex = -1;
    bool           m_shuffle  = false;
    RepeatMode     m_repeat   = RepeatNone;
    QList<int>     m_order;
    State          m_state    = Stopped;
    float          m_volume   = 0.70f;
    bool           m_muted    = false;

    // EQ gains stored for applying via QAudioDecoder background analysis
    QVector<float> m_eqGains;

    // Level metering timer
    QTimer        *m_levelTimer;
    int            m_levelPhase = 0;

    void rebuildOrder();
    int  nextIndex() const;
    int  prevIndex() const;
    void applyVolumeToSink();
};
