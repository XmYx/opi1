//! op1-og-native language spike, Rust side. Same behaviour and algorithms as ../cpp/spike.cpp; see
//! ../README.md. ALSA duplex audio at 44.1 kHz, ALSA sequencer MIDI input, an SDL3 window, measured.
use std::cell::UnsafeCell;
use std::ffi::{CStr, CString};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering};

use alsa::pcm::{Access, Format, Frames, HwParams, PCM};
use alsa::seq::{self, EventType, PortCap, PortSubscribe, PortType};
use alsa::{Direction, PollDescriptors, ValueOr};
use sdl3_sys::everything::*;

const VOICES: usize = 6;
const W: usize = 320;
const H: usize = 160;
const PI: f32 = 3.14159265358979;

static RUN: AtomicBool = AtomicBool::new(true);

fn now_ns() -> i64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    ts.tv_sec as i64 * 1_000_000_000 + ts.tv_nsec as i64
}
fn thread_cpu_ns() -> i64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_THREAD_CPUTIME_ID, &mut ts) };
    ts.tv_sec as i64 * 1_000_000_000 + ts.tv_nsec as i64
}
fn running() -> bool {
    RUN.load(Ordering::Relaxed)
}
fn stop() {
    RUN.store(false, Ordering::Relaxed)
}

#[derive(Clone)]
struct Config {
    device: String,
    rate: u32,
    period: u32,
    periods: u32,
    duration: f64,
    window: bool,
    capture: bool,
    mute: bool,
    rt: bool,
    rt_prio: i32,
    json: String,
    notes: String,
    shot: String,
    bench: f64,
}
impl Default for Config {
    fn default() -> Self {
        Config {
            device: "hw:0,0".into(),
            rate: 44100,
            period: 64,
            periods: 2,
            duration: 30.0,
            window: true,
            capture: true,
            mute: false,
            rt: false,
            rt_prio: 20,
            json: String::new(),
            notes: String::new(),
            shot: String::new(),
            bench: 0.0,
        }
    }
}

// ---------------------------------------------------------------- lock-free primitives

#[repr(align(64))]
struct Pad(AtomicUsize);

/// Single-producer single-consumer ring. push() on one thread, pop() on another; no locks, no allocation.
struct Spsc<T: Copy, const N: usize> {
    buf: [UnsafeCell<T>; N],
    head: Pad,
    tail: Pad,
}
unsafe impl<T: Copy + Send, const N: usize> Sync for Spsc<T, N> {}
impl<T: Copy + Default, const N: usize> Spsc<T, N> {
    fn new() -> Self {
        assert!(N.is_power_of_two());
        Spsc { buf: std::array::from_fn(|_| UnsafeCell::new(T::default())), head: Pad(AtomicUsize::new(0)), tail: Pad(AtomicUsize::new(0)) }
    }
    fn push(&self, v: T) -> bool {
        let h = self.head.0.load(Ordering::Relaxed);
        if h.wrapping_sub(self.tail.0.load(Ordering::Acquire)) == N {
            return false;
        }
        unsafe { *self.buf[h & (N - 1)].get() = v };
        self.head.0.store(h.wrapping_add(1), Ordering::Release);
        true
    }
    fn pop(&self) -> Option<T> {
        let t = self.tail.0.load(Ordering::Relaxed);
        if t == self.head.0.load(Ordering::Acquire) {
            return None;
        }
        let v = unsafe { *self.buf[t & (N - 1)].get() };
        self.tail.0.store(t.wrapping_add(1), Ordering::Release);
        Some(v)
    }
}

#[derive(Clone, Copy, Default)]
struct NoteEvent {
    t_recv: i64,
    on: u8,
    note: u8,
    vel: u8,
    src: u8, // 1 = MIDI, 2 = window keyboard
}

#[derive(Clone, Copy)]
struct Scope {
    wave: [f32; W],
    active: u32,
    period: u64,
}
impl Default for Scope {
    fn default() -> Self {
        Scope { wave: [0.0; W], active: 0, period: 0 }
    }
}

/// Wait-free triple buffer: the audio thread publishes the newest scope, the UI takes the newest one.
struct TripleBuffer {
    slot: [UnsafeCell<Scope>; 3],
    middle: AtomicU32,
    back: UnsafeCell<u32>,
    front: UnsafeCell<u32>,
}
unsafe impl Sync for TripleBuffer {}
const FRESH: u32 = 4;
impl TripleBuffer {
    fn new() -> Self {
        TripleBuffer {
            slot: std::array::from_fn(|_| UnsafeCell::new(Scope::default())),
            middle: AtomicU32::new(1),
            back: UnsafeCell::new(0),
            front: UnsafeCell::new(2),
        }
    }
    /// Producer only.
    #[allow(clippy::mut_from_ref)]
    unsafe fn back(&self) -> &mut Scope {
        &mut *self.slot[*self.back.get() as usize].get()
    }
    /// Producer only.
    unsafe fn publish(&self) {
        let b = *self.back.get();
        *self.back.get() = self.middle.swap(b | FRESH, Ordering::AcqRel) & 3;
    }
    /// Consumer only.
    unsafe fn read(&self) -> &Scope {
        if self.middle.load(Ordering::Acquire) & FRESH != 0 {
            let f = *self.front.get();
            *self.front.get() = self.middle.swap(f, Ordering::AcqRel) & 3;
        }
        &*self.slot[*self.front.get() as usize].get()
    }
}

// ---------------------------------------------------------------- DSP, float

