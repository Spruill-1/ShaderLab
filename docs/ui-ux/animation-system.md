# Animation System

ShaderLab supports animated parameters and video sources:

- **`isAnimatable` flag**: ParameterDefinition marks parameters that auto-advance over time (e.g., Phase, Speed).
- **Play/Pause toolbar toggle**: Animation starts paused. Press Play to begin advancing animatable parameters.
- **Phase/Speed auto-advance**: Animatable float parameters increment each frame based on Speed.
- **Video sources**: Video source nodes participate in the animation timeline (below).

## Video playback

A Video Source node shows the frame for its `Time` property: the frame whose
`[start, start + duration)` contains it. `Time` is usually bound to a Clock;
a plain value is a still frame. Looping wraps `Time` into the clip; without
looping, a time past the end shows opaque black at the frame size (a cropped
flood, since a source with no image would stop the whole graph from
evaluating). Each render tick,
`SourceNodeFactory::TickAndUploadVideos` hands the time to
`VideoSourceProvider::PlayTo` and then uploads whatever frame is due, so video
follows the Clock at any tick rate: when ticks are slower than the clip, frames
are dropped, not slowed.

- **Decode-ahead queue.** A decode thread keeps up to 4 decoded frames queued
  ahead of the target time (`VideoSourceProvider::DecodeAheadFrames()`). The
  upload takes the newest queued frame that starts at or before the target and
  drops the older ones, so a slow tick or a slow decode does not stall playback.
  A zero-copy frame holds its decoder surface while queued; the H.264 decoder
  lets a caller hold 9-10 samples before `ReadSample` blocks, and
  `MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT` does not raise that (measured by
  `TestVideoDecoderSurfacePool`), so the queue plus the frame being converted
  (5) stays well under it.
- **Catching up.** A target that has moved past queued frames is reached by
  decoding forward, and samples that end before the target are discarded
  without being copied. A catch-up that decodes for more than 0.1 s without
  reaching the target delivers the frame it has reached (after a seek, only
  once past the seek target), so a long catch-up shows the picture moving
  instead of freezing.
- **Seeking.** Moving backward seeks (to the keyframe before, then decodes
  forward to the frame). Moving forward seeks only when the keyframe the seek
  would land on is further ahead of the decode head than
  `SeekThresholdFrames()`: the frames that decode in the time a seek's own
  overhead takes (`SeekOverheadSeconds()`, from `SetCurrentPosition` to the
  first sample, over `DecodeSecondsPerFrame()`, which is timed over runs of 8
  or more back-to-back reads after the first). Comparing the landing keyframe
  rather than the target is what makes the decision right for any keyframe
  interval: the decode up from the keyframe is paid either way. A seek lands
  on the latest keyframe at or before its target, and every seek teaches the
  provider one: the first sample after `SetCurrentPosition` is the keyframe,
  and nothing between it and the target is one. So a target inside such a
  known span lands exactly on its keyframe; otherwise the landing is estimated
  as the target minus the mean distance seeks have decoded through
  (`MeanSeekPrerollSeconds()`, 1 s until measured), but never inside a known
  span and never closer to the last known keyframe than the longest span seen.
  Media Foundation exposes no keyframe index for decoded output (every decoded
  sample is flagged a clean point), and asking a second, compressed-only
  reader where a seek would land took 5 ms on the small test clip and over
  100 ms on a 290 Mbps one, too slow for the render thread.
- **Seeking while the target moves.** The decode head never drops below a
  pending seek's target, and until the seek's frame is decoded another forward
  seek is made only for a keyframe known to lie further ahead. Without both,
  a Clock-bound video jumped into the middle of a long keyframe interval
  restarted its seek on every tick and froze (the reads up from the keyframe
  kept the head behind the moving target).
- **Read failures.** A failed read is retried once (the seek again, or a
  re-seek to the decode head that skips frames already read); a second
  failure stops decoding until the next seek.
- **Holding.** A `Time` that stands still (a paused Clock, a static value)
  keeps the frame on screen with no further uploads or seeks.
- **What re-renders downstream.** A video node counts as changed only when its
  image changes: a frame uploaded, past-the-end black entered or left, a new
  file. Its bound `Time` moving does not count, though it is still applied
  every tick (`ResolveSourceBindings`, then `TickAndUploadVideos`). So the
  nodes reading a 24 fps clip's image re-render 24 times a second at any tick
  rate, not once per tick (measured by `TestVideoDownstreamRedraws`: 60.3 to
  24.1 re-renders/s at 60 Hz ticks; a 60 fps clip stays at 60, a paused Clock
  at 0). Nodes bound to the video's analysis fields (`FrameTime`, `Position`,
  ...) are woken as before whenever its bindings moved, through the
  evaluator's separate fields-changed set, which reaches binding consumers but
  not image consumers. Other sources keep the general rule: a bound value
  that moves dirties the node.
- **Free running.** `VideoSourceProvider::Tick(delta)` advances its own
  position by wall time times `Speed()` and wraps at the end when looping;
  `RequestNextFrame()` steps one frame. Neither is used by the graph, which
  always drives video through `Time`.

`UploadedFrameTime()` (the `FrameTime` analysis field) is the start of the
frame the output holds; `CurrentPosition()` (the `Position` field) is that
frame's start, or a seek target until its frame arrives. Headless
`SettleVideoSources` waits on `FrameTime`.

Video timestamps from Media Foundation include the B-frame reordering delay
and ignore the file's edit list, so an H.264 clip with B-frames starts at two
frames, not 0. `FirstFrameTime()` reports it; earlier times show the first
frame. `Time` is not offset by it, so `Time` T shows the frame encoded at
T minus that delay, and the last frames of such a clip (two, for the test
clips) are never shown; this is open.

`Tests/VideoPlaybackTests.cpp` measures all of this in real time against
frame-indexed clips; see `TestVideoPlaybackRate`, `TestVideoPlaybackControl`,
`TestVideoSeekWhilePlaying` (scrubbing a running Clock, including a clip with
one keyframe in 10 s) and `TestVideoPastEndShowsBlack`.


---

Back to [docs/](../README.md) • [Repo root](../../README.md)