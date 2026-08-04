#pragma once

#include "Controls.h"

/**
    Which cell is showing, and when.

    ## The one idea

    **The frame is a pure function of (clock, parameters).** There is no
    "current frame" anywhere, nothing is advanced, and nothing accumulates.

    That is the fleet's usual invariant and it earns its place here for the
    usual reasons — the harness can render frame 3.25 cold, beat sync is a
    different clock rather than a second code path — but it earns it hardest for
    one that is specific to this plugin.

    **A sprite sheet has a right answer.** Twelve frames at twelve frames a
    second is one second of animation, and an operator who cues it against a
    twelve-frame bar expects the loop point to land on the bar line every time.
    A plugin that advanced a counter per rendered frame would run at whatever
    rate Resolume managed that night: fine in the preview, a quarter-frame short
    per bar once the show gets heavy, and visibly adrift by the end of a track.
    Closed-form, the loop point is arithmetic on the host's clock and cannot
    drift no matter what the frame rate does.

    ## Reset is a subtraction, not a state machine

    Once mode needs somewhere to start from, and the host clock does not restart
    when a clip is triggered. So the plugin keeps **one number**: the clock
    reading at the last Reset, subtracted from the clock here.

    That is state, and it is worth being honest that it is. It is admissible
    because it is a *base*, not an accumulator: it is written on an event and
    read otherwise, so it cannot creep, cannot depend on the frame rate, and
    still leaves any frame renderable on its own given the same base. An
    integrated position would have none of those properties.
*/
namespace flipbook
{
/// The clock the sequence runs on, in **frames** since the run began.
///
/// Free is seconds off the host clock times the rate. Beat and Bar count beats
/// or bars, recovered statelessly from the tempo and the position-in-bar the
/// host hands over -- the same recovery as orrery, downpour and tinsel: the
/// clock estimates how many bars have passed, `barPhase` gives the exact
/// position inside this one, and the whole number reconciling the two is
/// `round( estimate - barPhase )`. Continuous across the bar line, and exact
/// while the estimate is within half a bar.
///
/// Manual ignores every argument but `manualPhase` and `length`, and maps the
/// slider across exactly one pass of the run. It is the mode for keyframing
/// against an edit, and the only one where the OFX build behaves identically to
/// the FFGL build.
double FrameClock( double seconds, float bpm, float barPhase, SyncMode mode,
                   float rate, float manualPhase, int length );

/// Resolve a position in frames to an offset within the run: 0..length-1.
///
/// Ping-pong has period `2 * length - 2`, not `2 * length`, so the first and
/// last frames are held for one frame each like every other -- the naive
/// version holds both ends for two, which reads as a hitch at each turn and is
/// the single most common way a bounce loop looks wrong.
int FrameOffset( double framePos, int length, PlayMode mode );

/// The number of cells the run covers, given the operator's Start and Frame
/// Count against a sheet of `cells`. Frame Count 0 means "to the end of the
/// sheet", which is the default because it is what somebody who has just
/// pointed the plugin at a sheet wants, without having to count.
int RunLength( int start, int frameCount, int cells );

} // namespace flipbook
