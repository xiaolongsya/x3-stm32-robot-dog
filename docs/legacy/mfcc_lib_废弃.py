"""mfcc_lib.py — 共享 MFCC 提取模块(训练和推理用同一份实现,保证一致)

参数(严格对齐训练时的 torchaudio MFCC 默认):
- sample_rate = 16000
- n_mfcc = 40
- n_fft = 400
- hop_length = 160
- n_mels = 80
- htk mel scale (跟 torchaudio 默认 'htk' 一致)
- power = 2.0
- center = True
- window = hann
"""
import numpy as np
from scipy.fft import dct as scipy_dct

SR = 16000
N_MFCC = 40
N_FFT = 400
HOP = 160
N_MELS = 80
TARGET_FRAMES = SR // HOP  # 100


def hz_to_mel_htk(hz):
    return 2595.0 * np.log10(1.0 + hz / 700.0)


def mel_to_hz_htk(mel):
    return 700.0 * (10.0 ** (mel / 2595.0) - 1.0)


def build_mel_filterbank():
    """HTK mel scale 三角滤波器组 + Slaney 归一化"""
    fmin = 0.0
    fmax = SR / 2.0
    mel_min = hz_to_mel_htk(fmin)
    mel_max = hz_to_mel_htk(fmax)
    mel_pts = np.linspace(mel_min, mel_max, N_MELS + 2)
    hz_pts = mel_to_hz_htk(mel_pts)
    bin_pts = np.floor((N_FFT + 1) * hz_pts / SR).astype(np.int64)

    n_freqs = N_FFT // 2 + 1
    fb = np.zeros((N_MELS, n_freqs), dtype=np.float32)
    for m in range(N_MELS):
        left = bin_pts[m]
        center = bin_pts[m + 1]
        right = bin_pts[m + 2]
        # Slaney normalization
        slaney = 2.0 / (hz_pts[m + 2] - hz_pts[m]) if hz_pts[m + 2] > hz_pts[m] else 1.0

        for k in range(left, min(center, n_freqs)):
            if center > left:
                fb[m, k] = (k - left) / (center - left)
        for k in range(center, min(right, n_freqs)):
            if right > center:
                fb[m, k] = (right - k) / (right - center)
        fb[m] *= slaney
    return fb


_MEL_FB = None
_WINDOW = None


def _ensure_cache():
    global _MEL_FB, _WINDOW
    if _MEL_FB is None:
        _MEL_FB = build_mel_filterbank()
        _WINDOW = (0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(N_FFT) / (N_FFT - 1))).astype(np.float32)


def mfcc_from_audio(audio):
    """audio: float32 1D numpy @ 16kHz,任意长度(>= N_FFT)

    返回: (40, TARGET_FRAMES=100) float32 MFCC,自动截断/补齐
    """
    _ensure_cache()

    audio = audio.astype(np.float32, copy=False)
    n = len(audio)
    if n < N_FFT:
        # 太短就 pad 0
        audio = np.pad(audio, (0, N_FFT - n))

    # Framing with center=True: pad N_FFT/2 reflect
    pad = N_FFT // 2
    audio_p = np.pad(audio, (pad, pad), mode="edge")

    n_samples = len(audio)
    n_frames = 1 + (n_samples + 2 * pad - N_FFT) // HOP

    # 帧 + 加窗
    frames = np.zeros((n_frames, N_FFT), dtype=np.float32)
    for i in range(n_frames):
        s = i * HOP
        frames[i] = audio_p[s:s + N_FFT] * _WINDOW

    # FFT + 功率谱
    spec = np.fft.rfft(frames, n=N_FFT)
    pow_spec = (spec.real ** 2 + spec.imag ** 2).astype(np.float32)

    # Mel filterbank
    mel = pow_spec @ _MEL_FB.T
    mel = np.log(mel + 1e-10).astype(np.float32)

    # DCT type-2, ortho norm
    mfcc = scipy_dct(mel, type=2, norm="ortho", axis=-1)[:, :N_MFCC].T.astype(np.float32)

    # 截断/补齐到 TARGET_FRAMES
    if mfcc.shape[1] > TARGET_FRAMES:
        mfcc = mfcc[:, :TARGET_FRAMES]
    elif mfcc.shape[1] < TARGET_FRAMES:
        mfcc = np.pad(mfcc, ((0, 0), (0, TARGET_FRAMES - mfcc.shape[1])))
    return mfcc