#[inline]
fn poly_blep(mut t: f32, dt: f32) -> f32 {
    if t < dt {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if t > 1.0 - dt {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    0.0
}

#[derive(Clone, Copy)]
struct Voice {
    phase: f32,
    inc: f32,
    env: f32,
    amp: f32,
    ic1: f32,
    ic2: f32,
    lfo: f32,
    lfo_inc: f32,
    gate: bool,
    note: i32,
    age: u64,
}
impl Default for Voice {
    fn default() -> Self {
        Voice { phase: 0.0, inc: 0.0, env: 0.0, amp: 0.0, ic1: 0.0, ic2: 0.0, lfo: 0.0, lfo_inc: 0.0, gate: false, note: -1, age: 0 }
    }
}

struct Synth {
    sr: f32,
    v: [Voice; VOICES],
    clock: u64,
    attack_step: f32,
    release_mul: f32,
    hp_x: [f32; 2],
    hp_y: [f32; 2],
}

impl Synth {
    fn new(sr: f32) -> Self {
        Synth {
            sr,
            v: [Voice::default(); VOICES],
            clock: 0,
            attack_step: 1.0 / (0.005 * sr),
            release_mul: (-1.0f64 / (0.2 * sr as f64)).exp() as f32,
            hp_x: [0.0; 2],
            hp_y: [0.0; 2],
        }
    }
    fn note_on(&mut self, note: i32, vel: i32) {
        let mut pick = usize::MAX;
        for i in 0..VOICES {
            if self.v[i].note < 0 {
                pick = i;
                break;
            }
        }
        if pick == usize::MAX {
            // steal the oldest
            pick = 0;
            for i in 1..VOICES {
                if self.v[i].age < self.v[pick].age {
                    pick = i;
                }
            }
        }
        self.clock += 1;
        let vo = &mut self.v[pick];
        vo.note = note;
        vo.inc = (440.0 * 2f64.powf((note - 69) as f64 / 12.0) / self.sr as f64) as f32;
        vo.lfo_inc = (2.0 + 0.37 * (note % 5) as f32) / self.sr; // cutoff LFO, 2-3.5 Hz
        vo.amp = 0.25 * vel as f32 / 127.0;
        vo.gate = true;
        vo.age = self.clock;
    }
    fn note_off(&mut self, note: i32) {
        for vo in self.v.iter_mut() {
            if vo.note == note && vo.gate {
                vo.gate = false;
            }
        }
    }
    fn active_mask(&self) -> u32 {
        let mut m = 0;
        for i in 0..VOICES {
            if self.v[i].note >= 0 {
                m |= 1 << i;
            }
        }
        m
    }

    fn render_voice(vo: &mut Voice, sr: f32, attack_step: f32, release_mul: f32, l: &mut [f32], r: &mut [f32]) {
        let k = 1.0f32; // SVF damping (resonance 0.5)
        for i in 0..l.len() {
            if vo.gate {
                vo.env += attack_step;
                if vo.env > 1.0 {
                    vo.env = 1.0;
                }
            } else {
                vo.env *= release_mul;
            }
            let t = vo.phase;
            let y = 2.0 * t - 1.0 - poly_blep(t, vo.inc);
            vo.phase += vo.inc;
            if vo.phase >= 1.0 {
                vo.phase -= 1.0;
            }
            vo.lfo += vo.lfo_inc;
            if vo.lfo >= 1.0 {
                vo.lfo -= 1.0;
            }
            let tri = 2.0 * (2.0 * vo.lfo - 1.0).abs() - 1.0;
            let fc = 300.0 + 6000.0 * vo.env * (0.6 + 0.4 * tri);
            let g = (PI * fc / sr).tan();
            let a1 = 1.0 / (1.0 + g * (g + k));
            let a2 = g * a1;
            let a3 = g * a2;
            let v3 = y - vo.ic2;
            let v1 = a1 * vo.ic1 + a2 * v3;
            let v2 = vo.ic2 + a2 * vo.ic1 + a3 * v3;
            vo.ic1 = 2.0 * v1 - vo.ic1;
            vo.ic2 = 2.0 * v2 - vo.ic2;
            let out = v2 * vo.env * vo.amp;
            l[i] += out;
            r[i] += out;
        }
        if !vo.gate && vo.env < 1e-5 {
            vo.note = -1;
            vo.env = 0.0;
        }
    }

    /// `input` may be None (no capture). Outputs are overwritten.
    fn render(&mut self, l: &mut [f32], r: &mut [f32], input: Option<(&[f32], &[f32])>) {
        l.fill(0.0);
        r.fill(0.0);
        let (sr, a, rm) = (self.sr, self.attack_step, self.release_mul);
        for vo in self.v.iter_mut() {
            if vo.note >= 0 {
                Self::render_voice(vo, sr, a, rm, l, r);
            }
        }
        if let Some((in_l, in_r)) = input {
            // input monitor through a DC blocker
            for (c, (inp, out)) in [(in_l, &mut *l), (in_r, &mut *r)].into_iter().enumerate() {
                for i in 0..out.len() {
                    let x = inp[i];
                    let y = x - self.hp_x[c] + 0.995 * self.hp_y[c];
                    self.hp_x[c] = x;
                    self.hp_y[c] = y;
                    out[i] += 0.5 * y;
                }
            }
        }
    }
}

// ---------------------------------------------------------------- DSP, Blackfin-style fixed point

#[inline]
fn sat16(x: i32) -> i16 {
    x.clamp(-32768, 32767) as i16
}
#[inline]
fn mult_fr16(a: i16, b: i16) -> i16 {
    sat16((a as i32 * b as i32) >> 15)
}
#[inline]
fn add_sat32(a: i32, b: i32) -> i32 {
    a.saturating_add(b)
}

#[derive(Clone, Copy, Default)]
struct FixVoice {
    phase: u32,
    inc: u32,
    env: i32,
    att: i32,
    low: i16,
    band: i16,
    amp: i16,
    gate: bool,
}
impl FixVoice {
    fn render(&mut self, acc: &mut [i32]) {
        let q: i16 = 16384;
        for a in acc.iter_mut() {
            if self.gate {
                self.env = add_sat32(self.env, self.att);
            } else {
                self.env -= self.env >> 13;
            }
            let saw = (self.phase >> 16) as u16 as i16;
            self.phase = self.phase.wrapping_add(self.inc);
            let env15 = (self.env >> 16) as i16;
            let f = (1000 + ((env15 as i32 * 12000) >> 15)) as i16;
            self.low = sat16(self.low as i32 + mult_fr16(f, self.band) as i32);
            let high = sat16(saw as i32 - self.low as i32 - mult_fr16(q, self.band) as i32);
            self.band = sat16(self.band as i32 + mult_fr16(f, high) as i32);
            let o = mult_fr16(mult_fr16(self.low, env15), self.amp);
            *a = add_sat32(*a, (o as i32) << 14);
        }
    }
}

// ---------------------------------------------------------------- stats helpers

#[derive(Default)]
struct Pct {
    p50: f64,
    p99: f64,
    max: f64,
    mean: f64,
    n: usize,
}
fn pct(mut v: Vec<f32>) -> Pct {
    let mut p = Pct { n: v.len(), ..Default::default() };
    if v.is_empty() {
        return p;
    }
    v.sort_by(|a, b| a.total_cmp(b));
    let sum: f64 = v.iter().map(|&x| x as f64).sum();
    p.mean = sum / v.len() as f64;
    p.p50 = v[v.len() / 2] as f64;
    p.p99 = v[(v.len() - 1).min((v.len() as f64 * 0.99) as usize)] as f64;
    p.max = *v.last().unwrap() as f64;
    p
}
fn pct_json(p: &Pct) -> String {
    format!("{{\"n\":{},\"mean\":{:.4},\"p50\":{:.4},\"p99\":{:.4},\"max\":{:.4}}}", p.n, p.mean, p.p50, p.p99, p.max)
}

// ---------------------------------------------------------------- offline DSP benchmark

/// `sr` comes from the command line so neither compiler can fold the sample rate into the DSP.
fn run_bench(secs: f64, sr: u32) -> i32 {
    let chord_len = (sr / 4) as u64;
    const BLOCK: u64 = 64;
    let frames = (secs * sr as f64) as u64;
    let (mut best_float, mut best_fixed) = (1e30f64, 1e30f64);
    let mut sum_float = 0.0f64;
    let mut sum_fixed = 0u64;
    for _run in 0..5 {
        {
            // float
            let mut s = Synth::new(sr as f32);
            let mut l = [0.0f32; BLOCK as usize];
            let mut r = [0.0f32; BLOCK as usize];
            let mut acc = 0.0f64;
            let t0 = now_ns();
            let mut chord: i32 = 0;
            let mut f = 0u64;
            while f < frames {
                if f % chord_len < BLOCK {
                    for j in 0..VOICES as i32 {
                        s.note_off(48 + ((chord - 1) * 7 + j * 4 + 36) % 36);
                    }
                    for j in 0..VOICES as i32 {
                        s.note_on(48 + (chord * 7 + j * 4) % 36, 100);
                    }
                    chord += 1;
                }
                s.render(&mut l, &mut r, None);
                for x in l.iter() {
                    acc += x.abs() as f64;
                }
                f += BLOCK;
            }
            let dt = (now_ns() - t0) as f64;
            best_float = best_float.min(dt);
            sum_float = acc;
        }
        {
            // fixed
            let mut fv = [FixVoice::default(); VOICES];
            let mut accb = [0i32; BLOCK as usize];
            let mut check = 0u64;
            let t0 = now_ns();
            let mut chord: i32 = 0;
            let mut f = 0u64;
            while f < frames {
                if f % chord_len < BLOCK {
                    for (j, v) in fv.iter_mut().enumerate() {
                        let note = 48 + (chord * 7 + j as i32 * 4) % 36;
                        v.inc = (440.0 * 2f64.powf((note - 69) as f64 / 12.0) / sr as f64 * 4294967296.0) as u32;
                        v.att = (2147483647.0 / (0.005 * sr as f64)) as i32;
                        v.amp = 8192;
                        v.gate = (chord & 1) == 0;
                    }
                    chord += 1;
                }
                accb.fill(0);
                for v in fv.iter_mut() {
                    v.render(&mut accb);
                }
                for &a in accb.iter() {
                    check = check.wrapping_add(a as u32 as u64);
                }
                f += BLOCK;
            }
            let dt = (now_ns() - t0) as f64;
            best_fixed = best_fixed.min(dt);
            sum_fixed = check;
        }
    }
    let audio_ns = secs * 1e9;
    let vs = (frames * VOICES as u64) as f64;
    println!(
        "{{\"lang\":\"rust\",\"bench_seconds\":{:.1},\"voices\":{},\"float\":{{\"wall_ms\":{:.3},\"realtime_x\":{:.1},\"ns_per_voice_sample\":{:.3},\"checksum\":{:.6}}},\"fixed\":{{\"wall_ms\":{:.3},\"realtime_x\":{:.1},\"ns_per_voice_sample\":{:.3},\"checksum\":{}}}}}",
        secs, VOICES, best_float / 1e6, audio_ns / best_float, best_float / vs, sum_float,
        best_fixed / 1e6, audio_ns / best_fixed, best_fixed / vs, sum_fixed
    );
    0
}

// ---------------------------------------------------------------- realtime priority via rtkit

fn request_rt(prio: i32) -> bool {
    let rl = libc::rlimit { rlim_cur: 200000, rlim_max: 200000 }; // rtkit requires RLIMIT_RTTIME
    unsafe { libc::setrlimit(libc::RLIMIT_RTTIME, &rl) };
    let pid = std::process::id();
    let tid = unsafe { libc::syscall(libc::SYS_gettid) };
    let _ = std::process::Command::new("busctl")
        .args(["--system", "call", "org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1",
               "org.freedesktop.RealtimeKit1", "MakeThreadRealtimeWithPID", "ttu"])
        .arg(pid.to_string())
        .arg(tid.to_string())
        .arg(prio.to_string())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status();
    // rtkit grants SCHED_RR|RESET_ON_FORK
    let policy = unsafe { libc::sched_getscheduler(0) } & !libc::SCHED_RESET_ON_FORK;
    policy == libc::SCHED_RR || policy == libc::SCHED_FIFO
}

// ---------------------------------------------------------------- audio

struct Pcm {
    h: PCM,
    s32: bool,
    period: usize,
    buffer: usize,
}

fn open_pcm(c: &Config, dir: Direction) -> Result<Pcm, String> {
    let h = PCM::new(&c.device, dir, false).map_err(|e| format!("snd_pcm_open: {e}"))?;
    let s32;
    {
        let hw = HwParams::any(&h).map_err(|e| format!("hw any: {e}"))?;
        hw.set_access(Access::RWInterleaved).map_err(|e| format!("access: {e}"))?;
        s32 = hw.test_format(Format::S32LE).is_ok();
        hw.set_format(if s32 { Format::S32LE } else { Format::S16LE }).map_err(|e| format!("format: {e}"))?;
        hw.set_channels(2).map_err(|e| format!("channels: {e}"))?;
        hw.set_rate_resample(false).map_err(|e| format!("resample: {e}"))?;
        hw.set_rate(c.rate, ValueOr::Nearest).map_err(|e| format!("rate: {e}"))?;
        hw.set_period_size_near(c.period as Frames, ValueOr::Nearest).map_err(|e| format!("period: {e}"))?;
        // The hardware buffer is larger than the latency target so a late wakeup never blocks a write;
        // latency is the fill level (c.periods periods queued ahead), as PipeWire and JACK's -n do it.
        let np = (c.periods + 2).max(4);
        if hw.set_periods(np, ValueOr::Nearest).is_err() {
            let _ = hw.set_buffer_size_near((c.period * np) as Frames);
        }
        h.hw_params(&hw).map_err(|e| format!("hw_params: {e}"))?;
    }
    let (period, buffer) = {
        let hw = h.hw_params_current().map_err(|e| format!("hw current: {e}"))?;
        (hw.get_period_size().unwrap_or(0) as usize, hw.get_buffer_size().unwrap_or(0) as usize)
    };
    {
        let sw = h.sw_params_current().map_err(|e| format!("sw current: {e}"))?;
        let boundary = sw.get_boundary().unwrap_or(Frames::MAX);
        let _ = sw.set_start_threshold(boundary); // started explicitly
        // playback-only: wake when the fill drops to (periods - 1) periods; duplex: capture paces the loop
        let mut avail_min = period;
        let keep = (c.periods as usize - 1) * period;
        if dir == Direction::Playback && !c.capture && buffer > keep {
            avail_min = buffer - keep;
        }
        let _ = sw.set_avail_min(avail_min as Frames);
        h.sw_params(&sw).map_err(|e| format!("sw_params: {e}"))?;
    }
    Ok(Pcm { h, s32, period, buffer })
}

#[derive(Clone, Copy, Default)]
struct NoteLog {
    t_recv: i64,
    t_out: i64,
    on: u8,
    note: u8,
    vel: u8,
    src: u8,
}

#[derive(Default)]
struct AudioResult {
    ok: bool,
    err: String,
    fmt: &'static str,
    period: usize,
    buffer: usize,
    capture: bool,
    linked: bool,
    rt: bool,
    periods_run: u64,
    xrun_play: u64,
    xrun_cap: u64,
    dsp_us: Vec<f32>,
    delay_frames: Vec<f32>,
    notes: Vec<NoteLog>,
    n_notes: usize,
    thread_cpu_s: f64,
    wall_s: f64,
}

type Ring = Spsc<NoteEvent, 1024>;

/// Writes every element (volatile, so it is not optimised away) to map the pages before realtime use.
fn prefault<T: Copy + Default>(v: &mut [T]) {
    for x in v.iter_mut() {
        unsafe { std::ptr::write_volatile(x, T::default()) };
    }
}

/// Interleaved device buffers of either sample width.
enum Buf {
    S32(Vec<i32>),
    S16(Vec<i16>),
}
impl Buf {
    fn new(s32: bool, n: usize) -> Buf {
        if s32 { Buf::S32(vec![0; n]) } else { Buf::S16(vec![0; n]) }
    }
    fn write(&self, pcm: &PCM) -> alsa::Result<usize> {
        match self {
            Buf::S32(b) => pcm.io_i32()?.writei(b),
            Buf::S16(b) => pcm.io_i16()?.writei(b),
        }
    }
    fn read(&mut self, pcm: &PCM, from: usize, frames: usize) -> alsa::Result<usize> {
        match self {
            Buf::S32(b) => pcm.io_i32()?.readi(&mut b[from * 2..(from + frames) * 2]),
            Buf::S16(b) => pcm.io_i16()?.readi(&mut b[from * 2..(from + frames) * 2]),
        }
    }
}

fn audio_thread(c: &Config, midi_ring: &Ring, ui_ring: &Ring, tb: &TripleBuffer) -> AudioResult {
    let mut res = AudioResult::default();
    let play = match open_pcm(c, Direction::Playback) {
        Ok(p) => p,
        Err(e) => {
            res.err = format!("playback {e}");
            stop();
            return res;
        }
    };
    let mut cap = None;
    if c.capture {
        match open_pcm(c, Direction::Capture) {
            Ok(p) if p.period == play.period && p.s32 == play.s32 => cap = Some(p),
            Ok(_) => eprintln!("audio: capture params differ from playback, playback only"),
            Err(e) => eprintln!("audio: capture unavailable ({e}), playback only"),
        }
    }
    res.capture = cap.is_some();
    res.linked = cap.as_ref().map(|cp| cp.h.link(&play.h).is_ok()).unwrap_or(false);
    let p = play.period;
    res.period = p;
    res.buffer = play.buffer;
    res.fmt = if play.s32 { "S32_LE" } else { "S16_LE" };
    let s32 = play.s32;

    let mut obuf = Buf::new(s32, p * 2);
    let mut ibuf = Buf::new(s32, p * 2);
    let silence = Buf::new(s32, p * 2);
    let (mut l, mut r, mut il, mut ir) = (vec![0.0f32; p], vec![0.0f32; p], vec![0.0f32; p], vec![0.0f32; p]);
    let cap_periods = (c.duration * c.rate as f64 / p as f64) as usize + 4096;
    res.dsp_us = vec![0.0; cap_periods];
    res.delay_frames = vec![0.0; cap_periods];
    // vec![0; n] is calloc'd and mapped lazily: touch every page now so the loop never page-faults
    // (the C++ side's std::vector::assign writes its zeros up front).
    for v in [&mut res.dsp_us, &mut res.delay_frames, &mut l, &mut r, &mut il, &mut ir] {
        prefault(v);
    }
    for b in [&mut obuf, &mut ibuf] {
        match b {
            Buf::S32(v) => prefault(v),
            Buf::S16(v) => prefault(v),
        }
    }
    res.notes = vec![NoteLog::default(); 200000];
    let mut hist = [0.0f32; W];
    let mut hist_pos = 0usize;
    let mut synth = Synth::new(c.rate as f32);
    let gain = if c.mute { 0.0f32 } else { 1.0 };
    let mut pending = [0usize; 64];

    if c.rt {
        res.rt = request_rt(c.rt_prio);
    }

    let restart = |res: &AudioResult| {
        if let Some(cp) = &cap {
            let _ = cp.h.drop();
        }
        let _ = play.h.drop();
        let _ = play.h.prepare();
        if let Some(cp) = &cap {
            let _ = cp.h.prepare();
        }
        for _ in 0..c.periods {
            let _ = silence.write(&play.h); // latency target
        }
        let _ = play.h.start();
        if let Some(cp) = &cap {
            if !res.linked {
                let _ = cp.h.start();
            }
        }
    };

    restart(&res);
    let t_start = now_ns();
    let cpu_start = thread_cpu_ns();
    let t_end = t_start + (c.duration * 1e9) as i64;
    let keep = (c.periods as usize - 1) * p;
    let mut idx = 0usize;
    while running() {
        if now_ns() >= t_end {
            break; // also ends runs that xrun on every period
        }
        if let Some(cp) = &cap {
            let mut got = 0usize;
            let mut xr = false;
            while got < p {
                match ibuf.read(&cp.h, got, p - got) {
                    Ok(n) => got += n,
                    Err(_) => {
                        xr = true;
                        break;
                    }
                }
            }
            if xr {
                res.xrun_cap += 1;
                restart(&res);
                continue;
            }
        } else {
            let w = play.h.wait(Some(1000));
            let avail = play.h.avail_update();
            match (w, avail) {
                (Ok(_), Ok(a)) => {
                    if (a as usize) < play.buffer - keep {
                        continue;
                    }
                }
                _ => {
                    res.xrun_play += 1;
                    restart(&res);
                    continue;
                }
            }
        }
        let t0 = now_ns();
        // events
        let mut npend = 0usize;
        for ring in [midi_ring, ui_ring] {
            while let Some(ev) = ring.pop() {
                if ev.on != 0 {
                    synth.note_on(ev.note as i32, ev.vel as i32);
                } else {
                    synth.note_off(ev.note as i32);
                }
                if res.n_notes < res.notes.len() {
                    res.notes[res.n_notes] = NoteLog { t_recv: ev.t_recv, t_out: 0, on: ev.on, note: ev.note, vel: ev.vel, src: ev.src };
                    if npend < 64 {
                        pending[npend] = res.n_notes;
                        npend += 1;
                    }
                    res.n_notes += 1;
                }
            }
        }
        // input
        let input = if cap.is_some() {
            match &ibuf {
                Buf::S32(s) => {
                    for i in 0..p {
                        il[i] = s[2 * i] as f32 * (1.0 / 2147483648.0);
                        ir[i] = s[2 * i + 1] as f32 * (1.0 / 2147483648.0);
                    }
                }
                Buf::S16(s) => {
                    for i in 0..p {
                        il[i] = s[2 * i] as f32 * (1.0 / 32768.0);
                        ir[i] = s[2 * i + 1] as f32 * (1.0 / 32768.0);
                    }
                }
            }
            Some((&il[..], &ir[..]))
        } else {
            None
        };
        synth.render(&mut l, &mut r, input);
        match &mut obuf {
            Buf::S32(d) => {
                for i in 0..p {
                    d[2 * i] = ((l[i] * gain).clamp(-1.0, 1.0) * 2147483520.0) as i32;
                    d[2 * i + 1] = ((r[i] * gain).clamp(-1.0, 1.0) * 2147483520.0) as i32;
                }
            }
            Buf::S16(d) => {
                for i in 0..p {
                    d[2 * i] = ((l[i] * gain).clamp(-1.0, 1.0) * 32767.0) as i16;
                    d[2 * i + 1] = ((r[i] * gain).clamp(-1.0, 1.0) * 32767.0) as i16;
                }
            }
        }
        for &x in l.iter() {
            hist[hist_pos] = x;
            hist_pos = (hist_pos + 1) % W;
        }
        unsafe {
            let sc = tb.back();
            for x in 0..W {
                sc.wave[x] = hist[(hist_pos + x) % W];
            }
            sc.active = synth.active_mask();
            sc.period = res.periods_run;
            tb.publish();
        }
        let t1 = now_ns();

        let delay = match play.h.delay() {
            Ok(d) => d,
            Err(_) => {
                res.xrun_play += 1;
                restart(&res);
                continue;
            }
        };
        let t_out = now_ns() + (delay as f64 * 1e9 / c.rate as f64) as i64;
        for &k in &pending[..npend] {
            res.notes[k].t_out = t_out;
        }
        if obuf.write(&play.h).is_err() {
            res.xrun_play += 1;
            restart(&res);
            continue;
        }
        if idx < cap_periods {
            res.dsp_us[idx] = (t1 - t0) as f32 / 1000.0;
            res.delay_frames[idx] = delay as f32;
            idx += 1;
        }
        res.periods_run += 1;
    }
    stop();
    res.thread_cpu_s = (thread_cpu_ns() - cpu_start) as f64 / 1e9;
    res.wall_s = (now_ns() - t_start) as f64 / 1e9;
    res.dsp_us.truncate(idx);
    res.delay_frames.truncate(idx);
    let _ = play.h.drop();
    res.ok = true;
    res
}

// ---------------------------------------------------------------- MIDI

fn midi_thread(ring: &Ring) -> String {
    let s = match seq::Seq::open(None, Some(Direction::Capture), true) {
        Ok(s) => s,
        Err(_) => return "no sequencer".into(),
    };
    let _ = s.set_client_name(&CString::new("op1og-spike-rust").unwrap());
    let port = s
        .create_simple_port(
            &CString::new("in").unwrap(),
            PortCap::WRITE | PortCap::SUBS_WRITE,
            PortType::MIDI_GENERIC | PortType::APPLICATION,
        )
        .unwrap_or(0);
    let me = s.client_id().unwrap_or(-1);
    let mut subscribed = 0;
    for ci in seq::ClientIter::new(&s) {
        let cl = ci.get_client();
        if cl == 0 || cl == me {
            continue;
        }
        if ci.get_name().map(|n| n.contains("Through")).unwrap_or(false) {
            continue;
        }
        for pi in seq::PortIter::new(&s, cl) {
            // hardware (USB/kernel) MIDI sources only; virtual senders connect to us themselves
            let caps = pi.get_capability();
            if caps.contains(PortCap::READ | PortCap::SUBS_READ)
                && !caps.contains(PortCap::NO_EXPORT)
                && pi.get_type().contains(PortType::HARDWARE)
            {
                if let Ok(sub) = PortSubscribe::empty() {
                    sub.set_sender(seq::Addr { client: cl, port: pi.get_port() });
                    sub.set_dest(seq::Addr { client: me, port });
                    if s.subscribe_port(&sub).is_ok() {
                        subscribed += 1;
                    }
                }
            }
        }
    }
    let info = format!("op1og-spike-rust {me}:{port}, {subscribed} hardware source(s)");
    eprintln!("midi: {info}");

    let mut fds = (&s, Some(Direction::Capture)).get().unwrap_or_default();
    let mut input = s.input();
    while running() {
        if alsa::poll::poll(&mut fds, 100).unwrap_or(0) == 0 {
            continue;
        }
        while let Ok(ev) = input.event_input() {
            let t = now_ns();
            let ty = ev.get_type();
            if ty == EventType::Noteon || ty == EventType::Noteoff {
                if let Some(n) = ev.get_data::<seq::EvNote>() {
                    let on = (ty == EventType::Noteon && n.velocity > 0) as u8;
                    ring.push(NoteEvent { t_recv: t, on, note: n.note, vel: n.velocity, src: 1 });
                }
            }
        }
    }
    info
}

// ---------------------------------------------------------------- UI

#[inline]
fn put(fb: &mut [u32], x: i32, y: i32, c: u32) {
    if x >= 0 && (x as usize) < W && y >= 0 && (y as usize) < H {
        fb[y as usize * W + x as usize] = c;
    }
}
fn line(fb: &mut [u32], mut x0: i32, mut y0: i32, x1: i32, y1: i32, c: u32) {
    let (dx, sx) = ((x1 - x0).abs(), if x0 < x1 { 1 } else { -1 });
    let (dy, sy) = (-(y1 - y0).abs(), if y0 < y1 { 1 } else { -1 });
    let mut e = dx + dy;
    loop {
        put(fb, x0, y0, c);
        if x0 == x1 && y0 == y1 {
            break;
        }
        let e2 = 2 * e;
        if e2 >= dy {
            e += dy;
            x0 += sx;
        }
        if e2 <= dx {
            e += dx;
            y0 += sy;
        }
    }
}
fn disc(fb: &mut [u32], cx: i32, cy: i32, rad: i32, c: u32) {
    for y in -rad..=rad {
        for x in -rad..=rad {
            if x * x + y * y <= rad * rad {
                put(fb, cx + x, cy + y, c);
            }
        }
    }
}
fn ring_outline(fb: &mut [u32], cx: i32, cy: i32, rad: i32, c: u32) {
    for y in -rad..=rad {
        for x in -rad..=rad {
            let d = x * x + y * y;
            if d <= rad * rad && d >= (rad - 2) * (rad - 2) {
                put(fb, cx + x, cy + y, c);
            }
        }
    }
}

fn draw(fb: &mut [u32], s: &Scope, frame: u64) {
    const DIAL: [u32; 4] = [0xFF2F6BFF, 0xFF1FCB5A, 0xFFF0F0F0, 0xFFFF6A1F];
    fb.fill(0xFF000000);
    for x in (0..W as i32).step_by(2) {
        put(fb, x, 70, 0xFF303030);
    }
    let mut py = 70;
    for x in 0..W as i32 {
        let y = 70 - (s.wave[x as usize].clamp(-1.0, 1.0) * 55.0) as i32;
        line(fb, if x > 0 { x - 1 } else { 0 }, py, x, y, 0xFFE0E0E0);
        py = y;
    }
    for i in 0..VOICES as i32 {
        disc(fb, 20 + i * 14, 10, 4, if (s.active >> i) & 1 != 0 { 0xFFFF6A1F } else { 0xFF303030 });
    }
    for (i, &col) in DIAL.iter().enumerate() {
        let (cx, cy) = (40 + i as i32 * 80, 136);
        ring_outline(fb, cx, cy, 16, col);
        let a = frame as f32 * 0.03 + i as f32;
        line(fb, cx, cy, cx + (a.cos() * 13.0) as i32, cy + (a.sin() * 13.0) as i32, col);
    }
}

fn key_note(k: SDL_Keycode) -> i32 {
    for (i, ch) in b"awsedftgyhujk".iter().enumerate() {
        if k.0 == *ch as u32 {
            return 60 + i as i32;
        }
    }
    -1
}

#[derive(Default)]
struct UiResult {
    ok: bool,
    err: String,
    driver: String,
    renderer: String,
    frame_cpu_ms: Vec<f32>,
    frame_interval_ms: Vec<f32>,
    shot_ok: bool,
}

fn cstr(p: *const std::ffi::c_char) -> String {
    if p.is_null() { "?".into() } else { unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned() }
}

fn run_ui(c: &Config, ui_ring: &Ring, tb: &TripleBuffer) -> UiResult {
    let mut res = UiResult::default();
    unsafe {
        if !SDL_Init(SDL_INIT_VIDEO) {
            res.err = cstr(SDL_GetError());
            return res;
        }
        let w = SDL_CreateWindow(c"op1-og spike (Rust)".as_ptr(), (W * 2) as i32, (H * 2) as i32, SDL_WindowFlags(0));
        let r = if w.is_null() { std::ptr::null_mut() } else { SDL_CreateRenderer(w, std::ptr::null()) };
        if r.is_null() {
            res.err = cstr(SDL_GetError());
            SDL_Quit();
            return res;
        }
        SDL_SetRenderVSync(r, 1);
        let tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W as i32, H as i32);
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
        res.driver = cstr(SDL_GetCurrentVideoDriver());
        res.renderer = cstr(SDL_GetRendererName(r));
        let mut fb = vec![0u32; W * H];
        res.frame_cpu_ms.reserve((c.duration * 240.0) as usize + 1000);
        res.frame_interval_ms.reserve((c.duration * 240.0) as usize + 1000);
        let shot = CString::new(c.shot.clone()).unwrap_or_default();
        let mut frame = 0u64;
        let mut last = 0i64;
        while running() {
            let t0 = now_ns();
            let mut e: SDL_Event = std::mem::zeroed();
            while SDL_PollEvent(&mut e) {
                let ty = e.r#type;
                if ty == SDL_EVENT_QUIT.0 || ty == SDL_EVENT_WINDOW_CLOSE_REQUESTED.0 {
                    stop();
                }
                if (ty == SDL_EVENT_KEY_DOWN.0 && !e.key.repeat) || ty == SDL_EVENT_KEY_UP.0 {
                    let n = key_note(e.key.key);
                    if n >= 0 {
                        ui_ring.push(NoteEvent { t_recv: now_ns(), on: (ty == SDL_EVENT_KEY_DOWN.0) as u8, note: n as u8, vel: 100, src: 2 });
                    }
                }
            }
            draw(&mut fb, tb.read(), frame);
            SDL_UpdateTexture(tex, std::ptr::null(), fb.as_ptr() as *const _, (W * 4) as i32);
            SDL_RenderClear(r);
            SDL_RenderTexture(r, tex, std::ptr::null(), std::ptr::null());
            if !c.shot.is_empty() && frame == 90 {
                let su = SDL_RenderReadPixels(r, std::ptr::null());
                if !su.is_null() {
                    res.shot_ok = SDL_SaveBMP(su, shot.as_ptr());
                    SDL_DestroySurface(su);
                }
            }
            let t1 = now_ns();
            SDL_RenderPresent(r);
            let t2 = now_ns();
            if frame > 0 {
                res.frame_cpu_ms.push((t1 - t0) as f32 / 1e6);
                res.frame_interval_ms.push((t2 - last) as f32 / 1e6);
            }
            last = t2;
            frame += 1;
        }
        SDL_DestroyTexture(tex);
        SDL_DestroyRenderer(r);
        SDL_DestroyWindow(w);
        SDL_Quit();
    }
    res.ok = true;
    res
}

// ---------------------------------------------------------------- main

fn parse() -> Option<Config> {
    let mut c = Config::default();
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        let mut next = || args.next().unwrap_or_default();
        match a.as_str() {
            "--device" => c.device = next(),
            "--period" => c.period = next().parse().unwrap_or(64),
            "--periods" => c.periods = next().parse().unwrap_or(2),
            "--rate" => c.rate = next().parse().unwrap_or(44100),
            "--duration" => c.duration = next().parse().unwrap_or(30.0),
            "--no-window" => c.window = false,
            "--no-capture" => c.capture = false,
            "--mute" => c.mute = true,
            "--rt" => c.rt = true,
            "--rt-prio" => c.rt_prio = next().parse().unwrap_or(20),
            "--json" => c.json = next(),
            "--notes" => c.notes = next(),
            "--shot" => c.shot = next(),
            "--bench-dsp" => c.bench = next().parse().unwrap_or(0.0),
            _ => {
                eprintln!("unknown option {a} (see spikes/README.md)");
                return None;
            }
        }
    }
    Some(c)
}

extern "C" fn on_signal(_: libc::c_int) {
    stop();
}

fn rusage() -> libc::rusage {
    let mut ru: libc::rusage = unsafe { std::mem::zeroed() };
    unsafe { libc::getrusage(libc::RUSAGE_SELF, &mut ru) };
    ru
}

fn main() {
    let Some(c) = parse() else { std::process::exit(64) };
    if c.bench > 0.0 {
        std::process::exit(run_bench(c.bench, c.rate));
    }
    unsafe {
        libc::signal(libc::SIGINT, on_signal as *const () as libc::sighandler_t);
        libc::signal(libc::SIGTERM, on_signal as *const () as libc::sighandler_t);
    }
    let midi_ring = Box::new(Ring::new());
    let ui_ring = Box::new(Ring::new());
    let tb = Box::new(TripleBuffer::new());
    let ru0 = rusage();
    let w0 = now_ns();

    let (ares, ures, midi_info) = std::thread::scope(|sc| {
        let mt = sc.spawn(|| midi_thread(&midi_ring));
        let at = sc.spawn(|| audio_thread(&c, &midi_ring, &ui_ring, &tb));
        let mut ures = UiResult::default();
        if c.window {
            ures = run_ui(&c, &ui_ring, &tb);
            if !ures.ok {
                eprintln!("ui: {} (continuing without a window)", ures.err);
            }
        }
        while running() {
            std::thread::sleep(std::time::Duration::from_millis(20));
        }
        let ares = at.join().unwrap();
        let mi = mt.join().unwrap();
        (ares, ures, mi)
    });

    let ru1 = rusage();
    let wall = (now_ns() - w0) as f64 / 1e9;
    let tv = |t: libc::timeval| t.tv_sec as f64 + t.tv_usec as f64 / 1e6;
    let cpu = tv(ru1.ru_utime) - tv(ru0.ru_utime) + tv(ru1.ru_stime) - tv(ru0.ru_stime);
    if !ares.ok {
        eprintln!("audio: {}", ares.err);
        std::process::exit(1);
    }
    let period_us = ares.period as f64 * 1e6 / c.rate as f64;
    let load: Vec<f32> = ares.dsp_us.iter().map(|&d| (d as f64 / period_us * 100.0) as f32).collect();
    let note_ms: Vec<f32> = ares.notes[..ares.n_notes]
        .iter()
        .filter(|n| n.src == 1 && n.on != 0 && n.t_out > 0)
        .map(|n| (n.t_out - n.t_recv) as f32 / 1e6)
        .collect();
    let delay = pct(ares.delay_frames.clone());
    let jb = |b: bool| if b { "true" } else { "false" };
    let mut js = format!(
        "{{\"lang\":\"rust\",\"device\":\"{}\",\"rate\":{},\"format\":\"{}\",\"period\":{},\"buffer\":{},\"fill_periods\":{},\
\"capture\":{},\"linked\":{},\"rt\":{},\"mute\":{},\"duration_s\":{:.2},\"periods_run\":{},\
\"xruns_play\":{},\"xruns_capture\":{},\"output_latency_ms\":{:.3},\"roundtrip_ms\":{:.3},\
\"process_cpu_pct\":{:.2},\"audio_thread_cpu_pct\":{:.2},\"max_rss_kb\":{},\"midi\":\"{}\",\
\"window\":{},\"video\":\"{}/{}\",\"shot\":{}",
        c.device, c.rate, ares.fmt, ares.period, ares.buffer, c.periods, jb(ares.capture), jb(ares.linked),
        jb(ares.rt), jb(c.mute), ares.wall_s, ares.periods_run, ares.xrun_play, ares.xrun_cap,
        delay.mean * 1000.0 / c.rate as f64,
        if ares.capture { (ares.period as f64 + delay.mean) * 1000.0 / c.rate as f64 } else { 0.0 },
        cpu / wall * 100.0, ares.thread_cpu_s / ares.wall_s * 100.0, ru1.ru_maxrss, midi_info, jb(ures.ok),
        ures.driver, ures.renderer, jb(ures.shot_ok)
    );
    js += &format!(",\"dsp_us\":{}", pct_json(&pct(ares.dsp_us.clone())));
    js += &format!(",\"dsp_load_pct\":{}", pct_json(&pct(load)));
    js += &format!(",\"delay_frames\":{}", pct_json(&delay));
    js += &format!(",\"note_to_dac_ms\":{}", pct_json(&pct(note_ms)));
    js += &format!(",\"frame_cpu_ms\":{}", pct_json(&pct(ures.frame_cpu_ms.clone())));
    js += &format!(",\"frame_interval_ms\":{}", pct_json(&pct(ures.frame_interval_ms.clone())));
    js += "}";
    println!("{js}");
    if !c.json.is_empty() {
        let _ = std::fs::write(&c.json, format!("{js}\n"));
    }
    if !c.notes.is_empty() {
        let mut s = String::from("i,on,note,vel,src,t_recv_ns,t_out_ns\n");
        for (i, n) in ares.notes[..ares.n_notes].iter().enumerate() {
            s += &format!("{},{},{},{},{},{},{}\n", i, n.on, n.note, n.vel, n.src, n.t_recv, n.t_out);
        }
        let _ = std::fs::write(&c.notes, s);
    }
}
